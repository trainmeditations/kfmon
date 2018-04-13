/*
	KFMon: Kobo inotify-based launcher
	Copyright (C) 2016-2018 NiLuJe <ninuje@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "kfmon.h"

// Because daemon() only appeared in glibc 2.21
static int daemonize(void)
{
	int fd;

	switch (fork()) {
		case -1:
			return -1;
		case 0:
			break;
		default:
			_exit(0);
	}

	if (setsid() == -1) {
		return -1;
	}

	// Double fork, for... reasons!
	signal(SIGHUP, SIG_IGN);
	switch (fork()) {
		case -1:
			return -1;
		case 0:
			break;
		default:
			_exit(0);
	}

	if (chdir("/") == -1) {
		return -1;
	}

	umask(0);

	// Store a copy of stdin, stdout & stderr so we can restore it to our children later on...
	orig_stdin = dup(fileno(stdin));
	orig_stdout = dup(fileno(stdout));
	orig_stderr = dup(fileno(stderr));

	// Redirect stdin & stdout to /dev/null
	if ((fd = open("/dev/null", O_RDWR)) != -1) {
		dup2(fd, fileno(stdin));
		dup2(fd, fileno(stdout));
		if (fd > 2 + 3) {
			close(fd);
		}
	} else {
		fprintf(stderr, "Failed to redirect stdin & stdout to /dev/null\n");
		return -1;
	}

	// Redirect stderr to our logfile
	int flags = O_WRONLY | O_CREAT | O_APPEND;
	// Check if we need to truncate our log because it has grown too much...
	struct stat st;
	if ((stat(KFMON_LOGFILE, &st) == 0) && (S_ISREG(st.st_mode))) {
		// Truncate if > 1MB
		if (st.st_size > 1*1024*1024) {
			flags |= O_TRUNC;
		}
	}
	if ((fd = open(KFMON_LOGFILE, flags, 0600)) != -1) {
		dup2(fd, fileno(stderr));
		if (fd > 2 + 3) {
			close(fd);
		}
	} else {
		fprintf(stderr, "Failed to redirect stderr to logfile '%s'\n", KFMON_LOGFILE);
		return -1;
	}

	return 0;
}

// Return the current time formatted as 2016-04-29 @ 20:44:13 (used for logging)
char *get_current_time(void)
{
	// cf. strftime(3) & https://stackoverflow.com/questions/7411301
	time_t t;
	struct tm *lt;

	t = time(NULL);
	lt = localtime(&t);

	// Needs to be static to avoid dealing with painful memory handling...
	static char sz_time[22];
	strftime(sz_time, sizeof(sz_time), "%Y-%m-%d @ %H:%M:%S", lt);

	return sz_time;
}

// Check that our target mountpoint is indeed mounted...
static bool is_target_mounted(void)
{
	// cf. http://program-nix.blogspot.fr/2008/08/c-language-check-filesystem-is-mounted.html
	FILE *mtab = NULL;
	struct mntent *part = NULL;
	bool is_mounted = false;

	if ((mtab = setmntent("/proc/mounts", "r")) != NULL) {
		while ((part = getmntent(mtab)) != NULL) {
			DBGLOG("Checking fs %s mounted on %s", part->mnt_fsname, part->mnt_dir);
			if ((part->mnt_dir != NULL) && (strcmp(part->mnt_dir, KFMON_TARGET_MOUNTPOINT)) == 0) {
				is_mounted = true;
				break;
			}
		}
		endmntent(mtab);
	}

	return is_mounted;
}

// Monitor mountpoint activity...
static void wait_for_target_mountpoint(void)
{
	// cf. https://stackoverflow.com/questions/5070801
	int mfd = open("/proc/mounts", O_RDONLY, 0);
	struct pollfd pfd;

	unsigned int changes = 0;
	pfd.fd = mfd;
	pfd.events = POLLERR | POLLPRI;
	pfd.revents = 0;
	while (poll(&pfd, 1, -1) >= 0) {
		if (pfd.revents & POLLERR) {
			LOG("Mountpoints changed (iteration nr. %d)", changes++);

			// Stop polling once we know our mountpoint is available...
			if (is_target_mounted()) {
				LOG("Yay! Target mountpoint is available!");
				break;
			}
		}
		pfd.revents = 0;

		// If we can't find our mountpoint after that many changes, assume we're screwed...
		if (changes > 15) {
			LOG("Too many mountpoint changes without finding our target. Going buh-bye!");
			exit(EXIT_FAILURE);
		}
	}
}

// Handle parsing the main KFMon config
static int daemon_handler(void *user, const char *section, const char *key, const char *value) {
	DaemonConfig *pconfig = (DaemonConfig *)user;

	#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(key, n) == 0
	if (MATCH("daemon", "db_timeout")) {
		pconfig->db_timeout = atoi(value);
	} else if (MATCH("daemon", "use_syslog")) {
		pconfig->use_syslog = atoi(value);
	} else {
		return 0;	// unknown section/name, error
	}
	return 1;
}

// Handle parsing a watch config
static int watch_handler(void *user, const char *section, const char *key, const char *value) {
	WatchConfig *pconfig = (WatchConfig *)user;

	#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(key, n) == 0
	// NOTE: Crappy strncpy() usage, but those char arrays are zeroed first (hence the MAX-1 len to ensure that we're NULL terminated)...
	if (MATCH("watch", "filename")) {
		strncpy(pconfig->filename, value, PATH_MAX-1);
	} else if (MATCH("watch", "action")) {
		strncpy(pconfig->action, value, PATH_MAX-1);
	} else if (MATCH("watch", "do_db_update")) {
		pconfig->do_db_update = atoi(value);
	} else if (MATCH("watch", "skip_db_checks")) {
		pconfig->skip_db_checks = atoi(value);
	} else if (MATCH("watch", "db_title")) {
		strncpy(pconfig->db_title, value, DB_SZ_MAX-1);
	} else if (MATCH("watch", "db_author")) {
		strncpy(pconfig->db_author, value, DB_SZ_MAX-1);
	} else if (MATCH("watch", "db_comment")) {
		strncpy(pconfig->db_comment, value, DB_SZ_MAX-1);
	} else {
		return 0;	// unknown section/name, error
	}
	return 1;
}

// Load our config files...
static int load_config() {
	// Our config files live in the target mountpoint...
	if (!is_target_mounted()) {
		LOG("%s isn't mounted, waiting for it to be . . .", KFMON_TARGET_MOUNTPOINT);
		// If it's not, wait for it to be...
		wait_for_target_mountpoint();
	}

	// Walk the config directory to pickup our ini files... (c.f., https://keramida.wordpress.com/2009/07/05/fts3-or-avoiding-to-reinvent-the-wheel/)
	FTS *ftsp;
	FTSENT *p, *chp;
	// We only need to walk a single directory...
	char *const cfg_path[] = {KFMON_CONFIGPATH, NULL};
	int rval = 0;

	// Don't chdir (because that mountpoint can go buh-bye), and don't stat (because we don't need to).
	if ((ftsp = fts_open(cfg_path, FTS_COMFOLLOW | FTS_LOGICAL | FTS_NOCHDIR | FTS_NOSTAT | FTS_XDEV, NULL)) == NULL) {
		perror("[KFMon] fts_open");
		return -1;
	}
	// Initialize ftsp with as many toplevel entries as possible.
	chp = fts_children(ftsp, 0);
	if (chp == NULL) {
		// No files to traverse!
		LOG("Config directory '%s' appears to be empty, aborting!", KFMON_CONFIGPATH);
		fts_close(ftsp);
		return -1;
	}
	while ((p = fts_read(ftsp)) != NULL) {
		switch (p->fts_info) {
			case FTS_F:
				// Check if it's a .ini and not a Mac resource fork...
				if (strncasecmp(p->fts_name+(p->fts_namelen-4), ".ini", 4) == 0 && strncasecmp(p->fts_name, "._", 2) != 0) {
					LOG("Trying to load config file '%s' . . .", p->fts_path);
					// The main config has to be parsed slightly differently...
					if (strncasecmp(p->fts_name, "kfmon.ini", 4) == 0) {
						if (ini_parse(p->fts_path, daemon_handler, &daemon_config) < 0) {
							LOG("Failed to parse main config file '%s'!", p->fts_name);
							// Flag as a failure...
							rval = -1;
						}
						LOG("Daemon config loaded from '%s': db_timeout=%d, use_syslog=%d", p->fts_name, daemon_config.db_timeout, daemon_config.use_syslog);
					} else {
						// NOTE: Don't blow up when trying to store more watches than we have space for...
						if (watch_count >= WATCH_MAX) {
							LOG("We've already setup the maximum amount of watches we can handle (%d), discarding '%s'!", WATCH_MAX, p->fts_name);
							// Don't flag this as a hard failure, just warn and go on...
							break;
						}

						if (ini_parse(p->fts_path, watch_handler, &watch_config[watch_count]) < 0) {
							LOG("Failed to parse watch config file '%s'!", p->fts_name);
							// Flag as a failure...
							rval = -1;
						}
						LOG("Watch config @ index %zd loaded from '%s': filename=%s, action=%s, do_db_update=%d, db_title=%s, db_author=%s, db_comment=%s",
							watch_count,
							p->fts_name,
							watch_config[watch_count].filename,
							watch_config[watch_count].action,
							watch_config[watch_count].do_db_update,
							watch_config[watch_count].db_title,
							watch_config[watch_count].db_author,
							watch_config[watch_count].db_comment
						);
						// Switch to the next slot!
						watch_count++;
					}
				}
				break;
			default:
				break;
		}
	}
	fts_close(ftsp);

#ifdef DEBUG
	// Let's recap...
	DBGLOG("Daemon config recap: db_timeout=%d, use_syslog=%d", daemon_config.db_timeout, daemon_config.use_syslog);
	for (unsigned int watch_idx = 0; watch_idx < watch_count; watch_idx++) {
		DBGLOG("Watch config @ index %d recap: filename=%s, action=%s, do_db_update=%d, skip_db_checks=%d, db_title=%s, db_author=%s, db_comment=%s",
			watch_idx,
			watch_config[watch_idx].filename,
			watch_config[watch_idx].action,
			watch_config[watch_idx].do_db_update,
			watch_config[watch_idx].skip_db_checks,
			watch_config[watch_idx].db_title,
			watch_config[watch_idx].db_author,
			watch_config[watch_idx].db_comment
		);
	}
#endif

	return rval;
}

// Implementation of Qt4's QtHash (cf. qhash @ https://github.com/kovidgoyal/calibre/blob/master/src/calibre/devices/kobo/driver.py#L37)
static unsigned int qhash(const unsigned char *bytes, size_t length) {
	unsigned int h = 0;
	unsigned int i;

	for(i = 0; i < length; i++) {
		h = (h << 4) + bytes[i];
		h ^= (h & 0xf0000000) >> 23;
		h &= 0x0fffffff;
	}

	return h;
}

// Check if our target file has been processed by Nickel...
static bool is_target_processed(unsigned int watch_idx, bool wait_for_db)
{
	sqlite3 *db;
	sqlite3_stmt * stmt;
	int rc;
	int idx;
	bool is_processed = false;
	bool needs_update = false;

#ifdef DEBUG
	// Bypass DB checks on demand for debugging purposes...
	if(watch_config[watch_idx].skip_db_checks)
		return true;
#endif

	// Did the user want to try to update the DB for this icon?
	bool update = watch_config[watch_idx].do_db_update;

	if (update) {
		CALL_SQLITE(open_v2(KOBO_DB_PATH , &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL));
	} else {
		// Open the DB ro to be extra-safe...
		CALL_SQLITE(open_v2(KOBO_DB_PATH , &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL));
	}

	// Wait at most for Nms on OPEN & N*2ms on CLOSE if we ever hit a locked database during any of our proceedings...
	// NOTE: The defaults timings (steps of 500ms) appear to work reasonably well on my H2O with a 50MB Nickel DB... (i.e., it trips on OPEN when Nickel is moderately busy, but if everything's quiet, we're good).
	//	 Time will tell if that's a good middle-ground or not ;). This is user configurable in kfmon.ini (db_timeout key).
	sqlite3_busy_timeout(db, daemon_config.db_timeout * (wait_for_db + 1));
	DBGLOG("SQLite busy timeout set to %dms", daemon_config.db_timeout * (wait_for_db + 1));

	// NOTE: ContentType 6 should mean a book on pretty much anything since FW 1.9.17 (and why a book? Because Nickel currently identifies single PNGs as application/x-cbz, bless its cute little bytes).
	CALL_SQLITE(prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM content WHERE ContentID = @id AND ContentType = '6');", -1, &stmt, NULL));

	// Append the proper URI scheme to our icon path...
	char book_path[PATH_MAX+7];
	snprintf(book_path, PATH_MAX+7, "file://%s", watch_config[watch_idx].filename);

	idx = sqlite3_bind_parameter_index(stmt, "@id");
	CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		DBGLOG("SELECT SQL query returned: %d", sqlite3_column_int(stmt, 0));
		if (sqlite3_column_int(stmt, 0) == 1) {
			is_processed = true;
		}
	}

	sqlite3_finalize(stmt);

	// Now that we know the book exists, we also want to check if the thumbnails do... to avoid getting triggered from the thumbnail creation.
	// NOTE: Again, this assumes FW >= 2.9.0
	if (is_processed) {
		// Assume they haven't been processed until we can confirm it...
		is_processed = false;

		// We'll need the ImageID first...
		CALL_SQLITE(prepare_v2(db, "SELECT ImageID FROM content WHERE ContentID = @id AND ContentType = '6';", -1, &stmt, NULL));

		idx = sqlite3_bind_parameter_index(stmt, "@id");
		CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
			DBGLOG("SELECT SQL query returned: %s", sqlite3_column_text(stmt, 0));
			const unsigned char *image_id = sqlite3_column_text(stmt, 0);
			size_t len = (size_t)sqlite3_column_bytes(stmt, 0);

			// Then we need the proper hashes Nickel devises...
			// cf. images_path @ https://github.com/kovidgoyal/calibre/blob/master/src/calibre/devices/kobo/driver.py#L2489
			unsigned int hash = qhash(image_id, len);
			unsigned int dir1 = hash & (0xff * 1);
			unsigned int dir2 = (hash & (0xff00 * 1)) >> 8;

			char images_path[PATH_MAX];
			snprintf(images_path, PATH_MAX, "%s/.kobo-images/%d/%d", KFMON_TARGET_MOUNTPOINT, dir1, dir2);
			DBGLOG("Checking for thumbnails in '%s' . . .", images_path);

			// Count the number of processed thumbnails we find...
			unsigned int thumbnails_num = 0;

			// Start with the full-size screensaver...
			char ss_path[PATH_MAX];
			snprintf(ss_path, PATH_MAX, "%s/%s - N3_FULL.parsed", images_path, image_id);
			if (access(ss_path, F_OK) == 0) {
				thumbnails_num++;
			} else {
				LOG("Full-size screensaver hasn't been parsed yet!");
			}

			// Then the Homescreen tile...
			// NOTE: This one might be a tad confusing...
			//	 If the icon has never been processed, this will only happen the first time we *close* the PNG's "book"... (i.e., the moment it pops up as the 'last opened' tile).
			//	 And *that* processing triggers a set of OPEN & CLOSE, meaning we can quite possibly run on book *exit* that first time (and only that first time), if database locking permits...
			char tile_path[PATH_MAX];
			snprintf(tile_path, PATH_MAX, "%s/%s - N3_LIBRARY_FULL.parsed", images_path, image_id);
			if (access(tile_path, F_OK) == 0) {
				thumbnails_num++;
			} else {
				LOG("Homescreen tile hasn't been parsed yet!");
			}

			// And finally the Library thumbnail...
			char thumb_path[PATH_MAX];
			snprintf(thumb_path, PATH_MAX, "%s/%s - N3_LIBRARY_GRID.parsed", images_path, image_id);
			if (access(thumb_path, F_OK) == 0) {
				thumbnails_num++;
			} else {
				LOG("Library thumbnail hasn't been parsed yet!");
			}

			// Only give a greenlight if we got all three!
			if (thumbnails_num == 3) {
				is_processed = true;
			}
		}

		sqlite3_finalize(stmt);
	}

	// NOTE: Here be dragons! This works in theory, but risks confusing Nickel's handling of the DB if we do that when nickel is running (which we are).
	// Because doing it with Nickel running is a potentially terrible idea, for various reasons (c.f., https://www.sqlite.org/howtocorrupt.html for the gory details, some of which probably even apply here! :p).
	// As such, we leave enabling this option to the user's responsibility. KOReader ships with it disabled.
	// The idea is to, optionally, update the Title, Author & Comment fields to make them more useful...
	if (is_processed && update) {
		// Check if the DB has already been updated by checking the title...
		CALL_SQLITE(prepare_v2(db, "SELECT Title FROM content WHERE ContentID = @id AND ContentType = '6';", -1, &stmt, NULL));

		idx = sqlite3_bind_parameter_index(stmt, "@id");
		CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
			DBGLOG("SELECT SQL query returned: %s", sqlite3_column_text(stmt, 0));
			if (strcmp((const char *)sqlite3_column_text(stmt, 0), watch_config[watch_idx].db_title) != 0) {
				needs_update = true;
			}
		}

		sqlite3_finalize(stmt);
	}
	if (needs_update) {
		CALL_SQLITE(prepare_v2(db, "UPDATE content SET Title = @title, Attribution = @author, Description = @comment WHERE ContentID = @id AND ContentType = '6';", -1, &stmt, NULL));

		// NOTE: No sanity checks are done to confirm that those watch configs are sane... The example config ships with a strong warning not to forget them if wanted, but that's it.
		idx = sqlite3_bind_parameter_index(stmt, "@title");
		CALL_SQLITE(bind_text(stmt, idx, watch_config[watch_idx].db_title, -1, SQLITE_STATIC));
		idx = sqlite3_bind_parameter_index(stmt, "@author");
		CALL_SQLITE(bind_text(stmt, idx, watch_config[watch_idx].db_author, -1, SQLITE_STATIC));
		idx = sqlite3_bind_parameter_index(stmt, "@comment");
		CALL_SQLITE(bind_text(stmt, idx, watch_config[watch_idx].db_comment, -1, SQLITE_STATIC));
		idx = sqlite3_bind_parameter_index(stmt, "@id");
		CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			LOG("UPDATE SQL query failed: %s", sqlite3_errmsg(db));
		} else {
			LOG("Successfully updated DB data for the target PNG");
		}

		sqlite3_finalize(stmt);
	}

	// A rather crappy check to wait for pending COMMITs...
	if (is_processed && wait_for_db) {
		// If there's a rollback journal for the DB, wait for it to go away...
		// NOTE: This assumes the DB was opened with the default journal_mode, DELETE
		//       This doesn't appear to be the case anymore, on FW 4.7.x (and possibly earlier, I haven't looked at this stuff in quite a while), it's now using WAL (which makes sense).
		unsigned int count = 0;
		while (access(KOBO_DB_PATH"-journal", F_OK) == 0) {
			LOG("Found a SQLite rollback journal, waiting for it to go away (iteration nr. %d) . . .", count++);
			usleep(250 * 1000);
			// NOTE: Don't wait more than 10s
			if (count > 40) {
				LOG("Waited for the SQLite rollback journal to go away for far too long, going on anyway.");
				break;
			}
		}
	}

	sqlite3_close(db);

	return is_processed;
}

// Heavily inspired from https://stackoverflow.com/a/35235950
// Initializes the process table. -1 means the entry in the table is available.
static void init_process_table(void) {
	for (unsigned int i = 0; i < WATCH_MAX; i++) {
		PT.spawn_pids[i] = -1;
		PT.spawn_fds[i].fd = -1;
		PT.spawn_fds[i].events = POLLIN;
		PT.spawn_watchids[i] = -1;
	}
}

// Returns the index of the next available entry in the process table.
static int get_next_available_pt_entry(void) {
	for (int i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_fds[i].fd == -1) {
			return i;
		}
	}
	return -1;
}

// Adds information about a new spawn to the process table.
static void add_process_to_table(int i, pid_t pid, int fd, unsigned int watch_idx) {
	PT.spawn_pids[i] = pid;
	PT.spawn_fds[i].fd = fd;
	PT.spawn_watchids[i] = (int) watch_idx;
}

// Removes information about a spawn from the process table.
static void remove_process_from_table(int i) {
	close(PT.spawn_fds[i].fd);
	PT.spawn_pids[i] = -1;
	PT.spawn_fds[i].fd = -1;
	PT.spawn_watchids[i] = -1;
}

/* Spawn a process and return its pid...
 * Initially inspired from popen2() implementations from https://stackoverflow.com/questions/548063
 * As well as the glibc's system() call,
 * With a bit of added tracking to handle reaping without a SIGCHLD handler.
 */
static pid_t spawn(char *const *command, unsigned int watch_idx)
{
	pid_t pid;

	// Create the pipe needed to handle reaping via polling during our main event loop
	int p[2];
	if (pipe(p) < 0) {
		perror("[KFMon] pipe");
		exit(EXIT_FAILURE);
	}

	pid = fork();

	if (pid < 0) {
		// Fork failed?
		perror("[KFMon] waitpid");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		// Sweet child o' mine!
		LOG("Spawned process %ld . . .", (long) getpid());
		// Close the read end of the pipe
		close(p[0]);
		// Do the whole stdin/stdout/stderr dance again to ensure that child process doesn't inherit our tweaked fds...
		dup2(orig_stdin, fileno(stdin));
		dup2(orig_stdout, fileno(stdout));
		dup2(orig_stderr, fileno(stderr));
		close(orig_stdin);
		close(orig_stdout);
		close(orig_stderr);
		// Restore signals
		signal(SIGHUP, SIG_DFL);
		// NOTE: We used to use execvpe when being launched from udev in order to sanitize all the crap we inherited from udev's env ;).
		//       Now, we actually rely on the specific env we inherit from rcS/on-animator!
		execvp(*command, command);
		// This will only ever be reached on error, hence the lack of actual return value check ;).
		perror("[KFMon] execvp");
		exit(EXIT_FAILURE);
	} else {
		// Parent
		// Close the write end of the pipe
		close(p[1]);
		// And keep track of the process
		int i = get_next_available_pt_entry();
		if (i < 0) {
			// NOTE: If we ever hit this error codepath, we don't have to worry about leaving that last spawn as a zombie:
			//       One of the benefits of the double-fork we do to daemonize is that, on our death, our children will get reparented to init,
			//       which, by design, will handle the reaping automatically.
			LOG("Failed to find an available entry in our process table for pid %ld!", (long) pid);
			exit(EXIT_FAILURE);
		} else {
			add_process_to_table(i, pid, p[0], watch_idx);
			DBGLOG("Assigned pid %ld (from watch idx %d and with pipefd %d) to process table entry idx %d", (long) pid, watch_idx, p[0], i);
		}
	}

	return pid;
}

// Reap and cleans up any dead child processes from the process table.
static void reap_zombie_processes(void) {
	// NOTE: For *some* scripts, the whole "detecting the termination of the child process via the closed pipe" somehow doesn't take.
	//        As a dirty workaround, we try to manually reap our currently registered spawns on *every* batch of inotify events...
	//        ... but do so with a twist: use WNOHANG to avoid blocking.
	//       This *may* be HW/FW specific (or at least I hope), but I only have an H2O to test...
	for (int i = 1; i < WATCH_MAX; i++) {
		if (PT.spawn_pids[i] != -1) {
			LOG("Forcefully trying to reap process %ld ...", (long) PT.spawn_pids[i]);
			pid_t ret;
			int wstatus;
			ret = waitpid(PT.spawn_pids[i], &wstatus, WNOHANG);
			// Recap what happened to it
			if (ret < 0) {
				perror("[KFMon] waitpid");
				exit(EXIT_FAILURE);
			} else if (ret == PT.spawn_pids[i]) {
				if (WIFEXITED(wstatus)) {
					LOG("Reaped zombie process %d: It exited with status %d.", PT.spawn_pids[i], WEXITSTATUS(wstatus));
				} else if (WIFSIGNALED(wstatus)) {
					LOG("Reaped zombie process %d: It was killed by signal %d (%s).", PT.spawn_pids[i], WTERMSIG(wstatus), strsignal(WTERMSIG(wstatus)));
				}
				// And now we can safely remove it from the process table
				remove_process_from_table(i);
			} else {
				LOG("... process %ld is still alive.", (long) PT.spawn_pids[i]);
			}
		}
	}
}

// Check if a given inotify watch already has a spawn running
static bool is_watch_already_spawned(unsigned int watch_idx)
{
	// Walk our process table to see if the given watch currently has a registered running process
	for (unsigned int i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_watchids[i] == (int) watch_idx) {
			return true;
			// NOTE: Assume everything's peachy, and we'll never end up with the same watch_idx assigned to multiple indices in the process table.
			//       Good news: That assumption seems to hold true so far :).
		}
	}

	return false;
}

// Return the pid of the spawn of a given inotify watch
static pid_t get_spawn_pid_for_watch(unsigned int watch_idx) {
	for (unsigned int i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_watchids[i] == (int) watch_idx) {
			return PT.spawn_pids[i];
		}
	}

	return -1;
}

// Read all available inotify events from the file descriptor 'fd'.
static bool handle_events(int fd)
{
	/* Some systems cannot read integer variables if they are not
	   properly aligned. On other systems, incorrect alignment may
	   decrease performance. Hence, the buffer used for reading from
	   the inotify file descriptor should have the same alignment as
	   struct inotify_event. */
	char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *event;
	ssize_t len;
	char *ptr;
	bool destroyed_wd = false;
	bool was_unmounted = false;
	static bool pending_processing = false;

	// Loop while events can be read from inotify file descriptor.
	for (;;) {
		// Read some events.
		len = read(fd, buf, sizeof buf);
		if (len == -1 && errno != EAGAIN) {
			perror("[KFMon] read");
			exit(EXIT_FAILURE);
		}

		/* If the nonblocking read() found no events to read, then
		   it returns -1 with errno set to EAGAIN. In that case,
		   we exit the loop. */
		if (len <= 0) {
			break;
		}

		// Loop over all events in the buffer
		for (ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
			// NOTE: This trips -Wcast-align on ARM, but it works, and saves us some code ;).
			event = (const struct inotify_event *) ptr;

			// Identify which of our target file we've caught an event for...
			unsigned int watch_idx = 0;
			bool found_watch_idx = false;
			for (watch_idx = 0; watch_idx < watch_count; watch_idx++) {
				if (watch_config[watch_idx].inotify_wd == event->wd) {
					found_watch_idx = true;
					break;
				}
			}
			if (!found_watch_idx) {
				// NOTE: Err, that should (hopefully) never happen!
				LOG("!! Failed to match the current inotify event to any of our watched file! !!");
			}

			// Print event type
			if (event->mask & IN_OPEN) {
				LOG("Tripped IN_OPEN for %s", watch_config[watch_idx].filename);
				// Clunky detection of potential Nickel processing...
				if (!is_watch_already_spawned(watch_idx)) {
					// Only check if we're ready to spawn something...
					if (!is_target_processed(watch_idx, false)) {
						// It's not processed on OPEN, flag as pending...
						pending_processing = true;
						LOG("Flagged target icon '%s' as pending processing ...", watch_config[watch_idx].filename);
					} else {
						// It's already processed, we're good!
						pending_processing = false;
					}
				}
			}
			if (event->mask & IN_CLOSE) {
				LOG("Tripped IN_CLOSE for %s", watch_config[watch_idx].filename);
				// NOTE: Make sure we won't run a specific command multiple times while an earlier instance of it is still running...
				//       This is mostly of interest for KOReader/Plato: it means we can keep KFMon running while they're up, without risking
				//       trying to spawn multiple instances of them in case they end up tripping their own inotify watch ;).
				if (!is_watch_already_spawned(watch_idx)) {
					// Check that our target file has already fully been processed by Nickel before launching anything...
					if (!pending_processing && is_target_processed(watch_idx, true)) {
						LOG("Spawning %s . . .", watch_config[watch_idx].action);
						// We're using execvp()...
						char *const cmd[] = {watch_config[watch_idx].action, NULL};
						spawn(cmd, watch_idx);
					} else {
						LOG("Target icon '%s' might not have been fully processed by Nickel yet, don't launch anything.", watch_config[watch_idx].filename);
						// NOTE: That, or we hit a SQLITE_BUSY timeout on OPEN, which tripped our 'pending processing' check.
					}
				} else {
					LOG("Our last spawn (%ld) is still alive!", (long) get_spawn_pid_for_watch(watch_idx));
				}
			}
			if (event->mask & IN_UNMOUNT) {
				LOG("Tripped IN_UNMOUNT for %s", watch_config[watch_idx].filename);
				// Remember that we encountered an unmount, so we don't try to manually remove watches that are already gone...
				was_unmounted = true;
			}
			if (event->mask & IN_IGNORED) {
				LOG("Tripped IN_IGNORED for %s", watch_config[watch_idx].filename);
				// Remember that the watch was automatically destroyed so we can break from the loop...
				destroyed_wd = true;
				watch_config[watch_idx].wd_was_destroyed = true;
			}
			if (event->mask & IN_Q_OVERFLOW) {
				if (event->len) {
					LOG("Huh oh... Tripped IN_Q_OVERFLOW for %s", event->name);
				} else {
					LOG("Huh oh... Tripped IN_Q_OVERFLOW for... something?");
				}
				// Try to remove the inotify watch we matched (... hoping matching actually was succesful), and break the loop.
				LOG("Trying to remove inotify watch for '%s' @ index %d.", watch_config[watch_idx].filename, watch_idx);
				if (inotify_rm_watch(fd, watch_config[watch_idx].inotify_wd) == -1) {
					// That's too bad, but may not be fatal, so warn only...
					perror("[KFMon] inotify_rm_watch");
				}
				destroyed_wd = true;
				watch_config[watch_idx].wd_was_destroyed = true;
			}
		}

		// If we caught an event indicating that a watch was automatically destroyed, break the loop.
		if (destroyed_wd) {
			// But before we do that, make sure we've removed *all* our *other* watches first (again, hoping matching was successful), since we'll be setting them up all again later...
			for (unsigned int watch_idx = 0; watch_idx < watch_count; watch_idx++) {
				if (!watch_config[watch_idx].wd_was_destroyed) {
					// Don't do anything if that was because of an unmount... Because that assures us that everything's gone (since by design, we're sure all our target files live on the same mountpoint), even if we didn't get to parse all the events in one go to flag them as destroyed one by one.
					if (!was_unmounted) {
						// Log what we're doing...
						LOG("Trying to remove inotify watch for '%s' @ index %d.", watch_config[watch_idx].filename, watch_idx);
						if (inotify_rm_watch(fd, watch_config[watch_idx].inotify_wd) == -1) {
							// That's too bad, but may not be fatal, so warn only...
							perror("[KFMon] inotify_rm_watch");
						}
					}
				} else {
					// Reset the flag to avoid false-positives on the next iteration of the loop, since we re-use the array's content.
					watch_config[watch_idx].wd_was_destroyed = false;
				}
			}
			break;
		}
	}

	// And we have another outer loop to break, so pass that on...
	return destroyed_wd;
}

int main(int argc __attribute__ ((unused)), char* argv[] __attribute__ ((unused)))
{
	int fd, poll_num;

	// Being launched via udev leaves us with a negative nice value, fix that.
	if (setpriority(PRIO_PROCESS, 0, 0) == -1) {
		perror("[KFMon] setpriority");
		exit(EXIT_FAILURE);
	}

	// Fly, little daemon!
	if (daemonize() != 0) {
		fprintf(stderr, "Failed to daemonize!\n");
		exit(EXIT_FAILURE);
	}

	// Say hello :)
	LOG("Initializing KFMon %s | Built on %s @ %s | Using SQLite %s (built against version %s)", KFMON_VERSION,  __DATE__, __TIME__, sqlite3_libversion(), SQLITE_VERSION);

	// Load our configs
	if (load_config() == -1) {
		LOG("Failed to load one or more config files!");
		exit(EXIT_FAILURE);
	}

	// Squish stderr if we want to log to the syslog... (can't do that w/ the rest in daemonize, since we don't have our config yet at that point)
	if (daemon_config.use_syslog) {
		// Redirect stderr (which is now actually our log file) to /dev/null
		if ((fd = open("/dev/null", O_RDWR)) != -1) {
			dup2(fd, fileno(stderr));
			if (fd > 2 + 3) {
				close(fd);
			}
		} else {
			fprintf(stderr, "Failed to redirect stderr to /dev/null\n");
			return -1;
		}

		// And connect to the system logger...
		openlog("kfmon", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);
	}

	// Initialize the process table, to track our spawns
	init_process_table();

	// We pretty much want to loop forever...
	while (1) {
		LOG("Beginning the main loop.");

		// Create the file descriptor for accessing the inotify API
		LOG("Initializing inotify.");
		fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (fd == -1) {
			perror("[KFMon] inotify_init1");
			exit(EXIT_FAILURE);
		}

		// Make sure our target file is available (i.e., the partition it resides in is mounted)
		if (!is_target_mounted()) {
			LOG("%s isn't mounted, waiting for it to be . . .", KFMON_TARGET_MOUNTPOINT);
			// If it's not, wait for it to be...
			wait_for_target_mountpoint();
		}

		// Flag each of our target files for 'file was opened' and 'file was closed' events
		for (unsigned int watch_idx = 0; watch_idx < watch_count; watch_idx++) {
			watch_config[watch_idx].inotify_wd = inotify_add_watch(fd, watch_config[watch_idx].filename, IN_OPEN | IN_CLOSE);
			if (watch_config[watch_idx].inotify_wd == -1) {
				LOG("Cannot watch '%s'! Giving up.", watch_config[watch_idx].filename);
				perror("[KFMon] inotify_add_watch");
				exit(EXIT_FAILURE);
				// NOTE: This effectively means we exit when any one of our target file cannot be found, which is not a bad thing, per se...
				//	 This basically means that it takes some kind of effort to actually be running during Nickel's processing of said target file ;).
			}
			LOG("Setup an inotify watch for '%s' @ index %d.", watch_config[watch_idx].filename, watch_idx);
		}

		// NOTE: Always hijack the first entry of the process table for our inotify input fd...
		//       This is not terribly neat, but it simplifies my life a bit during the polling...
		PT.spawn_fds[0].fd = fd;
		PT.spawn_fds[0].events = POLLIN;

		// Wait for events (both from inotify, and spawned processes terminations)
		LOG("Listening for events.");
		while (1) {
			poll_num = poll(PT.spawn_fds, WATCH_MAX, -1);
			if (poll_num == -1) {
				if (errno == EINTR) {
					continue;
				}
				perror("[KFMon] poll");
				exit(EXIT_FAILURE);
			}

			if (poll_num > 0) {
				// Loop over our process table to check if any of the pipes have closed
				for (int i = 1; i < WATCH_MAX; i++) {
					// NOTE: Unfortunately, what happens when the remote end of a pipe is closed is implementation dependent. It can be a combination of POLLHUP, POLLIN, or both.
					//       c.f., http://www.greenend.org.uk/rjk/tech/poll.html
					//       Try to cover everything just to be safe...
					if (PT.spawn_fds[i].revents & (POLLIN | POLLHUP)) {
						LOG(". . . Reaping process %ld", (long) PT.spawn_pids[i]);
						pid_t ret;
						int wstatus;
						// Wait for our child process to terminate, retrying on EINTR
						do {
							ret = waitpid(PT.spawn_pids[i], &wstatus, 0);
						} while (ret == -1 && errno == EINTR);
						// Recap what happened to it
						if (ret != PT.spawn_pids[i]) {
							perror("[KFMon] waitpid");
							exit(EXIT_FAILURE);
						} else {
							if (WIFEXITED(wstatus)) {
								LOG("Reaped process %d: It exited with status %d.", PT.spawn_pids[i], WEXITSTATUS(wstatus));
							} else if (WIFSIGNALED(wstatus)) {
								LOG("Reaped process %d: It was killed by signal %d (%s).", PT.spawn_pids[i], WTERMSIG(wstatus), strsignal(WTERMSIG(wstatus)));
							}
						}
						// And now we can safely remove it from the process table
						remove_process_from_table(i);
					}
				}
				// And finally handle our inotify events
				if (PT.spawn_fds[0].revents & POLLIN) {
					// Try to reap potentially stale zombie processes before processing the inotify events (without blocking).
					// FIXME: This shouldn't be needed, because the whole pipe polling dance should do the trick just fine, but there you have it...
					reap_zombie_processes();
					// Inotify events are available
					if (handle_events(fd)) {
						// Go back to the main loop if we exited early (because a watch was destroyed automatically after an unmount or an unlink, for instance)
						break;
					}
				}
			}
		}
		LOG("Stopped listening for events.");

		// Close inotify file descriptor
		close(fd);
	}

	if (daemon_config.use_syslog) {
		closelog();
	}

	exit(EXIT_SUCCESS);
}
