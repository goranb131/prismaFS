/* ============================================================
   PrismaFS - ops_file.c
   
   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */
#include "prismafs.h"

// open operation func implementation
int myfs_open(const char *path, struct fuse_file_info *fi)
{
    // opening registered synth file (/dev/cpu, /net/interfaces, etc)
    if (syn_fp(path) != NULL) 
    {
        return 0;
    }

    int res;
    char fpath[PATH_MAX];

    // try to open file in session layer
    session_fullpath(fpath, path);
    res = open(fpath, fi -> flags);

    if (res != -1) {
        close(res);
        return 0;
    }

    // check .deleted marker in session
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);

    if (access(deleted_marker, F_OK) == 0) {
        // marked/masked as deleted
        return -ENOENT;
    }

    // dont open with fi -> flags, because flags O_WRONLY|O_TRUNC could truncate file
    if (base_fullpath_func(fpath, path) == 0)
        return 0;

    // if after above file not in layers, log and err
    fprintf(stderr, "open: %s not found in session or base layers\n", path);
    return -ENOENT;
}

// statfs operation func implementation
int myfs_statfs(const char *path, struct statvfs *stbuf) {
    // fill statvfs structure
    memset(stbuf, 0, sizeof(struct statvfs));

    stbuf->f_bsize   = 4096;
    stbuf->f_frsize  = 4096;
    stbuf->f_blocks  = 1024 * 1024;
    stbuf->f_bfree   = 1024 * 512;
    stbuf->f_bavail  = 1024 * 512;
    stbuf->f_files   = 1024 * 1024;
    stbuf->f_ffree   = 1024 * 512;
    stbuf->f_namemax = 255;

    return 0;
}

// read operation func implementation
int myfs_read(const char *path, char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi) {

    // for reading from registered synthetic file
    const syn_entry_t *se = syn_fp(path);

    if (se) 
    {
        char tmp[2048];
        int len = se -> gen(tmp, sizeof(tmp));

        if (len < 0)
            return -EIO;

        if (offset < len) 
        {
            if (offset + size > (size_t)len)
                size = len - offset;

            memcpy(buf, tmp + offset, size);

        } else {
            size = 0;
        }

        return size;
    }

    int fd;
    int res;
    char fpath[PATH_MAX];

    // try to open file in session layer
    session_fullpath(fpath, path);

    fd = open(fpath, O_RDONLY);

    if (fd != -1) {
        // read from file
        res = pread(fd, buf, size, offset);
        if (res == -1)
            res = -errno;
        close(fd);
        return res;
    }

    // check .deleted marker in the session layer
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);

    if (access(deleted_marker, F_OK) == 0) {
        // masked as deleted
        return -ENOENT;
    }

    // if not in session layer and not masked, try base layers
    for (int i = 0; i < num_base_layers; i++) {
        // full path for current base layer
        if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
        else
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

        // check file is in current base layer
        if (access(fpath, F_OK) == 0) {
            fd = open(fpath, O_RDONLY);
            if (fd != -1) {
                // read from file
                res = pread(fd, buf, size, offset);
                if (res == -1)
                    res = -errno;
                close(fd);
                return res;
            }
        }
    }

    // file not in any layer
    return -ENOENT;
}

// write operation func implementation
int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi)
{
    int fd;
    int res;
    char fpath[PATH_MAX];

    // write operations need to happen in session layer
    session_fullpath(fpath, path);

    // if file doesnt exist in session layer, copy it from base layer
    if (access(fpath, F_OK) == -1)
    {
        char base_fpath[PATH_MAX];
        base_fullpath_func(base_fpath, path);

        // create dirs
        char *dir_end = strrchr(fpath, '/');
        if (dir_end)
        {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        // copy file from base layer into session (CoW)
        /* now cow_file() does copying checking writes and removes any incomplete dest content on fail 
            session copy permissions 0644 = rw-r--r-- */
        if (cow_file(base_fpath, fpath, 0644) != 0)
            return -EIO;
    }

    // open file in session layer for writing
    fd = open(fpath, O_WRONLY);
    if (fd == -1)
        return -errno;

    res = pwrite(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return res;
}

// truncate operation func implementation
#if FUSE_USE_VERSION >= 30
int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void) fi;
#else
int myfs_truncate(const char *path, off_t size)
{
#endif
    char fpath[PATH_MAX];

    // truncate happens in session layer
    session_fullpath(fpath, path);

    // copy file from base layer if not in session yet
    if (access(fpath, F_OK) == -1)
    {
        char base_fpath[PATH_MAX];
        base_fullpath_func(base_fpath, path);

        // create dirs
        char *dir_end = strrchr(fpath, '/');
        if (dir_end)
        {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

         // copy file from base layer into session (CoW)
        /* now cow_file() does copying checking writes and removes any incomplete dest content on fail 
            session copy permissions 0644 = rw-r--r-- */
        if (cow_file(base_fpath, fpath, 0644) != 0)
            return -EIO;
    }

    // truncate file
    int res = truncate(fpath, size);
    if (res == -1)
        return -errno;

    return 0;
}

// create operation func implementation
int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int res;
    char fpath[PATH_MAX];

    // create file in session layer
    session_fullpath(fpath, path);

    // create directories
    char *dir_end = strrchr(fpath, '/');
    if (dir_end)
    {
        char dir_path[PATH_MAX];
        strncpy(dir_path, fpath, dir_end - fpath);
        dir_path[dir_end - fpath] = '\0';
        mkdir(dir_path, 0755);
    }

    res = open(fpath, fi->flags, mode);
    if (res == -1)
        return -errno;

    fi->fh = res;
    return 0;
}
