/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * AvahiOlaPoll.cpp
 * The glue between the AvahiPoll structure and OLA's SelectServer.
 * Copyright (C) 2014 Simon Newton
 */

#include "tools/e133/AvahiOlaPoll.h"

#include <ola/Callback.h>
#include <ola/Logging.h>
#include <ola/stl/STLUtils.h>
#include <ola/io/Descriptor.h>

#include <map>
#include <utility>

using ola::io::UnmanagedFileDescriptor;
using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;

// The Avahi data structures.
struct AvahiWatch {
  AvahiOlaPoll *poll;
  UnmanagedFileDescriptor *descriptor;

  AvahiWatchEvent registered_events;

  AvahiWatchCallback callback;
  void *userdata;
};

struct AvahiTimeout {
  AvahiOlaPoll *poll;
  ola::thread::timeout_id id;

  AvahiTimeoutCallback callback;
  void  *userdata;
};


// Static callbacks
//-----------------------------------------------------------------------------
static AvahiWatch* ola_watch_new(
    const AvahiPoll *api,
    int fd,
    AvahiWatchEvent event,
    AvahiWatchCallback callback,
    void *userdata) {
  AvahiOlaPoll *poll = reinterpret_cast<AvahiOlaPoll*>(api->userdata);
  return poll->WatchNew(fd, event, callback, userdata);
}

void ola_watch_new(AvahiWatch *watch) {
  watch->poll->WatchFree(watch);
}

static void ola_watch_free(AvahiWatch *watch) {
  watch->poll->WatchFree(watch);
}

static void ola_watch_update(AvahiWatch *watch, AvahiWatchEvent event) {
  watch->poll->WatchUpdate(watch, event);
}

static AvahiWatchEvent ola_watch_get_events(AvahiWatch *watch) {
  return watch->poll->WatchGetEvents(watch);
}

static AvahiTimeout* ola_timeout_new(
    const AvahiPoll *api,
    const struct timeval *tv,
    AvahiTimeoutCallback callback,
    void *userdata) {
  AvahiOlaPoll *poll = reinterpret_cast<AvahiOlaPoll*>(api->userdata);
  return poll->TimeoutNew(tv, callback, userdata);
}

static void ola_timeout_free(AvahiTimeout *timeout) {
  timeout->poll->TimeoutFree(timeout);
}

static void ola_timeout_update(AvahiTimeout *timeout,
                               const struct timeval *tv) {
  timeout->poll->TimeoutUpdate(timeout, tv);
}

static void ReadEvent(AvahiWatch *watch) {
  watch->callback(watch, watch->descriptor->ReadDescriptor(), AVAHI_WATCH_IN,
                  watch->userdata);
}

static void WriteEvent(AvahiWatch *watch) {
  watch->callback(watch, watch->descriptor->ReadDescriptor(), AVAHI_WATCH_OUT,
                  watch->userdata);
}

static void ExecuteTimeout(AvahiTimeout *timeout) {
  timeout->id = ola::thread::INVALID_TIMEOUT;
  timeout->callback(timeout, timeout->userdata);
}

// AvahiOlaPoll implementation
//-----------------------------------------------------------------------------
AvahiOlaPoll::AvahiOlaPoll(ola::io::SelectServerInterface *ss)
    : m_ss(ss) {
  m_poll.userdata = this;
  m_poll.watch_new = ola_watch_new;
  m_poll.watch_free = ola_watch_free;
  m_poll.watch_update = ola_watch_update;
  m_poll.watch_get_events = ola_watch_get_events;

  m_poll.timeout_new = ola_timeout_new;
  m_poll.timeout_free = ola_timeout_free;
  m_poll.timeout_update = ola_timeout_update;
}

AvahiOlaPoll::~AvahiOlaPoll() {
  if (!m_watch_map.empty()) {
    OLA_WARN << m_watch_map.size() << " entries remaining in Avahi WatchMap!";
    // It's hard to know what to do here, delete the remaining entries or not?
    // Either way we're probably going to crash.
  }
}

AvahiWatch* AvahiOlaPoll::WatchNew(
    int fd,
    AvahiWatchEvent event,
    AvahiWatchCallback callback,
    void *userdata) {
  std::pair<WatchMap::iterator, bool> p = m_watch_map.insert(
      WatchMap::value_type(fd, NULL));
  if (p.first->second) {
    OLA_WARN << "FD " << fd << " is already in the AvahiPoll watch map";
    return p.first->second;
  }

  p.first->second = new AvahiWatch;
  AvahiWatch *watch = p.first->second;

  watch->poll = this;
  watch->descriptor = new UnmanagedFileDescriptor(fd);
  watch->registered_events = event;
  watch->callback = callback;
  watch->userdata = userdata;

  watch->descriptor->SetOnData(NewCallback(&ReadEvent, watch));
  watch->descriptor->SetOnWritable(NewCallback(&WriteEvent, watch));

  // We cheat here. The only call to watch_new in Avahi passes the output
  // from dbus_watch_get_flags as the second argument. From the D-Bus docs this
  // never returns DBUS_WATCH_HANGUP or DBUS_WATCH_ERROR.
  if (event & AVAHI_WATCH_IN) {
    m_ss->AddReadDescriptor(watch->descriptor);
  }

  if (event & AVAHI_WATCH_OUT) {
    m_ss->AddWriteDescriptor(watch->descriptor);
  }

  if (event & AVAHI_WATCH_ERR || event & AVAHI_WATCH_HUP) {
    OLA_WARN << "Attempt to register with AVAHI_WATCH_ERR or AVAHI_WATCH_HUP: "
             << static_cast<int>(event);
  }

  return watch;
}

void AvahiOlaPoll::WatchFree(AvahiWatch *watch_ptr) {
  AvahiWatch *watch = ola::STLLookupAndRemovePtr(
      &m_watch_map,
      watch_ptr->descriptor->ReadDescriptor());

  if (watch->registered_events & AVAHI_WATCH_IN) {
    m_ss->RemoveReadDescriptor(watch->descriptor);
  }

  if (watch->registered_events & AVAHI_WATCH_OUT) {
    m_ss->RemoveWriteDescriptor(watch->descriptor);
  }

  delete watch->descriptor;
  delete watch;
}

void AvahiOlaPoll::WatchUpdate(AvahiWatch *watch, AvahiWatchEvent event) {
  // We cheat here. The only call to watch_update in Avahi passes the output
  // from dbus_watch_get_flags as the second argument. From the D-Bus docs this
  // never returns DBUS_WATCH_HANGUP or DBUS_WATCH_ERROR.

  if ((watch->registered_events & AVAHI_WATCH_IN) !=
      (event & AVAHI_WATCH_IN)) {
    if (watch->registered_events & AVAHI_WATCH_IN) {
      m_ss->RemoveReadDescriptor(watch->descriptor);
    } else {
      m_ss->AddReadDescriptor(watch->descriptor);
    }
  }

  if ((watch->registered_events & AVAHI_WATCH_OUT) !=
      (event & AVAHI_WATCH_OUT)) {
    if (watch->registered_events & AVAHI_WATCH_OUT) {
      m_ss->RemoveWriteDescriptor(watch->descriptor);
    } else {
      m_ss->AddWriteDescriptor(watch->descriptor);
    }
  }

  if (event & AVAHI_WATCH_ERR || event & AVAHI_WATCH_HUP) {
    OLA_WARN << "Attempt to update with AVAHI_WATCH_ERR or AVAHI_WATCH_HUP: "
             << static_cast<int>(event);
  }
  watch->registered_events = event;
}

AvahiWatchEvent AvahiOlaPoll::WatchGetEvents(AvahiWatch *watch) {
  // Not implemented.
  return static_cast<AvahiWatchEvent>(0);
  (void) watch;
}

AvahiTimeout* AvahiOlaPoll::TimeoutNew(
    const struct timeval *tv,
    AvahiTimeoutCallback callback,
    void *userdata) {
  AvahiTimeout *timeout = new AvahiTimeout();
  timeout->poll = this;
  timeout->callback = callback;
  timeout->userdata = userdata;

  if (tv) {
    TimeInterval delay(*tv);
    timeout->id = m_ss->RegisterSingleTimeout(
        delay, NewSingleCallback(ExecuteTimeout, timeout));
  }
  return timeout;
}

void AvahiOlaPoll::TimeoutFree(AvahiTimeout *timeout) {
  if (timeout->id != ola::thread::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(timeout->id);
  }
  delete timeout;
}

void AvahiOlaPoll::TimeoutUpdate(AvahiTimeout *timeout,
                            const struct timeval *tv) {
  if (timeout->id != ola::thread::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(timeout->id);
    timeout->id = ola::thread::INVALID_TIMEOUT;
  }
  if (!tv) {
    return;
  }

  TimeInterval delay(*tv);
  timeout->id = m_ss->RegisterSingleTimeout(
      delay, NewSingleCallback(ExecuteTimeout, timeout));
}
