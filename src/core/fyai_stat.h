/* SPDX-License-Identifier: MIT */
#ifndef FYAI_STAT_H
#define FYAI_STAT_H

#include <sys/stat.h>

/* The modification time of a struct stat, as a struct timespec. */
#ifdef __APPLE__
#define fyai_st_mtime(st) ((st)->st_mtimespec)
#else
#define fyai_st_mtime(st) ((st)->st_mtim)
#endif

/* The status change time of a struct stat, as a struct timespec. */
#ifdef __APPLE__
#define fyai_st_ctime(st) ((st)->st_ctimespec)
#else
#define fyai_st_ctime(st) ((st)->st_ctim)
#endif

#endif
