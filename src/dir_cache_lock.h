/* indu - Incremental NCurses Disk Usage

  Based on ncdu by Yorhel (https://dev.yorhel.nl/ncdu)
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

#ifndef _dir_cache_lock_h
#define _dir_cache_lock_h

typedef enum {
    CACHE_LOCK_SHARED,     /* For reading - allows concurrent readers */
    CACHE_LOCK_EXCLUSIVE   /* For writing - blocks all access */
} cache_lock_mode;

/* Initialize lock subsystem with cache file path */
int cache_lock_init(const char *cache_path);

/* Acquire lock with timeout (-1 = blocking, 0 = non-blocking, >0 = timeout seconds) */
int cache_lock_acquire(cache_lock_mode mode, int timeout_sec);

/* Release current lock */
void cache_lock_release(void);

/* Cleanup on exit */
void cache_lock_cleanup(void);

#endif
