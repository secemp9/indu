/* ncdu - NCurses Disk Usage

  Copyright (c) 2007-2024 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include "dir_cache_lock.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Maximum age in seconds for a lock to be considered stale */
#define STALE_LOCK_THRESHOLD 300

/* Initial retry delay in microseconds (10ms) */
#define INITIAL_RETRY_DELAY_US 10000

/* Maximum retry delay in microseconds (500ms) */
#define MAX_RETRY_DELAY_US 500000

/* Lock file path */
static char *lock_file_path = NULL;

/* Lock file descriptor (-1 if not held) */
static int lock_fd = -1;

/* Current lock mode */
static cache_lock_mode current_mode;

/* Whether we currently hold a lock */
static int lock_held = 0;


/* Check if a process is still running */
static int process_alive(pid_t pid) {
    if (pid <= 0)
        return 0;
    /* kill with signal 0 tests if process exists without sending signal */
    return (kill(pid, 0) == 0 || errno == EPERM);
}


/* Read lock file contents and extract PID and timestamp */
static int read_lock_info(int fd, pid_t *pid_out, time_t *timestamp_out) {
    char buf[64];
    ssize_t n;
    long pid_val, ts_val;

    *pid_out = 0;
    *timestamp_out = 0;

    /* Seek to beginning */
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;

    buf[n] = '\0';

    /* Parse "PID TIMESTAMP\n" format */
    if (sscanf(buf, "%ld %ld", &pid_val, &ts_val) != 2)
        return -1;

    *pid_out = (pid_t)pid_val;
    *timestamp_out = (time_t)ts_val;

    return 0;
}


/* Write lock file contents with our PID and current timestamp */
static int write_lock_info(int fd) {
    char buf[64];
    int len;
    ssize_t written;

    /* Truncate file first */
    if (ftruncate(fd, 0) < 0)
        return -1;

    /* Seek to beginning */
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;

    len = snprintf(buf, sizeof(buf), "%ld %ld\n", (long)getpid(), (long)time(NULL));
    if (len < 0 || len >= (int)sizeof(buf))
        return -1;

    written = write(fd, buf, len);
    if (written != len)
        return -1;

    /* Ensure data is on disk */
    if (fsync(fd) < 0)
        return -1;

    return 0;
}


/* Check if the current lock holder is stale (dead process or old timestamp) */
static int is_lock_stale(int fd) {
    pid_t holder_pid;
    time_t holder_timestamp;
    time_t now;

    if (read_lock_info(fd, &holder_pid, &holder_timestamp) < 0) {
        /* Can't read lock info - consider it potentially stale */
        return 1;
    }

    /* Check if the holding process is still alive */
    if (!process_alive(holder_pid)) {
        return 1;
    }

    /* Check if the lock is too old (process might be stuck) */
    now = time(NULL);
    if (holder_timestamp > 0 && now - holder_timestamp > STALE_LOCK_THRESHOLD) {
        return 1;
    }

    return 0;
}


/* Attempt to acquire the flock on the lock file descriptor */
static int try_flock(int fd, cache_lock_mode mode, int blocking) {
    int operation;

    if (mode == CACHE_LOCK_SHARED)
        operation = LOCK_SH;
    else
        operation = LOCK_EX;

    if (!blocking)
        operation |= LOCK_NB;

    return flock(fd, operation);
}


/* Initialize the lock subsystem */
int cache_lock_init(const char *cache_path) {
    size_t path_len;

    /* Clean up any existing state */
    cache_lock_cleanup();

    if (!cache_path)
        return -1;

    /* Create lock file path: cache_path + ".lock" */
    path_len = strlen(cache_path);
    lock_file_path = xmalloc(path_len + 6);
    strcpy(lock_file_path, cache_path);
    strcat(lock_file_path, ".lock");

    return 0;
}


/* Acquire a lock with timeout */
int cache_lock_acquire(cache_lock_mode mode, int timeout_sec) {
    int fd;
    int ret;
    time_t start_time;
    time_t elapsed;
    unsigned int retry_delay;
    int first_attempt;
    struct timespec ts;

    if (!lock_file_path)
        return -1;

    /* Already holding a lock? */
    if (lock_held) {
        /* If we already have an exclusive lock, we can satisfy any request */
        if (current_mode == CACHE_LOCK_EXCLUSIVE)
            return 0;
        /* If we have shared and want shared, that's fine */
        if (mode == CACHE_LOCK_SHARED)
            return 0;
        /* Upgrading shared to exclusive requires release first */
        cache_lock_release();
    }

    /* Open or create the lock file */
    fd = open(lock_file_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        /* Try to create parent directory structure doesn't exist case */
        return -1;
    }

    /* Set close-on-exec flag */
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    start_time = time(NULL);
    retry_delay = INITIAL_RETRY_DELAY_US;
    first_attempt = 1;

    while (1) {
        /* Try non-blocking acquire first */
        ret = try_flock(fd, mode, 0);

        if (ret == 0) {
            /* Got the lock */
            /* For exclusive locks, write our info */
            if (mode == CACHE_LOCK_EXCLUSIVE) {
                if (write_lock_info(fd) < 0) {
                    flock(fd, LOCK_UN);
                    close(fd);
                    return -1;
                }
            }

            lock_fd = fd;
            current_mode = mode;
            lock_held = 1;
            return 0;
        }

        /* Lock is held by someone else */
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            /* Some other error */
            close(fd);
            return -1;
        }

        /* Check for stale lock */
        if (first_attempt) {
            first_attempt = 0;
            if (is_lock_stale(fd)) {
                /* Try to forcibly take over a stale lock */
                /* First try to get exclusive access for cleanup */
                ret = try_flock(fd, CACHE_LOCK_EXCLUSIVE, 0);
                if (ret == 0) {
                    /* Got it - this means the stale process released between checks */
                    if (mode == CACHE_LOCK_EXCLUSIVE) {
                        if (write_lock_info(fd) < 0) {
                            flock(fd, LOCK_UN);
                            close(fd);
                            return -1;
                        }
                    } else {
                        /* Downgrade to shared if that's what was requested */
                        flock(fd, LOCK_UN);
                        ret = try_flock(fd, CACHE_LOCK_SHARED, 0);
                        if (ret < 0) {
                            close(fd);
                            return -1;
                        }
                    }
                    lock_fd = fd;
                    current_mode = mode;
                    lock_held = 1;
                    return 0;
                }
                /* Couldn't get it - someone else might have taken it */
            }
        }

        /* Check timeout */
        if (timeout_sec == 0) {
            /* Non-blocking mode - fail immediately */
            close(fd);
            return -1;
        }

        if (timeout_sec > 0) {
            elapsed = time(NULL) - start_time;
            if (elapsed >= timeout_sec) {
                /* Timeout expired */
                close(fd);
                return -1;
            }
        }

        /* Blocking mode (-1) or within timeout - sleep and retry */
        ts.tv_sec = retry_delay / 1000000;
        ts.tv_nsec = (retry_delay % 1000000) * 1000;
        nanosleep(&ts, NULL);

        /* Exponential backoff with cap */
        retry_delay *= 2;
        if (retry_delay > MAX_RETRY_DELAY_US)
            retry_delay = MAX_RETRY_DELAY_US;
    }
}


/* Release the current lock */
void cache_lock_release(void) {
    if (!lock_held || lock_fd < 0)
        return;

    /* Release the flock */
    flock(lock_fd, LOCK_UN);

    /* Close the file descriptor */
    close(lock_fd);

    lock_fd = -1;
    lock_held = 0;
}


/* Cleanup the lock subsystem */
void cache_lock_cleanup(void) {
    /* Release any held lock */
    cache_lock_release();

    /* Free the lock file path */
    if (lock_file_path) {
        free(lock_file_path);
        lock_file_path = NULL;
    }
}
