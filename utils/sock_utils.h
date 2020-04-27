/*
	KFMon: Kobo inotify-based launcher
	Copyright (C) 2016-2020 NiLuJe <ninuje@gmail.com>
	SPDX-License-Identifier: GPL-2.0-or-later

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// Small helpers related to socket handling for IPC

#include <errno.h>
#include <poll.h>
#include <stdlib.h>

// Check that we still have a remote to talk to over the socket.
// Returns ETIMEDOUT if it's not ready after <attempts> * <timeout>ms
//   Set timeout to -1 and attempts to > 0 to wait indefinitely.
// Returns EPIPE if remote has closed the connection
// Returns EXIT_SUCCESS if remote is ready for us
// Returns EXIT_FAILURE if poll failed unexpectedly, check errno
int can_write_to_socket(int data_fd, int timeout, size_t attempts);