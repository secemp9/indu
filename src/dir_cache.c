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

#include "global.h"
#include "dir_cache.h"
#include "dir_cache_lock.h"
#include "dir.h"
#include "util.h"

#include <khashl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>

/* Maximum length for JSON string values */
#define MAX_VAL (32*1024)

/* Read buffer size for JSON parsing */
#define READ_BUF_SIZE (64*1024)

/* Global cache file path */
char *cache_file = NULL;

/* Hash function for string keys - wrapper for khashl */
static khint_t cache_hash_str(const char *s) {
  return kh_hash_str(s);
}

/* Equality function for string keys */
static int cache_eq_str(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

/* Define hash table type: maps string (path) -> cache_entry* */
KHASHL_MAP_INIT(KH_LOCAL, cache_ht_t, cache_ht, const char *, struct cache_entry *, cache_hash_str, cache_eq_str)

/* Static hash table instance */
static cache_ht_t *cache_table = NULL;

/* Linked list of all cache entries for cleanup */
struct cache_entry_node {
  struct cache_entry *entry;
  struct cache_entry_node *next;
};
static struct cache_entry_node *entry_list = NULL;


/* ============================================================================
 * Helper functions for memory management
 * ============================================================================ */

/* Free a cache_child structure and all its nested children recursively */
static void free_cache_child(struct cache_child *child) {
  int i;
  if (!child)
    return;
  if (child->name)
    free(child->name);
  for (i = 0; i < child->nchildren; i++)
    free_cache_child(&child->children[i]);
  if (child->children)
    free(child->children);
}

/* Free a cache_entry structure */
static void free_cache_entry(struct cache_entry *entry) {
  int i;
  if (!entry)
    return;
  if (entry->path)
    free(entry->path);
  for (i = 0; i < entry->nchildren; i++)
    free_cache_child(&entry->children[i]);
  if (entry->children)
    free(entry->children);
  free(entry);
}

/* Add entry to the tracking list */
static void add_to_entry_list(struct cache_entry *entry) {
  struct cache_entry_node *node = xmalloc(sizeof(struct cache_entry_node));
  node->entry = entry;
  node->next = entry_list;
  entry_list = node;
}


/* ============================================================================
 * JSON Output helpers (for saving cache)
 * ============================================================================ */

/* Output a JSON-escaped string to file */
static void output_string(FILE *f, const char *str) {
  for (; *str; str++) {
    switch (*str) {
    case '\n': fputs("\\n", f); break;
    case '\r': fputs("\\r", f); break;
    case '\b': fputs("\\b", f); break;
    case '\t': fputs("\\t", f); break;
    case '\f': fputs("\\f", f); break;
    case '\\': fputs("\\\\", f); break;
    case '"':  fputs("\\\"", f); break;
    default:
      if ((unsigned char)*str <= 31 || (unsigned char)*str == 127)
        fprintf(f, "\\u00%02x", (unsigned char)*str);
      else
        fputc(*str, f);
      break;
    }
  }
}

/* Output an unsigned 64-bit integer */
static void output_int(FILE *f, uint64_t n) {
  char tmp[21];
  int i = 0;

  if (n == 0) {
    fputc('0', f);
    return;
  }

  while (n > 0) {
    tmp[i++] = '0' + (n % 10);
    n /= 10;
  }

  while (i > 0)
    fputc(tmp[--i], f);
}

/* Output a signed 64-bit integer */
static void output_int64(FILE *f, int64_t n) {
  if (n < 0) {
    fputc('-', f);
    output_int(f, (uint64_t)(-n));
  } else {
    output_int(f, (uint64_t)n);
  }
}

/* Write a cache_child to JSON output */
static void write_cache_child(FILE *f, struct cache_child *child) {
  int i;
  int first_field = 1;

  /* If this child is a directory, wrap in array brackets */
  if (child->flags & FF_DIR)
    fputc('[', f);

  fputc('{', f);

  /* name */
  fputs("\"name\":\"", f);
  output_string(f, child->name);
  fputc('"', f);
  first_field = 0;

  /* asize */
  if (child->asize != 0) {
    fputs(",\"asize\":", f);
    output_int64(f, child->asize);
  }

  /* dsize */
  if (child->size != 0) {
    fputs(",\"dsize\":", f);
    output_int64(f, child->size);
  }

  /* dev - only output if non-zero */
  if (child->dev != 0) {
    fputs(",\"dev\":", f);
    output_int(f, child->dev);
  }

  /* ino */
  if (child->ino != 0) {
    fputs(",\"ino\":", f);
    output_int(f, child->ino);
  }

  /* mtime */
  if (child->mtime != 0) {
    fputs(",\"mtime\":", f);
    output_int(f, child->mtime);
  }

  /* uid */
  if (child->uid != 0) {
    fputs(",\"uid\":", f);
    output_int(f, child->uid);
  }

  /* gid */
  if (child->gid != 0) {
    fputs(",\"gid\":", f);
    output_int(f, child->gid);
  }

  /* mode */
  if (child->mode != 0) {
    fputs(",\"mode\":", f);
    output_int(f, child->mode);
  }

  /* nlink - for hard link detection */
  if (child->nlink > 1) {
    fputs(",\"hlnkc\":true,\"nlink\":", f);
    output_int(f, child->nlink);
  }

  /* flags - handle special flags */
  if (child->flags & FF_ERR)
    fputs(",\"read_error\":true", f);
  if (!(child->flags & (FF_DIR | FF_FILE | FF_ERR | FF_EXL | FF_OTHFS | FF_KERNFS | FF_FRMLNK)))
    fputs(",\"notreg\":true", f);
  if (child->flags & FF_EXL)
    fputs(",\"excluded\":\"pattern\"", f);
  else if (child->flags & FF_OTHFS)
    fputs(",\"excluded\":\"otherfs\"", f);
  else if (child->flags & FF_KERNFS)
    fputs(",\"excluded\":\"kernfs\"", f);
  else if (child->flags & FF_FRMLNK)
    fputs(",\"excluded\":\"frmlnk\"", f);

  fputc('}', f);

  /* Write nested children */
  if (child->nchildren > 0) {
    for (i = 0; i < child->nchildren; i++) {
      fputs(",\n", f);
      write_cache_child(f, &child->children[i]);
    }
  }

  /* Close directory bracket */
  if (child->flags & FF_DIR)
    fputc(']', f);
}


/* ============================================================================
 * JSON Parsing helpers (for loading cache)
 * ============================================================================ */

/* Parser context */
struct parse_ctx {
  FILE *f;
  char *buf;
  char *pos;
  char *end;
  int line;
  int eof;
  char val[MAX_VAL];
};

/* Fill the read buffer */
static int parse_fill(struct parse_ctx *ctx) {
  size_t remaining, nread;

  if (ctx->eof)
    return 0;

  remaining = ctx->end - ctx->pos;
  if (remaining > 0)
    memmove(ctx->buf, ctx->pos, remaining);

  ctx->pos = ctx->buf;
  ctx->end = ctx->buf + remaining;

  nread = fread(ctx->end, 1, READ_BUF_SIZE - remaining - 1, ctx->f);
  if (nread == 0) {
    if (feof(ctx->f))
      ctx->eof = 1;
    else if (ferror(ctx->f))
      return -1;
  }

  ctx->end += nread;
  *ctx->end = '\0';

  return 0;
}

/* Skip whitespace */
static int parse_skip_ws(struct parse_ctx *ctx) {
  while (1) {
    if (ctx->pos >= ctx->end) {
      if (parse_fill(ctx) < 0)
        return -1;
      if (ctx->pos >= ctx->end)
        return 0;
    }

    switch (*ctx->pos) {
    case ' ':
    case '\t':
    case '\r':
      ctx->pos++;
      break;
    case '\n':
      ctx->pos++;
      ctx->line++;
      break;
    default:
      return 0;
    }
  }
}

/* Expect and consume a character */
static int parse_expect(struct parse_ctx *ctx, char c) {
  if (parse_skip_ws(ctx) < 0)
    return -1;
  if (ctx->pos >= ctx->end || *ctx->pos != c)
    return -1;
  ctx->pos++;
  return 0;
}

/* Check current character without consuming */
static char parse_peek(struct parse_ctx *ctx) {
  if (parse_skip_ws(ctx) < 0)
    return '\0';
  if (ctx->pos >= ctx->end)
    return '\0';
  return *ctx->pos;
}

/* Parse a JSON string into dest (max destlen bytes) */
static int parse_string(struct parse_ctx *ctx, char *dest, int destlen) {
  int len = 0;

  if (parse_expect(ctx, '"') < 0)
    return -1;

  while (1) {
    if (ctx->pos >= ctx->end) {
      if (parse_fill(ctx) < 0)
        return -1;
      if (ctx->pos >= ctx->end)
        return -1; /* Unexpected EOF */
    }

    if (*ctx->pos == '"') {
      ctx->pos++;
      if (dest && len < destlen)
        dest[len] = '\0';
      return 0;
    }

    if (*ctx->pos == '\\') {
      ctx->pos++;
      if (ctx->pos >= ctx->end) {
        if (parse_fill(ctx) < 0)
          return -1;
        if (ctx->pos >= ctx->end)
          return -1;
      }

      char c = '\0';
      switch (*ctx->pos) {
      case '"': c = '"'; break;
      case '\\': c = '\\'; break;
      case '/': c = '/'; break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'u':
        /* Skip unicode escapes - just read 4 hex digits */
        ctx->pos++;
        for (int i = 0; i < 4 && ctx->pos < ctx->end; i++)
          ctx->pos++;
        continue;
      default:
        return -1; /* Invalid escape */
      }
      ctx->pos++;
      if (dest && len < destlen - 1)
        dest[len++] = c;
      continue;
    }

    if (dest && len < destlen - 1)
      dest[len++] = *ctx->pos;
    ctx->pos++;
  }
}

/* Parse a JSON integer */
static int parse_int64(struct parse_ctx *ctx, int64_t *val) {
  int neg = 0;
  *val = 0;

  if (parse_skip_ws(ctx) < 0)
    return -1;

  if (ctx->pos < ctx->end && *ctx->pos == '-') {
    neg = 1;
    ctx->pos++;
  }

  if (ctx->pos >= ctx->end || *ctx->pos < '0' || *ctx->pos > '9')
    return -1;

  while (ctx->pos < ctx->end && *ctx->pos >= '0' && *ctx->pos <= '9') {
    *val = (*val * 10) + (*ctx->pos - '0');
    ctx->pos++;
  }

  if (neg)
    *val = -(*val);

  return 0;
}

/* Parse an unsigned 64-bit integer */
static int parse_uint64(struct parse_ctx *ctx, uint64_t *val) {
  *val = 0;

  if (parse_skip_ws(ctx) < 0)
    return -1;

  if (ctx->pos >= ctx->end || *ctx->pos < '0' || *ctx->pos > '9')
    return -1;

  while (ctx->pos < ctx->end && *ctx->pos >= '0' && *ctx->pos <= '9') {
    *val = (*val * 10) + (*ctx->pos - '0');
    ctx->pos++;
  }

  /* Handle decimal part (discard it) */
  if (ctx->pos < ctx->end && *ctx->pos == '.') {
    ctx->pos++;
    while (ctx->pos < ctx->end && *ctx->pos >= '0' && *ctx->pos <= '9')
      ctx->pos++;
  }

  return 0;
}

/* Skip any JSON value */
static int parse_skip_value(struct parse_ctx *ctx);

/* Skip a JSON object */
static int parse_skip_object(struct parse_ctx *ctx) {
  if (parse_expect(ctx, '{') < 0)
    return -1;

  if (parse_peek(ctx) == '}') {
    ctx->pos++;
    return 0;
  }

  while (1) {
    if (parse_string(ctx, NULL, 0) < 0)
      return -1;
    if (parse_expect(ctx, ':') < 0)
      return -1;
    if (parse_skip_value(ctx) < 0)
      return -1;

    char c = parse_peek(ctx);
    if (c == '}') {
      ctx->pos++;
      return 0;
    }
    if (c != ',')
      return -1;
    ctx->pos++;
  }
}

/* Skip a JSON array */
static int parse_skip_array(struct parse_ctx *ctx) {
  if (parse_expect(ctx, '[') < 0)
    return -1;

  if (parse_peek(ctx) == ']') {
    ctx->pos++;
    return 0;
  }

  while (1) {
    if (parse_skip_value(ctx) < 0)
      return -1;

    char c = parse_peek(ctx);
    if (c == ']') {
      ctx->pos++;
      return 0;
    }
    if (c != ',')
      return -1;
    ctx->pos++;
  }
}

/* Skip any JSON value */
static int parse_skip_value(struct parse_ctx *ctx) {
  char c = parse_peek(ctx);

  switch (c) {
  case '{':
    return parse_skip_object(ctx);
  case '[':
    return parse_skip_array(ctx);
  case '"':
    return parse_string(ctx, NULL, 0);
  case 't': /* true */
    ctx->pos += 4;
    return 0;
  case 'f': /* false */
    ctx->pos += 5;
    return 0;
  case 'n': /* null */
    ctx->pos += 4;
    return 0;
  default:
    /* number */
    if (c == '-' || (c >= '0' && c <= '9')) {
      while (ctx->pos < ctx->end &&
             (*ctx->pos == '-' || *ctx->pos == '+' || *ctx->pos == '.' ||
              *ctx->pos == 'e' || *ctx->pos == 'E' ||
              (*ctx->pos >= '0' && *ctx->pos <= '9')))
        ctx->pos++;
      return 0;
    }
    return -1;
  }
}

/* Forward declaration */
static int parse_item(struct parse_ctx *ctx, struct cache_child *child, uint64_t parent_dev);

/* Parse item info object into a cache_child */
static int parse_item_info(struct parse_ctx *ctx, struct cache_child *child, uint64_t parent_dev) {
  int64_t iv;
  uint64_t uv;

  if (parse_expect(ctx, '{') < 0)
    return -1;

  child->dev = parent_dev; /* Inherit parent's dev by default */

  while (1) {
    if (parse_skip_ws(ctx) < 0)
      return -1;

    if (parse_peek(ctx) == '}') {
      ctx->pos++;
      return 0;
    }

    /* Parse key */
    if (parse_string(ctx, ctx->val, MAX_VAL) < 0)
      return -1;
    if (parse_expect(ctx, ':') < 0)
      return -1;

    /* Parse value based on key */
    if (strcmp(ctx->val, "name") == 0) {
      char name[MAX_VAL];
      if (parse_string(ctx, name, MAX_VAL) < 0)
        return -1;
      child->name = xstrdup(name);
    }
    else if (strcmp(ctx->val, "asize") == 0) {
      if (parse_int64(ctx, &iv) < 0)
        return -1;
      child->asize = iv;
    }
    else if (strcmp(ctx->val, "dsize") == 0) {
      if (parse_int64(ctx, &iv) < 0)
        return -1;
      child->size = iv;
    }
    else if (strcmp(ctx->val, "dev") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->dev = uv;
    }
    else if (strcmp(ctx->val, "ino") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->ino = uv;
    }
    else if (strcmp(ctx->val, "mtime") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->mtime = uv;
    }
    else if (strcmp(ctx->val, "uid") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->uid = (unsigned int)uv;
    }
    else if (strcmp(ctx->val, "gid") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->gid = (unsigned int)uv;
    }
    else if (strcmp(ctx->val, "mode") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->mode = (unsigned short)uv;
    }
    else if (strcmp(ctx->val, "nlink") == 0) {
      if (parse_uint64(ctx, &uv) < 0)
        return -1;
      child->nlink = (unsigned int)uv;
      if (uv > 1)
        child->flags |= FF_HLNKC;
    }
    else if (strcmp(ctx->val, "hlnkc") == 0) {
      if (parse_peek(ctx) == 't') {
        ctx->pos += 4; /* true */
        child->flags |= FF_HLNKC;
      } else {
        ctx->pos += 5; /* false */
      }
    }
    else if (strcmp(ctx->val, "read_error") == 0) {
      if (parse_peek(ctx) == 't') {
        ctx->pos += 4;
        child->flags |= FF_ERR;
      } else {
        ctx->pos += 5;
      }
    }
    else if (strcmp(ctx->val, "excluded") == 0) {
      char excl[16];
      if (parse_string(ctx, excl, sizeof(excl)) < 0)
        return -1;
      if (strcmp(excl, "otherfs") == 0 || strcmp(excl, "othfs") == 0)
        child->flags |= FF_OTHFS;
      else if (strcmp(excl, "kernfs") == 0)
        child->flags |= FF_KERNFS;
      else if (strcmp(excl, "frmlnk") == 0)
        child->flags |= FF_FRMLNK;
      else
        child->flags |= FF_EXL;
    }
    else if (strcmp(ctx->val, "notreg") == 0) {
      if (parse_peek(ctx) == 't') {
        ctx->pos += 4;
        child->flags &= ~FF_FILE;
      } else {
        ctx->pos += 5;
      }
    }
    else {
      /* Unknown key, skip value */
      if (parse_skip_value(ctx) < 0)
        return -1;
    }

    /* Check for comma or end of object */
    if (parse_skip_ws(ctx) < 0)
      return -1;
    if (parse_peek(ctx) == '}') {
      ctx->pos++;
      return 0;
    }
    if (parse_peek(ctx) == ',')
      ctx->pos++;
  }
}

/* Parse an item (file or directory) */
static int parse_item(struct parse_ctx *ctx, struct cache_child *child, uint64_t parent_dev) {
  int isdir = 0;
  int nchildren = 0;
  int children_cap = 0;
  struct cache_child *children = NULL;

  memset(child, 0, sizeof(*child));
  child->flags = FF_FILE; /* Default to file */

  if (parse_skip_ws(ctx) < 0)
    return -1;

  /* Check if this is a directory (starts with '[') */
  if (parse_peek(ctx) == '[') {
    isdir = 1;
    child->flags = FF_DIR;
    ctx->pos++;
    if (parse_skip_ws(ctx) < 0)
      return -1;
  }

  /* Parse the info object */
  if (parse_item_info(ctx, child, parent_dev) < 0)
    return -1;

  /* If directory, parse children */
  if (isdir) {
    uint64_t dev = child->dev;

    while (1) {
      if (parse_skip_ws(ctx) < 0)
        goto err;

      char c = parse_peek(ctx);
      if (c == ']') {
        ctx->pos++;
        break;
      }
      if (c != ',')
        goto err;
      ctx->pos++;

      /* Expand children array if needed */
      if (nchildren >= children_cap) {
        children_cap = children_cap ? children_cap * 2 : 8;
        children = xrealloc(children, children_cap * sizeof(struct cache_child));
      }

      /* Parse child item */
      if (parse_item(ctx, &children[nchildren], dev) < 0)
        goto err;
      nchildren++;
    }

    child->children = children;
    child->nchildren = nchildren;
  }

  return 0;

err:
  for (int i = 0; i < nchildren; i++)
    free_cache_child(&children[i]);
  if (children)
    free(children);
  return -1;
}

/* Build cache entry from a parsed cache_child (only creates entry for this directory,
 * does NOT recursively process children - they have their own standalone entries in the cache) */
static void build_cache_entries(struct cache_child *child, const char *parent_path) {
  char *full_path;
  struct cache_entry *entry;
  int absent;
  khint_t k;
  int i;

  if (!child || !child->name)
    return;

  /* Build full path */
  if (parent_path && parent_path[0]) {
    size_t plen = strlen(parent_path);
    size_t nlen = strlen(child->name);
    full_path = xmalloc(plen + 1 + nlen + 1);
    strcpy(full_path, parent_path);
    if (parent_path[plen - 1] != '/')
      strcat(full_path, "/");
    strcat(full_path, child->name);
  } else {
    full_path = xstrdup(child->name);
  }

  /* Only create cache entries for directories */
  if (child->flags & FF_DIR) {
    entry = xcalloc(1, sizeof(struct cache_entry));
    entry->path = full_path;
    entry->mtime = child->mtime;
    entry->dev = child->dev;
    entry->ino = child->ino;
    entry->size = child->size;
    entry->asize = child->asize;
    entry->items = child->nchildren;
    entry->used = 0;

    /* Copy children for replay */
    if (child->nchildren > 0) {
      entry->children = xcalloc(child->nchildren, sizeof(struct cache_child));
      entry->nchildren = child->nchildren;
      for (i = 0; i < child->nchildren; i++) {
        /* Deep copy children */
        struct cache_child *src = &child->children[i];
        struct cache_child *dst = &entry->children[i];
        dst->name = src->name ? xstrdup(src->name) : NULL;
        dst->flags = src->flags;
        dst->size = src->size;
        dst->asize = src->asize;
        dst->ino = src->ino;
        dst->dev = src->dev;
        dst->mtime = src->mtime;
        dst->uid = src->uid;
        dst->gid = src->gid;
        dst->nlink = src->nlink;
        dst->mode = src->mode;
        /* Don't copy nested children here - they have their own standalone entries */
        dst->children = NULL;
        dst->nchildren = 0;
      }
    }

    /* Add to hash table */
    k = cache_ht_put(cache_table, entry->path, &absent);
    if (absent) {
      kh_val(cache_table, k) = entry;
      add_to_entry_list(entry);
    } else {
      /* Entry already exists, free this one */
      free_cache_entry(entry);
    }

    /* NOTE: Do NOT recursively process child directories here!
     * Child directories have their own standalone entries in the cache file
     * with full children data. If we recursively create entries from parents,
     * we'd create entries with 0 children (since parents store shallow copies),
     * which would then block the correct entry from being added later. */
  } else {
    free(full_path);
  }
}


/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/* Initialize cache system with given filename */
void dir_cache_init(const char *fn) {
  char *new_cache_file = xstrdup(fn);  /* Copy first to avoid use-after-free if fn == cache_file */
  if (cache_file)
    free(cache_file);
  cache_file = new_cache_file;

  if (cache_table)
    cache_ht_destroy(cache_table);
  cache_table = cache_ht_init();

  /* Initialize the lock subsystem */
  cache_lock_init(cache_file);
}


/* Load cache from file */
int dir_cache_load(void) {
  struct parse_ctx ctx;
  struct cache_child item;
  int64_t major, minor;
  FILE *f;
  int ret = 0;
  char c;

  if (!cache_file)
    return -1;

  /* Acquire shared lock for reading (5 second timeout) */
  if (cache_lock_acquire(CACHE_LOCK_SHARED, 5) < 0) {
    /* Could not acquire lock - proceed without cache */
    return 0;
  }

  f = fopen(cache_file, "r");
  if (!f) {
    cache_lock_release();
    /* Missing cache file is not an error - just means no cache */
    if (errno == ENOENT)
      return 0;
    return -1;
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.f = f;
  ctx.buf = xmalloc(READ_BUF_SIZE);
  ctx.pos = ctx.buf;
  ctx.end = ctx.buf;
  ctx.line = 1;
  ctx.eof = 0;

  /* Parse header: [1,2,{...}, */
  if (parse_expect(&ctx, '[') < 0)
    goto err;

  /* Major version */
  if (parse_int64(&ctx, &major) < 0 || major != 1)
    goto err;
  if (parse_expect(&ctx, ',') < 0)
    goto err;

  /* Minor version */
  if (parse_int64(&ctx, &minor) < 0)
    goto err;
  if (parse_expect(&ctx, ',') < 0)
    goto err;

  /* Skip metadata object */
  if (parse_skip_object(&ctx) < 0)
    goto err;

  /* Parse all directory entries in the root array */
  while (1) {
    c = parse_peek(&ctx);
    if (c == ']') {
      /* End of root array */
      ctx.pos++;
      break;
    }
    if (c != ',')
      goto err;
    ctx.pos++;

    /* Parse this directory entry */
    memset(&item, 0, sizeof(item));
    if (parse_item(&ctx, &item, 0) < 0) {
      free_cache_child(&item);
      goto err;
    }

    /* Build cache entries from the parsed item */
    build_cache_entries(&item, "");

    /* Clean up parsed item (entries have been copied) */
    free_cache_child(&item);
  }

  goto cleanup;

err:
  ret = -1;

cleanup:
  free(ctx.buf);
  fclose(f);
  cache_lock_release();
  return ret;
}


/* Look up cached entry by path, validating mtime/dev/ino */
struct cache_entry *dir_cache_lookup(const char *path, uint64_t mtime, uint64_t dev, uint64_t ino) {
  khint_t k;
  struct cache_entry *entry;

  if (!cache_table || !path)
    return NULL;

  k = cache_ht_get(cache_table, path);
  if (k >= kh_end(cache_table))
    return NULL;

  entry = kh_val(cache_table, k);
  if (!entry)
    return NULL;

  /* Validate the entry - all three must match */
  if (entry->mtime != mtime || entry->dev != dev || entry->ino != ino)
    return NULL;

  /* Mark as used */
  entry->used = 1;

  return entry;
}


/* Store a scanned directory in the cache with explicit children */
void dir_cache_store(const char *path, struct dir *d, struct dir_ext *ext,
                     struct cache_child *children, int nchildren) {
  struct cache_entry *entry;
  int i;
  khint_t k;
  int absent;

  if (!cache_table || !path || !d)
    return;

  /* Create new entry */
  entry = xcalloc(1, sizeof(struct cache_entry));
  entry->path = xstrdup(path);
  entry->mtime = ext && (ext->flags & FFE_MTIME) ? ext->mtime : 0;
  entry->dev = d->dev;
  entry->ino = d->ino;
  entry->size = d->size;
  entry->asize = d->asize;
  entry->items = d->items;
  entry->used = 1; /* Mark as used immediately */

  /* Store children (deep copy) */
  if (nchildren > 0 && children) {
    entry->children = xcalloc(nchildren, sizeof(struct cache_child));
    entry->nchildren = nchildren;

    for (i = 0; i < nchildren; i++) {
      struct cache_child *src = &children[i];
      struct cache_child *dst = &entry->children[i];

      dst->name = src->name ? xstrdup(src->name) : NULL;
      dst->flags = src->flags;
      dst->size = src->size;
      dst->asize = src->asize;
      dst->ino = src->ino;
      dst->dev = src->dev;
      dst->mtime = src->mtime;
      dst->uid = src->uid;
      dst->gid = src->gid;
      dst->mode = src->mode;
      dst->nlink = src->nlink;
      dst->children = NULL;
      dst->nchildren = 0;
    }
  }

  /* Check if entry already exists */
  k = cache_ht_get(cache_table, path);
  if (k < kh_end(cache_table)) {
    /* Replace existing entry */
    struct cache_entry *old = kh_val(cache_table, k);
    kh_val(cache_table, k) = entry;
    /* Don't free old - it's still in entry_list and will be freed at destroy */
    /* Mark old as unused so it won't be saved */
    if (old)
      old->used = 0;
  } else {
    /* Add new entry */
    k = cache_ht_put(cache_table, entry->path, &absent);
    kh_val(cache_table, k) = entry;
  }

  add_to_entry_list(entry);
}


/* Helper to recursively replay a cached subtree */
static void dir_cache_replay_recursive(const char *parent_path, struct cache_child *child) {
  struct dir d;
  struct dir_ext ext;
  char *child_path;
  size_t parent_len, child_len;
  khint_t k;
  struct cache_entry *subentry;

  /* Set up dir struct */
  memset(&d, 0, sizeof(d));
  d.size = child->size;
  d.asize = child->asize;
  d.ino = child->ino;
  d.dev = child->dev;
  d.flags = child->flags;

  /* Set up ext struct */
  memset(&ext, 0, sizeof(ext));
  if (child->mtime) {
    ext.mtime = child->mtime;
    ext.flags |= FFE_MTIME;
    d.flags |= FF_EXT;
  }
  if (child->uid) {
    ext.uid = child->uid;
    ext.flags |= FFE_UID;
    d.flags |= FF_EXT;
  }
  if (child->gid) {
    ext.gid = child->gid;
    ext.flags |= FFE_GID;
    d.flags |= FF_EXT;
  }
  if (child->mode) {
    ext.mode = child->mode;
    ext.flags |= FFE_MODE;
    d.flags |= FF_EXT;
  }

  /* Call the output callback */
  dir_output.item(&d, child->name, &ext, child->nlink);

  /* For directories, recursively replay children */
  if (child->flags & FF_DIR) {
    /* Build full path for this child directory */
    parent_len = strlen(parent_path);
    child_len = strlen(child->name);
    child_path = xmalloc(parent_len + 1 + child_len + 1);
    strcpy(child_path, parent_path);
    if (parent_len > 0 && parent_path[parent_len - 1] != '/')
      strcat(child_path, "/");
    strcat(child_path, child->name);

    /* Look up cached entry for this subdirectory */
    if (cache_table) {
      k = cache_ht_get(cache_table, child_path);
      if (k < kh_end(cache_table)) {
        subentry = kh_val(cache_table, k);
        if (subentry) {
          int i;
          /* Mark as used */
          subentry->used = 1;
          /* Recursively replay children of this subdirectory */
          for (i = 0; i < subentry->nchildren; i++) {
            dir_cache_replay_recursive(child_path, &subentry->children[i]);
          }
        }
      }
    }

    free(child_path);

    /* Close the directory */
    dir_output.item(NULL, NULL, NULL, 0);
  }
}

/* Replay cached subtree to dir_output callbacks */
void dir_cache_replay(struct cache_entry *entry) {
  int i;

  if (!entry)
    return;

  /* Replay each child recursively */
  for (i = 0; i < entry->nchildren; i++) {
    dir_cache_replay_recursive(entry->path, &entry->children[i]);
  }
}


/* Helper to fsync a directory by path */
static int fsync_dir(const char *dirpath) {
  int fd;
  int ret;

  fd = open(dirpath, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return -1;

  ret = fsync(fd);
  close(fd);
  return ret;
}

/* Save cache to file */
void dir_cache_save(void) {
  FILE *f;
  char *tmp_path;
  char *dir_path;
  char *dir_copy;
  int tmp_fd;
  struct cache_entry_node *node;
  int first = 1;
  khint_t k;
  int save_errno;

  if (!cache_file || !cache_table)
    return;

  /* Acquire exclusive lock for writing (10 second timeout) */
  if (cache_lock_acquire(CACHE_LOCK_EXCLUSIVE, 10) < 0) {
    /* Could not acquire lock - skip saving */
    return;
  }

  /* Create temporary file using mkstemp for unique naming
   * Format: cache_file + ".XXXXXX" */
  tmp_path = xmalloc(strlen(cache_file) + 8);
  sprintf(tmp_path, "%s.XXXXXX", cache_file);

  tmp_fd = mkstemp(tmp_path);
  if (tmp_fd < 0) {
    free(tmp_path);
    cache_lock_release();
    return;
  }

  /* Convert fd to FILE* for convenience */
  f = fdopen(tmp_fd, "w");
  if (!f) {
    save_errno = errno;
    close(tmp_fd);
    unlink(tmp_path);
    free(tmp_path);
    cache_lock_release();
    return;
  }

  /* Write header */
  fputs("[1,2,{\"progname\":\"" PACKAGE "\",\"progver\":\"" PACKAGE_VERSION "\",\"timestamp\":", f);
  output_int(f, (uint64_t)time(NULL));
  fputc('}', f);

  /* We need to write entries as a tree structure
   * For simplicity, we'll write each used directory entry as a separate item
   * This creates a flat structure where each directory has its children
   */

  /* Find and write root entries (entries that are at the top level) */
  for (k = 0; k < kh_end(cache_table); k++) {
    if (!__kh_used(cache_table->used, k))
      continue;

    struct cache_entry *entry = kh_val(cache_table, k);
    if (!entry || !entry->used)
      continue;

    /* Write this entry and its children */
    fputs(",\n[{\"name\":\"", f);
    output_string(f, entry->path);
    fputc('"', f);

    if (entry->asize) {
      fputs(",\"asize\":", f);
      output_int64(f, entry->asize);
    }
    if (entry->size) {
      fputs(",\"dsize\":", f);
      output_int64(f, entry->size);
    }
    if (entry->dev) {
      fputs(",\"dev\":", f);
      output_int(f, entry->dev);
    }
    if (entry->ino) {
      fputs(",\"ino\":", f);
      output_int(f, entry->ino);
    }
    if (entry->mtime) {
      fputs(",\"mtime\":", f);
      output_int(f, entry->mtime);
    }

    fputc('}', f);

    /* Write children */
    for (int i = 0; i < entry->nchildren; i++) {
      fputs(",\n", f);
      write_cache_child(f, &entry->children[i]);
    }

    fputc(']', f);
    first = 0;
  }

  /* Close the root array */
  fputs("]\n", f);

  /* Flush stdio buffers */
  if (fflush(f) != 0) {
    fclose(f);
    unlink(tmp_path);
    free(tmp_path);
    cache_lock_release();
    return;
  }

  /* fsync the file data before rename to ensure durability */
  if (fsync(fileno(f)) != 0) {
    fclose(f);
    unlink(tmp_path);
    free(tmp_path);
    cache_lock_release();
    return;
  }

  if (fclose(f) == 0) {
    /* Atomic rename */
    if (rename(tmp_path, cache_file) == 0) {
      /* fsync the parent directory to ensure the rename is durable */
      dir_copy = xstrdup(cache_file);
      dir_path = dirname(dir_copy);
      fsync_dir(dir_path);
      free(dir_copy);
    } else {
      unlink(tmp_path);
    }
  } else {
    unlink(tmp_path);
  }

  free(tmp_path);
  cache_lock_release();
}


/* Free all cache memory */
void dir_cache_destroy(void) {
  struct cache_entry_node *node, *next;

  /* Cleanup lock subsystem first */
  cache_lock_cleanup();

  /* Free all entries in the list */
  for (node = entry_list; node; node = next) {
    next = node->next;
    free_cache_entry(node->entry);
    free(node);
  }
  entry_list = NULL;

  /* Destroy hash table */
  if (cache_table) {
    cache_ht_destroy(cache_table);
    cache_table = NULL;
  }

  /* Free cache file path */
  if (cache_file) {
    free(cache_file);
    cache_file = NULL;
  }
}
