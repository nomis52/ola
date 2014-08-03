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
 * AvahiHelper.cpp
 * Functions to help with reporting Avahi state.
 * Copyright (C) 2014 Simon Newton
 */

#include "tools/e133/AvahiHelper.h"

#include <string>

using std::string;

string ClientStateToString(AvahiClientState state) {
  switch (state) {
    case AVAHI_CLIENT_S_REGISTERING:
      return "AVAHI_CLIENT_S_REGISTERING";
    case AVAHI_CLIENT_S_RUNNING:
      return "AVAHI_CLIENT_S_RUNNING";
    case AVAHI_CLIENT_S_COLLISION:
      return "AVAHI_CLIENT_S_COLLISION";
    case AVAHI_CLIENT_FAILURE:
      return "AVAHI_CLIENT_FAILURE";
    case AVAHI_CLIENT_CONNECTING:
      return "AVAHI_CLIENT_CONNECTING";
    default:
      return "Unknown state";
  }
}

string GroupStateToString(AvahiEntryGroupState state) {
  switch (state) {
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
      return "AVAHI_ENTRY_GROUP_UNCOMMITED";
    case AVAHI_ENTRY_GROUP_REGISTERING:
      return "AVAHI_ENTRY_GROUP_REGISTERING";
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
      return "AVAHI_ENTRY_GROUP_ESTABLISHED";
    case AVAHI_ENTRY_GROUP_COLLISION:
      return "AVAHI_ENTRY_GROUP_COLLISION";
    case AVAHI_ENTRY_GROUP_FAILURE:
      return "AVAHI_ENTRY_GROUP_FAILURE";
    default:
      return "Unknown state";
  }
}

string BrowseEventToString(AvahiBrowserEvent state) {
  switch (state) {
    case AVAHI_BROWSER_NEW:
      return "AVAHI_BROWSER_NEW";
    case AVAHI_BROWSER_REMOVE:
      return "AVAHI_BROWSER_REMOVE";
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      return "AVAHI_BROWSER_CACHE_EXHAUSTED";
    case AVAHI_BROWSER_ALL_FOR_NOW:
      return "AVAHI_BROWSER_ALL_FOR_NOW";
    case AVAHI_BROWSER_FAILURE:
      return "AVAHI_BROWSER_FAILURE";
    default:
      return "Unknown event";
  }
}
