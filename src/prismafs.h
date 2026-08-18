/*
 * PrismaFS: A lightweight, layered filesystem.
 * Version: 1.7.1
 * Copyright 2026 Goran Bunić
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PRISMAFS_H
#define PRISMAFS_H

#if defined(__linux__)
#define FUSE_USE_VERSION 30
#else
#define FUSE_USE_VERSION 29
#endif
#define PRISMAFS_VERSION "1.7.1"
#define MAX_BASE_LAYERS 10

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>      // open, fd
#include <dirent.h>
#include <unistd.h>     // read, write, close, pread, pwrite, mkdir, rmdir, unlink
#include <limits.h>     // PATH_MAX
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>  // extended attributes, macOS and Linux
#include <stdlib.h>
#ifdef __APPLE__
#include <sys/sysctl.h> // sysctl (macOS only)
#endif
#include <sys/utsname.h>
#include <stdint.h>

// ENOATTR = "xattr not found" error on macOS
// ENODATA on Linux
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

// 5 args = FUSE3 (Linux)
// 4 args = FUSE2 (macOS)
#if FUSE_USE_VERSION >= 30
#define FUSE_FILL(buf, name, st, off) filler(buf, name, st, off, 0)
#else
#define FUSE_FILL(buf, name, st, off) filler(buf, name, st, off)
#endif

/* -------------------------------------------------------------
   GLOBAL LAYER STATE (definitions in layers.c)
   -------------------------------------------------------------
*/
extern char base_paths[MAX_BASE_LAYERS][PATH_MAX];
extern int  num_base_layers;
extern char session_path[PATH_MAX];

/* -------------------
 readdir reads entries from session layer, every base layer. 
  Same filename might appear in more layers, normally base and session(s).
  Need to track what is already added in linked list and FUSE, 
  so it doesnt get duplicates listed. 
  Linked list to track:
  -------------------*/
struct filename_node {
    char *name;
    struct filename_node *next;
};

/* -------------------------------------------------------------
   LAYER HELPERS (layers.c) 
   -------------------------------------------------------------
*/
void session_fullpath(char fpath[PATH_MAX], const char *path);
int  base_fullpath_func(char fpath[PATH_MAX], const char *path);
int  is_in_list(struct filename_node *list, const char *name);
void add_to_list(struct filename_node **list_ptr, const char *name);
int  cow_file(const char *src, const char *dst, mode_t mode);
int  cow_xattrs(const char *src, const char *dst);
int  expand_tilde(const char *in, char *out, size_t outsz);
int  mkdir_p(const char *path, mode_t mode);

/* -------------------------------------------------------------
   FUSE operation signatures (differences FUSE2(macOS) vs FUSE3(Linux)
   -------------------------------------------------------------
*/
#if FUSE_USE_VERSION >= 30
int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi,
                 enum fuse_readdir_flags flags);
int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int myfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int myfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi);
int myfs_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi);
int myfs_rename(const char *from, const char *to, unsigned int flags);
#else
int myfs_getattr(const char *path, struct stat *stbuf);
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi);
int myfs_truncate(const char *path, off_t size);
int myfs_chmod(const char *path, mode_t mode);
int myfs_chown(const char *path, uid_t uid, gid_t gid);
int myfs_utimens(const char *path, const struct timespec ts[2]);
int myfs_rename(const char *from, const char *to);
#endif

int myfs_access(const char *path, int mask);
int myfs_open(const char *path, struct fuse_file_info *fi);
int myfs_statfs(const char *path, struct statvfs *stbuf);
int myfs_read(const char *path, char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi);
int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi);
int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int myfs_mkdir(const char *path, mode_t mode);
int myfs_rmdir(const char *path);
int myfs_unlink(const char *path);
int myfs_symlink(const char *target, const char *linkpath);
int myfs_readlink(const char *path, char *buf, size_t size);

// xattr signatures, macOS FUSE has additional "uint32_t position" in getxattr/setxattr
// listxattr and removexattr same sig on both
#ifdef __APPLE__
int myfs_getxattr(const char *path, const char *name, char *value, size_t size, uint32_t position);
int myfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position);
#else
int myfs_getxattr(const char *path, const char *name, char *value, size_t size);
int myfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags);
#endif
int myfs_listxattr(const char *path, char *list, size_t size);
int myfs_removexattr(const char *path, const char *name);

#endif /* PRISMAFS_H */
