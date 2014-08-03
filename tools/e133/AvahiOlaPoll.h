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
 * AvahiOlaPoll.h
 * The glue between the AvahiPoll structure and OLA's SelectServer.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_AVAHIOLAPOLL_H_
#define TOOLS_E133_AVAHIOLAPOLL_H_

#include <avahi-common/watch.h>

#include <ola/io/Descriptor.h>
#include <ola/io/SelectServerInterface.h>
#include <ola/thread/SchedulerInterface.h>

#include <map>

// The OLA implementation of an AvahiPoll.
//-----------------------------------------------------------------------------
class AvahiOlaPoll {
 public:
  explicit AvahiOlaPoll(ola::io::SelectServerInterface *ss);
  ~AvahiOlaPoll();

  const AvahiPoll* GetPoll() const {
    return &m_poll;
  }

  AvahiWatch* WatchNew(
    int fd,
    AvahiWatchEvent event,
    AvahiWatchCallback callback,
    void *userdata);
  void WatchFree(AvahiWatch *watch);
  void WatchUpdate(AvahiWatch *watch, AvahiWatchEvent event);
  AvahiWatchEvent WatchGetEvents(AvahiWatch *watch);

  AvahiTimeout* TimeoutNew(
    const struct timeval *tv,
    AvahiTimeoutCallback callback,
    void *userdata);

  void TimeoutFree(AvahiTimeout *timeout);

  void TimeoutUpdate(AvahiTimeout *timeout, const struct timeval *tv);

 private:
  // TODO(simon): we don't need a map here, just a set.
  typedef std::map<int, AvahiWatch*> WatchMap;

  ola::io::SelectServerInterface *m_ss;
  AvahiPoll m_poll;
  WatchMap m_watch_map;
};
#endif  // TOOLS_E133_AVAHIOLAPOLL_H_
