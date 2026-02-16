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

#ifndef _dir_cache_h
#define _dir_cache_h

#include "global.h"

/* Cache child structure for subtree replay */
struct cache_child {
  char *name;
  uint16_t flags;
  int64_t size, asize;
  uint64_t ino, dev, mtime;
  unsigned int uid, gid, nlink;
  unsigned short mode;
  struct cache_child *children;  /* Nested directories */
  int nchildren;
};

/* Cache entry structure */
struct cache_entry {
  char *path;              /* Full path (hash key) */
  uint64_t mtime, dev, ino;/* Validation fields */
  int64_t size, asize;     /* Aggregated sizes */
  int items;               /* Item count */
  int used;                /* Still valid in current scan */
  struct cache_child *children;  /* For subtree replay */
  int nchildren;
};

/* Global cache file path (set via --cache option) */
extern char *cache_file;

/* Initialize cache system with given filename */
void dir_cache_init(const char *fn);

/* Load cache from file, returns 0 on success, non-zero on error */
int dir_cache_load(void);

/* Look up cached entry, returns entry if path/mtime/dev/ino match, NULL otherwise */
struct cache_entry *dir_cache_lookup(const char *path, uint64_t mtime, uint64_t dev, uint64_t ino);

/* Store a scanned directory in the cache with explicit children */
void dir_cache_store(const char *path, struct dir *d, struct dir_ext *ext,
                     struct cache_child *children, int nchildren);

/* Replay cached subtree to dir_output callbacks */
void dir_cache_replay(struct cache_entry *entry);

/* Save cache to file */
void dir_cache_save(void);

/* Free all cache memory */
void dir_cache_destroy(void);

#endif
