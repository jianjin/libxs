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

#include "epoll.hpp"

#if defined XS_USE_ASYNC_EPOLL

#include <sys/epoll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <new>

#include "epoll.hpp"
#include "config.hpp"
#include "err.hpp"

xs::epoll_t::epoll_t (xs::ctx_t *ctx_, uint32_t tid_) :
    io_thread_t (ctx_, tid_),
    stopping (false)
{
    epoll_fd = epoll_create (1);
    errno_assert (epoll_fd != -1);
}

xs::epoll_t::~epoll_t ()
{
    //  Wait till the worker thread exits.
    thread_stop (&worker);

    close (epoll_fd);
    for (retired_t::iterator it = retired.begin (); it != retired.end (); ++it)
        delete *it;
}

xs::handle_t xs::epoll_t::add_fd (fd_t fd_, i_poll_events *events_)
{
    poll_entry_t *pe = new (std::nothrow) poll_entry_t;
    alloc_assert (pe);

    //  The memset is not actually needed. It's here to prevent debugging
    //  tools to complain about using uninitialised memory.
    memset (pe, 0, sizeof (poll_entry_t));

    pe->fd = fd_;
    pe->ev.events = 0;
    pe->ev.data.ptr = pe;
    pe->events = events_;

    int rc = epoll_ctl (epoll_fd, EPOLL_CTL_ADD, fd_, &pe->ev);
    errno_assert (rc != -1);

    //  Increase the load metric of the thread.
    adjust_load (1);

    return pe;
}

void xs::epoll_t::rm_fd (handle_t handle_)
{
    poll_entry_t *pe = (poll_entry_t*) handle_;
    int rc = epoll_ctl (epoll_fd, EPOLL_CTL_DEL, pe->fd, &pe->ev);
    errno_assert (rc != -1);
    pe->fd = retired_fd;
    retired.push_back (pe);

    //  Decrease the load metric of the thread.
    adjust_load (-1);
}

void xs::epoll_t::set_pollin (handle_t handle_)
{
    poll_entry_t *pe = (poll_entry_t*) handle_;
    pe->ev.events |= EPOLLIN;
    int rc = epoll_ctl (epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void xs::epoll_t::reset_pollin (handle_t handle_)
{
    poll_entry_t *pe = (poll_entry_t*) handle_;
    pe->ev.events &= ~((short) EPOLLIN);
    int rc = epoll_ctl (epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void xs::epoll_t::set_pollout (handle_t handle_)
{
    poll_entry_t *pe = (poll_entry_t*) handle_;
    pe->ev.events |= EPOLLOUT;
    int rc = epoll_ctl (epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void xs::epoll_t::reset_pollout (handle_t handle_)
{
    poll_entry_t *pe = (poll_entry_t*) handle_;
    pe->ev.events &= ~((short) EPOLLOUT);
    int rc = epoll_ctl (epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void xs::epoll_t::xstart ()
{
    thread_start (&worker, worker_routine, this);
}

void xs::epoll_t::xstop ()
{
    stopping = true;
}

void xs::epoll_t::loop ()
{
    epoll_event ev_buf [max_io_events];

    while (!stopping) {

        //  Execute any due timers.
        int timeout = (int) execute_timers ();

        //  Wait for events.
        int n = epoll_wait (epoll_fd, &ev_buf [0], max_io_events,
            timeout ? timeout : -1);
        if (n == -1 && errno == EINTR)
            continue;
        errno_assert (n != -1);

        for (int i = 0; i < n; i ++) {
            poll_entry_t *pe = ((poll_entry_t*) ev_buf [i].data.ptr);

            if (pe->fd == retired_fd)
                continue;
            if (ev_buf [i].events & (EPOLLERR | EPOLLHUP))
                pe->events->in_event (pe->fd);
            if (pe->fd == retired_fd)
               continue;
            if (ev_buf [i].events & EPOLLOUT)
                pe->events->out_event (pe->fd);
            if (pe->fd == retired_fd)
                continue;
            if (ev_buf [i].events & EPOLLIN)
                pe->events->in_event (pe->fd);
        }

        //  Destroy retired event sources.
        for (retired_t::iterator it = retired.begin (); it != retired.end ();
              ++it)
            delete *it;
        retired.clear ();
    }
}

void xs::epoll_t::worker_routine (void *arg_)
{
    ((epoll_t*) arg_)->loop ();
}

#endif
