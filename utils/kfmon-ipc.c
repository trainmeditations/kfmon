/*
	KFMon: Kobo inotify-based launcher
	Copyright (C) 2016-2022 NiLuJe <ninuje@gmail.com>
	SPDX-License-Identifier: GPL-3.0-or-later

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// Small client that sends stdin to the KFMon IPC socket and prints the replies.
// Replies are always sent to stdout, stderr is used for errors and 'UI'
// (i.e., in a script, you'll generally want to discard stderr).

// Because we're pretty much Linux-bound ;).
#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "../openssh/atomicio.h"
#include "../str5/str5.h"
#include <errno.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Path to KFMon's IPC Unix socket
#define KFMON_IPC_SOCKET "/tmp/kfmon-ipc.ctl"

// Drain stdin and send it to the IPC socket (caller aborts on false)
static bool
    handle_stdin(int data_fd)
{
	// Check how many bytes we need to drain
	int bytes = 0;
	if (ioctl(fileno(stdin), FIONREAD, &bytes) == -1) {
		fprintf(stderr, "[%s] Aborting: ioctl: %m!\n", __PRETTY_FUNCTION__);
		exit(EXIT_FAILURE);
	}

	// If there's nothing to read, abort.
	// That includes a user-triggered End-of-Transmission (i.e., ^D), which flags stdin w/ POLLIN
	if (bytes == 0) {
		return false;
	}

	// Eh, recycle PIPE_BUF, it should be more than enough for our needs.
	char buf[PIPE_BUF] = { 0 };

	// Now that we know how much to read, do it!
	ssize_t len = read_in_full(fileno(stdin), buf, (size_t) bytes);
	if (len < 0) {
		// Only actual failures are left, xread handles the rest
		fprintf(stderr, "[%s] Aborting: read: %m!\n", __PRETTY_FUNCTION__);
		exit(EXIT_FAILURE);
	}

	// If there's actually nothing to read (EoF), abort.
	if (len == 0) {
		return false;
	}

	// Send it over the socket (w/ NUL, and without an LF)
	size_t packet_len = (size_t) bytes;
	if (buf[bytes - 1] == '\n') {
		buf[bytes - 1] = '\0';
	} else {
		// Don't blow past the buffer in case bytes == sizeof(buf).
		if ((size_t) bytes < sizeof(buf)) {
			// buf is zero-initialized, we're good, just send one byte more, it's going to be NUL already.
			//buf[bytes] = '\0';
			packet_len++;
		} else {
			// Otherwise, just truncate to NUL-terminate
			buf[bytes - 1] = '\0';
		}
	}

	// Do it, being careful to handle EPIPE sanely
	if (send_in_full(data_fd, buf, packet_len) < 0) {
		// Only actual failures are left, so we're pretty much done
		if (errno == EPIPE) {
			fprintf(stderr, "KFMon closed the connection!\n");
		} else {
			fprintf(stderr, "[%s] Aborting: send: %m!\n", __PRETTY_FUNCTION__);
		}
		exit(EXIT_FAILURE);
	}

	// Done
	return true;
}

// Handle replies from the IPC socket (caller aborts on false)
static bool
    handle_reply(int data_fd)
{
	// Eh, recycle PIPE_BUF, it should be more than enough for our needs.
	char buf[PIPE_BUF] = { 0 };

	// We don't actually know the size of the reply, so, best effort here.
	ssize_t len = xread(data_fd, buf, sizeof(buf) - 1U);
	if (len < 0) {
		// Only actual failures are left, xread handles the rest
		fprintf(stderr, "[%s] Aborting: read: %m!\n", __PRETTY_FUNCTION__);
		exit(EXIT_FAILURE);
	}

	// If there's actually nothing to read (EoF), abort.
	if (len == 0) {
		return false;
	}

	// Then print it!
	fprintf(stderr, "<<< Got a reply:\n");
	fprintf(stdout, "%.*s", (int) len, buf);

	// NOTE: We *could* try to detect warning/error codes in the reply,
	//       but that's arguably a job better left to whatever's using us ;).

	// Back to sending...
	fprintf(stderr, ">>> ");

	// Done
	return true;
}

// Main entry point
// NOTE: While I'd ideally want to be able to detect early if KFMon is already busy handling another IPC connection,
//       the socket's listen backlog is inflated by the kernel, so connect() won't fail w/ EAGAIN any time soon.
//       As for an initial POLLOUT check on the connected socket, it'll would happily go through immediately.
//       So, I'm left with detecting delays in KFMon's *reply*, and making sure everybody handles connections
//       dropped early sanely (i.e., send w/ MSG_NOSIGNAL, and a sane handling of EPIPE).
//       c.f., utils/sock_utils.h for more details about that conundrum.
// NOTE: This means, that, yes, KFMon replying to a command is a mandatory part of the "protocol" ;).
int
    main(void)
{
	// Setup the local socket
	int data_fd = -1;
	if ((data_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) == -1) {
		fprintf(stderr, "Failed to create local IPC socket (socket: %m), aborting!\n");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_un sock_name = { 0 };
	sock_name.sun_family         = AF_UNIX;
	str5cpy(sock_name.sun_path, sizeof(sock_name.sun_path), KFMON_IPC_SOCKET, sizeof(KFMON_IPC_SOCKET), TRUNC);

	// Connect to IPC socket, retrying safely on EINTR (c.f., http://www.madore.org/~david/computers/connect-intr.html)
	while (connect(data_fd, (const struct sockaddr*) &sock_name, sizeof(sock_name)) == -1 && errno != EISCONN) {
		if (errno != EINTR) {
			fprintf(stderr, "KFMon IPC is down (connect: %m), aborting!\n");
			exit(EXIT_FAILURE);
		}
	}

	// Assume everything's peachy until shit happens...
	int rc = EXIT_SUCCESS;

	// Now that KFMon is ready for us, cheap-ass prompt is cheap!
	fprintf(stderr, ">>> ");

	// We'll be polling both stdin and the socket...
	nfds_t        nfds    = 2;
	struct pollfd pfds[2] = { 0 };

	// stdin
	pfds[0].fd     = fileno(stdin);
	pfds[0].events = POLLIN;
	// Data socket
	pfds[1].fd     = data_fd;
	pfds[1].events = POLLIN;

	// Chat with hot sockets in your area!
	while (1) {
		int poll_num = poll(pfds, nfds, -1);
		if (poll_num == -1) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "[%s] Aborting: poll: %m!\n", __PRETTY_FUNCTION__);
			rc = EXIT_FAILURE;
			goto cleanup;
		}

		if (poll_num > 0) {
			// There's potentially data to be read in stdin
			if (pfds[0].revents & POLLIN) {
				if (!handle_stdin(data_fd)) {
					// There wasn't actually any data left in stdin
					fprintf(stderr, "No more data in stdin!\n");
					// Don't flag that as an error, sending an EoT is a perfectly sane way to, well,
					// end the transmission ;).
					goto cleanup;
				}
				// If it was also closed (i.e., it's a pipe), go back to poll to check for replies now.
				if (pfds[0].revents & POLLHUP) {
					continue;
				}
			}

			if (pfds[1].revents & POLLIN) {
				// There was a reply from the socket
				if (!handle_reply(data_fd)) {
					// If the remote closed the connection, we get POLLIN|POLLHUP w/ EoF ;).
					if (pfds[1].revents & POLLHUP) {
						fprintf(stderr, "KFMon closed the connection!\n");
						// Flag that as an error
						rc = EPIPE;
					} else {
						// There wasn't actually any data!
						fprintf(stderr, "Nothing more to read!\n");
						// Flag that as an error
						rc = ENODATA;
					}
					goto cleanup;
				}
			}

			// stdin was closed
			if (pfds[0].revents & POLLHUP) {
				fprintf(stderr, "stdin was closed!\n");
				// Much like earlier, don't flag that as an error.
				goto cleanup;
			}
			// Remote closed the connection
			if (pfds[1].revents & POLLHUP) {
				fprintf(stderr, "KFMon closed the connection!\n");
				// Flag that as an error
				rc = EPIPE;
				goto cleanup;
			}
		}
	}

cleanup:
	// Bye now!
	close(data_fd);

	return rc;
}
