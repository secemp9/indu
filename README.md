# ncdu-turbo

A fork of [ncdu](https://dev.yorhel.nl/ncdu) (NCurses Disk Usage) with **incremental caching** for dramatically faster subsequent scans.

## What's New

ncdu-turbo adds mtime-based incremental caching. On first scan, it builds a cache file storing directory metadata. On subsequent scans, unchanged directories are loaded from cache instead of being rescanned.

**Expected speedup: ~90%** for typical workloads where <10% of files changed between scans.

## Usage

```bash
# Scan with cache (first run creates cache, subsequent runs use it)
ncdu --cache ~/.cache/ncdu-home.json ~

# Or short form
ncdu -C ~/.cache/ncdu-home.json ~
```

## How It Works

1. On first scan: builds cache file storing directory metadata (mtime, inode, device, sizes)
2. On subsequent scans: checks if directory mtime + inode + device match cache
3. If all match → skip recursion, use cached sizes
4. If any differ → rescan that directory subtree

The cache correctly handles:
- File modifications (mtime changes)
- New/deleted files (parent directory mtime changes)
- Renamed directories (inode changes)
- Moved to different filesystem (device changes)
- Concurrent access (file locking with stale lock detection)

## Building

```bash
autoreconf -fi
./configure
make
sudo make install
```

## Requirements

- ncurses library (ncursesw for wide character support)
- Standard C compiler (gcc/clang)
- autotools (autoconf, automake) for building from git

## Original ncdu

This is a fork of ncdu by Yoran Heling. See the [original project](https://dev.yorhel.nl/ncdu) for more information.

## License

MIT License (same as original ncdu)
