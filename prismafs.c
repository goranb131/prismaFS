/*
 * PrismaFS: A lightweight, layered filesystem inspired by Plan 9.
 * Version: 1.0.0
 * Copyright 2024 Goran Bunic, ITHAS 
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

#define FUSE_USE_VERSION 26
#define PRISMAFS_VERSION "1.0.0"

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>     // open, and fd
#include <dirent.h>
#include <unistd.h>    // for read, write, close, pread, pwrite, mkdir, rmdir, unlink
#include <limits.h>    // For PATH_MAX
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>

static const char *base_path_initial = "/"; // initial base layer path
static char base_path[PATH_MAX] = "/";
static char session_path[PATH_MAX]; // session layer 

// helper function to construct full path in the session layer
static void session_fullpath(char fpath[PATH_MAX], const char *path)
{
    if (session_path[strlen(session_path) - 1] == '/' && path[0] == '/')
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path);
}

// helper function to construct full path in the base layer
static void base_fullpath_func(char fpath[PATH_MAX], const char *path)
{
    if (base_path[strlen(base_path) - 1] == '/' && path[0] == '/')
        snprintf(fpath, PATH_MAX, "%s%s", base_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", base_path, path);
}

// getattr operation function implementation
static int myfs_getattr(const char *path, struct stat *stbuf)
{
    char fpath[PATH_MAX];
    int res;

    // Check if the file exists in the session layer
    session_fullpath(fpath, path);
    res = lstat(fpath, stbuf);
    if (res == 0) {
        printf("getattr: Found in session layer: %s\n", fpath);
        return 0;
    }

    // If not found in session, check in the base layer
    base_fullpath_func(fpath, path);
    res = lstat(fpath, stbuf);
    if (res == 0) {
        printf("getattr: Found in base layer: %s\n", fpath);
        return 0;
    }

    // File not found in either layer
    printf("getattr: File not found: %s\n", path);
    return -ENOENT;
}

//readdir operation function implementation
static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
    DIR *dp_session, *dp_base;
    struct dirent *de_session, *de_base;
    char session_fpath[PATH_MAX];
    char base_fpath[PATH_MAX];
    char marker_fpath[PATH_MAX];
    char session_file_path[PATH_MAX];

    // read files from session layer
    session_fullpath(session_fpath, path);
    dp_session = opendir(session_fpath);
    if (dp_session != NULL)
    {
        while ((de_session = readdir(dp_session)) != NULL)
        {
            // Skip hidden files and `.deleted` markers
            if (de_session->d_name[0] == '.' ||
                strstr(de_session->d_name, ".deleted") != NULL)
                continue;

            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de_session->d_ino;
            st.st_mode = de_session->d_type << 12;

            if (filler(buf, de_session->d_name, &st, 0))
                break;
        }
        closedir(dp_session);
    }

    // read files from base layer, masking files in the session layer
    base_fullpath_func(base_fpath, path);
    dp_base = opendir(base_fpath);
    if (dp_base == NULL)
        return -errno;

    while ((de_base = readdir(dp_base)) != NULL)
    {
        // skip hidden files
        if (de_base->d_name[0] == '.')
            continue;

        // construct a path to .deleted marker in session layer
        snprintf(marker_fpath, PATH_MAX, "%s/%s.deleted", session_fpath, de_base->d_name);

        // If a `.deleted` marker exists, skip this file
        if (access(marker_fpath, F_OK) == 0)
            continue;

        // construct full path to file in the session layer
        snprintf(session_file_path, PATH_MAX, "%s/%s", session_fpath, de_base->d_name);

        // if session layer file exists, skip to avoid duplicates
        if (access(session_file_path, F_OK) == 0)
            continue;

        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de_base->d_ino;
        st.st_mode = de_base->d_type << 12;

        if (filler(buf, de_base->d_name, &st, 0))
            break;
    }
    closedir(dp_base);

    return 0;
}

// open operation function implementation
static int myfs_open(const char *path, struct fuse_file_info *fi)
{
    int res;
    char fpath[PATH_MAX];

    // try to open file in session layer
    session_fullpath(fpath, path);
    res = open(fpath, fi->flags);
    if (res != -1)
    {
        close(res);
        return 0;
    }

    // if not in session layer, try inside base layer
    base_fullpath_func(fpath, path);
    res = open(fpath, fi->flags);
    if (res == -1)
        return -errno;

    close(res);
    return 0;
}


// read operation function implementation
static int myfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    int fd;
    int res;
    char fpath[PATH_MAX];

    // try to read from session layer first
    session_fullpath(fpath, path);
    fd = open(fpath, O_RDONLY);
    if (fd == -1)
    {
        // if not in session layer, read from base layer
        base_fullpath_func(fpath, path);
        fd = open(fpath, O_RDONLY);
        if (fd == -1)
            return -errno;
    }

    res = pread(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return res;
}

// write operation function implementation
static int myfs_write(const char *path, const char *buf, size_t size,
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

        // create directories 
        char *dir_end = strrchr(fpath, '/');
        if (dir_end)
        {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        // copy file from base layer
        int source_fd = open(base_fpath, O_RDONLY);
        int dest_fd = open(fpath, O_WRONLY | O_CREAT, 0644);

        if (source_fd != -1 && dest_fd != -1)
        {
            char buffer[8192];
            ssize_t bytes;
            while ((bytes = read(source_fd, buffer, sizeof(buffer))) > 0)
            {
                write(dest_fd, buffer, bytes);
            }
        }

        if (source_fd != -1)
            close(source_fd);
        if (dest_fd != -1)
            close(dest_fd);
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

// truncate operation function implementation
static int myfs_truncate(const char *path, off_t size)
{
    char fpath[PATH_MAX];

    // truncate happens in session layer
    session_fullpath(fpath, path);

    // if file doesnt exist in session layer - copy it from base layer
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

        // copy file from base layer
        int source_fd = open(base_fpath, O_RDONLY);
        int dest_fd = open(fpath, O_WRONLY | O_CREAT, 0644);

        if (source_fd != -1 && dest_fd != -1)
        {
            char buffer[8192];
            ssize_t bytes;
            while ((bytes = read(source_fd, buffer, sizeof(buffer))) > 0)
            {
                write(dest_fd, buffer, bytes);
            }
        }

        if (source_fd != -1)
            close(source_fd);
        if (dest_fd != -1)
            close(dest_fd);
    }

    // truncate file in the session layer
    int res = truncate(fpath, size);
    if (res == -1)
        return -errno;

    return 0;
}

// create operation function implementation
static int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int res;
    char fpath[PATH_MAX];

    // create file in session layer
    session_fullpath(fpath, path);

    // create directories if required
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

// mkdir operation function implementation
static int myfs_mkdir(const char *path, mode_t mode) {
    char fpath[PATH_MAX];

    // construct full path for the directory to be created
    session_fullpath(fpath, path); 

    int res = mkdir(fpath, mode);
    if (res == -1) {
        return -errno;
    }
    return 0;
}

// rmdir operation function implementation
static int myfs_rmdir(const char *path) {
    char fpath[PATH_MAX];

    // construct full path for the directory to be removed
    session_fullpath(fpath, path);

    // try to remove the directory
    int res = rmdir(fpath);
    if (res == -1) {
        perror("rmdir failed");
        return -errno;
    }

    return 0;
}

// chmod operation function implementation
static int myfs_chmod(const char *path, mode_t mode)
{
    char fpath[PATH_MAX];

    // first try to change mode in session layer
    session_fullpath(fpath, path);
    if (access(fpath, F_OK) == 0) {
        if (chmod(fpath, mode) == -1) {
            perror("chmod: Error changing mode in session layer");
            return -errno;
        }
        printf("chmod: Changed mode in session layer: %s\n", fpath);
        return 0;
    }

    // if not found, try in base layer
    base_fullpath_func(fpath, path);
    if (access(fpath, F_OK) == 0) {
        if (chmod(fpath, mode) == -1) {
            perror("chmod: Error changing mode in base layer");
            return -errno;
        }
        printf("chmod: Changed mode in base layer: %s\n", fpath);
        return 0;
    }

    // if file doesnt exist in any layer
    printf("chmod: File not found: %s\n", path);
    return -ENOENT;
}

// unlink operation function implementation
static int myfs_unlink(const char *path)
{
    char session_fpath[PATH_MAX];
    char base_fpath[PATH_MAX];

    // full paths
    session_fullpath(session_fpath, path);
    base_fullpath_func(base_fpath, path);

    // when file exists in the session layer
    if (access(session_fpath, F_OK) == 0) {
        // try to delete file in session layer
        if (unlink(session_fpath) == -1) {
            perror("unlink: Error deleting from session layer");
            return -errno; // error if delete fails
        }
        printf("unlink: File successfully deleted from session layer: %s\n", session_fpath);
        return 0; // unlinked
    }

    // when file exists only in the base layer
    if (access(base_fpath, F_OK) == 0) {
        // .deleted marker for deleted file in session layer
        char deleted_marker[PATH_MAX];
        snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_fpath);
        int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
        if (fd == -1) {
            perror("unlink: Error creating .deleted marker");
            return -errno; // error if marker fails being created
        }
        close(fd);
        printf("unlink: File in base layer masked with .deleted marker: %s\n", deleted_marker);
        return 0; // masked
    }

    // if file is not found in any layer
    printf("unlink: File not found in session or base layer: %s\n", path);
    return -ENOENT; // file not found
}

// utimensat operation function implementation (POSIX)
static int myfs_utimens(const char *path, const struct timespec ts[2])
{
    char fpath[PATH_MAX];

    // update times in the session layer
    session_fullpath(fpath, path);

    int res = utimensat(0, fpath, ts, AT_SYMLINK_NOFOLLOW);
    if (res == -1)
        return -errno;

    return 0;
}

// FUSE operations
static struct fuse_operations myfs_oper = {
    .getattr  = myfs_getattr,
    .readdir  = myfs_readdir,
    .open     = myfs_open,
    .read     = myfs_read,
    .write    = myfs_write,
    .truncate = myfs_truncate,
    .create   = myfs_create,
    .utimens  = myfs_utimens,
    .unlink   = myfs_unlink,
    .chmod    = myfs_chmod,
    .mkdir    = myfs_mkdir,
    .rmdir    = myfs_rmdir
    // extend any other operations 
};

int main(int argc, char *argv[])
{
    // POSIX-compliant version flag
    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("PrismaFS Version: %s\n", PRISMAFS_VERSION);
        return 0;
    }

    // session path is passed via environment variable
    char *session_dir = getenv("SESSION_LAYER_DIR");
    if (session_dir == NULL)
    {
        fprintf(stderr, "SESSION_LAYER_DIR environment variable is not set.\n");
        exit(1);
    }

    strncpy(session_path, session_dir, PATH_MAX - 1);
    session_path[PATH_MAX - 1] = '\0'; 

    // base path environment variable
    char *base_dir = getenv("BASE_LAYER_DIR");
    if (base_dir != NULL)
    {
        strncpy(base_path, base_dir, PATH_MAX - 1);
        base_path[PATH_MAX - 1] = '\0'; 
    }
    else
    {
        strncpy(base_path, base_path_initial, PATH_MAX - 1);
        base_path[PATH_MAX - 1] = '\0'; 
    }

    return fuse_main(argc, argv, &myfs_oper, NULL);
}