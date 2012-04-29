/*
    Copyright (c) 2009-2012 250bpm s.r.o.
    Copyright (c) 2007-2009 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of Crossroads I/O project.

    Crossroads I/O is free software; you can redistribute it and/or modify it
    under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Crossroads is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "platform.hpp"
#if defined XS_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <string.h>
#include <new>

#include "stream_engine.hpp"
#include "io_thread.hpp"
#include "session_base.hpp"
#include "config.hpp"
#include "err.hpp"
#include "ip.hpp"

xs::stream_engine_t::stream_engine_t (fd_t fd_, const options_t &options_) :
    s (fd_),
    inpos (NULL),
    insize (0),
    decoder (in_batch_size, options_.maxmsgsize),
    outpos (NULL),
    outsize (0),
    encoder (out_batch_size),
    session (NULL),
    leftover_session (NULL),
    options (options_),
    plugged (false),
    header_pos (in_header),
    header_remaining (sizeof in_header),
    header_received (false),
    header_sent (false)
{
    //  Fill in outgoing SP protocol header and the complementary (desired)
    //  header.
    if (!options.legacy_protocol) {
        sp_get_header (out_header, options.sp_pattern, options.sp_version,
            options.sp_role);
        sp_get_header (desired_header, options.sp_pattern, options.sp_version,
            options.sp_complement);
    }

    //  Get the socket into non-blocking mode.
    unblock_socket (s);

    //  Set the socket buffer limits for the underlying socket.
    if (options.sndbuf) {
        int rc = setsockopt (s, SOL_SOCKET, SO_SNDBUF,
            (char*) &options.sndbuf, sizeof (int));
#ifdef XS_HAVE_WINDOWS
		wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif
    }
    if (options.rcvbuf) {
        int rc = setsockopt (s, SOL_SOCKET, SO_RCVBUF,
            (char*) &options.rcvbuf, sizeof (int));
#ifdef XS_HAVE_WINDOWS
		wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif
    }

#ifdef SO_NOSIGPIPE
    //  Make sure that SIGPIPE signal is not generated when writing to a
    //  connection that was already closed by the peer.
    int set = 1;
    int rc = setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof (int));
    errno_assert (rc == 0);
#endif
}

xs::stream_engine_t::~stream_engine_t ()
{
    xs_assert (!plugged);

    if (s != retired_fd) {
#ifdef XS_HAVE_WINDOWS
		int rc = closesocket (s);
		wsa_assert (rc != SOCKET_ERROR);
#else
		int rc = close (s);
        errno_assert (rc == 0 || errno == ECONNRESET);
#endif
		s = retired_fd;
    }
}

void xs::stream_engine_t::plug (io_thread_t *io_thread_,
    session_base_t *session_)
{
    xs_assert (!plugged);
    plugged = true;
    leftover_session = NULL;

    //  Connect to session object.
    xs_assert (!session);
    xs_assert (session_);
    encoder.set_session (session_);
    decoder.set_session (session_);
    session = session_;

    //  Connect to the io_thread object.
    io_object_t::plug (io_thread_);
    handle = add_fd (s);
    set_pollin (handle);
    set_pollout (handle);

    //  Flush all the data that may have been already received downstream.
    in_event (s);
}

void xs::stream_engine_t::unplug ()
{
    xs_assert (plugged);
    plugged = false;

    //  Cancel all fd subscriptions.
    rm_fd (handle);

    //  Disconnect from the io_thread object.
    io_object_t::unplug ();

    //  Disconnect from session object.
    encoder.set_session (NULL);
    decoder.set_session (NULL);
    leftover_session = session;
    session = NULL;
}

void xs::stream_engine_t::terminate ()
{
    unplug ();
    delete this;
}

void xs::stream_engine_t::in_event (fd_t fd_)
{
    bool disconnection = false;

    //  If we have not yet received the full protocol header...
    if (unlikely (!options.legacy_protocol && !header_received)) {

        //  Read remaining header bytes.
        int hbytes = read (header_pos, header_remaining);

        //  Check whether the peer has closed the connection.
        if (hbytes == -1) {
            error ();
            return;
        }

        header_remaining -= hbytes;
        header_pos += hbytes;

        //  If we did not read the whole header, poll for more.
        if (header_remaining)
            return;

        //  If the protocol headers do not match, close the connection.
        if (memcmp (in_header, desired_header, sizeof in_header) != 0) {
            error ();
            return;
        }

        //  Done with protocol header; proceed to read data.
        header_received = true;
    }

    //  If there's no data to process in the buffer...
    if (!insize) {

        //  Retrieve the buffer and read as much data as possible.
        //  Note that buffer can be arbitrarily large. However, we assume
        //  the underlying TCP layer has fixed buffer size and thus the
        //  number of bytes read will be always limited.
        decoder.get_buffer (&inpos, &insize);
        insize = read (inpos, insize);

        //  Check whether the peer has closed the connection.
        if (insize == (size_t) -1) {
            insize = 0;
            disconnection = true;
        }
    }

    //  Push the data to the decoder.
    size_t processed = decoder.process_buffer (inpos, insize);

    if (unlikely (processed == (size_t) -1)) {
        disconnection = true;
    }
    else {

        //  Stop polling for input if we got stuck.
        if (processed < insize) {

            //  This may happen if queue limits are in effect.
            if (plugged)
                reset_pollin (handle);
        }

        //  Adjust the buffer.
        inpos += processed;
        insize -= processed;
    }

    //  Flush all messages the decoder may have produced.
    //  If IO handler has unplugged engine, flush transient IO handler.
    if (unlikely (!plugged)) {
        xs_assert (leftover_session);
        leftover_session->flush ();
    } else {
        session->flush ();
    }

    if (session && disconnection)
        error ();
}

void xs::stream_engine_t::out_event (fd_t fd_)
{
    bool more_data = true;

    //  If protocol header was not yet sent...
    if (unlikely (!options.legacy_protocol && !header_sent)) {
        int hbytes = write (out_header, sizeof out_header);

        //  It should always be possible to write the full protocol header to a
        //  freshly connected TCP socket. Therefore, if we get an error or
        //  partial write here the peer has disconnected.
        if (hbytes != sizeof out_header) {
            error ();
            return;
        }
        header_sent = true;
    }

    //  If write buffer is empty, try to read new data from the encoder.
    if (!outsize) {

        outpos = NULL;
        more_data = encoder.get_data (&outpos, &outsize);

        //  If IO handler has unplugged engine, flush transient IO handler.
        if (unlikely (!plugged)) {
            xs_assert (leftover_session);
            leftover_session->flush ();
            return;
        }

        //  If there is no data to send, stop polling for output.
        if (outsize == 0) {
            reset_pollout (handle);
            return;
        }
    }

    //  If there are any data to write in write buffer, write as much as
    //  possible to the socket. Note that amount of data to write can be
    //  arbitratily large. However, we assume that underlying TCP layer has
    //  limited transmission buffer and thus the actual number of bytes
    //  written should be reasonably modest.
    int nbytes = write (outpos, outsize);

    //  Handle problems with the connection.
    if (nbytes == -1) {
        error ();
        return;
    }

    outpos += nbytes;
    outsize -= nbytes;

    //  If the encoder reports that there are no more data to get from it
    //  we can stop polling for POLLOUT immediately.
    if (!more_data && !outsize)
        reset_pollout (handle);
}

void xs::stream_engine_t::activate_out ()
{
    set_pollout (handle);

    //  Speculative write: The assumption is that at the moment new message
    //  was sent by the user the socket is probably available for writing.
    //  Thus we try to write the data to socket avoiding polling for POLLOUT.
    //  Consequently, the latency should be better in request/reply scenarios.
    out_event (s);
}

void xs::stream_engine_t::activate_in ()
{
    set_pollin (handle);

    //  Speculative read.
    in_event (s);
}

void xs::stream_engine_t::error ()
{
    xs_assert (session);
    session->detach ();
    unplug ();
    delete this;
}

int xs::stream_engine_t::write (const void *data_, size_t size_)
{
#ifdef XS_HAVE_WINDOWS

    int nbytes = send (s, (char*) data_, (int) size_, 0);

    //  If not a single byte can be written to the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative write).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;
		
    //  Signalise peer failure.
    if (nbytes == -1 && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAEHOSTUNREACH ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);
    return (size_t) nbytes;

#else

#if defined MSG_NOSIGNAL
    ssize_t nbytes = send (s, data_, size_, MSG_NOSIGNAL);
#else
    ssize_t nbytes = send (s, data_, size_, 0);
#endif

    //  Several errors are OK. When speculative write is being done we may not
    //  be able to write a single byte from the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR))
        return 0;

    //  Signalise peer failure.
    if (nbytes == -1 && (errno == ECONNRESET || errno == EPIPE ||
          errno == ETIMEDOUT))
        return -1;

    errno_assert (nbytes != -1);
    return (size_t) nbytes;

#endif
}

int xs::stream_engine_t::read (void *data_, size_t size_)
{
#ifdef XS_HAVE_WINDOWS

    int nbytes = recv (s, (char*) data_, (int) size_, 0);

    //  If not a single byte can be read from the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative read).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;

    //  Connection failure.
    if (nbytes == -1 && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET ||
          WSAGetLastError () == WSAECONNREFUSED ||
          WSAGetLastError () == WSAENOTCONN))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);

    //  Orderly shutdown by the other peer.
    if (nbytes == 0)
        return -1; 

    return (size_t) nbytes;

#else

    ssize_t nbytes = recv (s, data_, size_, 0);

    //  Several errors are OK. When speculative read is being done we may not
    //  be able to read a single byte from the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR))
        return 0;

    //  Signalise peer failure.
    if (nbytes == -1 && (errno == ECONNRESET || errno == ECONNREFUSED ||
          errno == ETIMEDOUT || errno == EHOSTUNREACH || errno == ENOTCONN))
        return -1;

    errno_assert (nbytes != -1);

    //  Orderly shutdown by the peer.
    if (nbytes == 0)
        return -1;

    return (size_t) nbytes;

#endif
}

