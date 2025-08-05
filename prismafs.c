/*
 * PrismaFS: A lightweight, layered filesystem inspired by Plan 9.
 * Version: 1.0.2
 * Copyright 2024 Goran Bunić (developed under ITHAS legal entity) 
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

#define FUSE_USE_VERSION 29
#define PRISMAFS_VERSION "1.0.2"
#define MAX_BASE_LAYERS 10


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
#include <sys/sysctl.h> // For sysctl
#include <stdint.h>     // For int64_t

static const char *base_path_initial = "/"; // initial base layer path
//static char base_path[PATH_MAX] = "/"; //commented out for older ver single directory per base layer

// for: base_paths and num_base_layers
// multiple base layers can be combined for session view in single mount
static char base_paths[MAX_BASE_LAYERS][PATH_MAX];
static int num_base_layers = 0;

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
static int base_fullpath_func(char fpath[PATH_MAX], const char *path) {
    for (int i = 0; i < num_base_layers; i++) {
        if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
        else
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

        // check if file exists in current base layer
        if (access(fpath, F_OK) == 0) {
            return 0; // file found in this base layer
        }
    }
    return -1; // file not found in any base layer
}

// getattr operation function implementation
static int myfs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));

    // Handle the special /dev directory and /dev/cpu file
    if (strcmp(path, "/") == 0 || strcmp(path, "/dev") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;  // directory permissions
        stbuf->st_nlink = 2;
        return 0;
        } else if (strcmp(path, "/dev/cpu") == 0) {
    // calculate size of content
    const char *cpu_brand = "Apple M1";  // testing value
    size_t content_length = strlen("CPU Brand: ") + strlen(cpu_brand) + 1;  // +1 for newline

    stbuf->st_mode = S_IFREG | 0444;  // regular file with read-only permissions
    stbuf->st_nlink = 1;
    stbuf->st_size = content_length;
    return 0;
}

    char fpath[PATH_MAX];
    int res;

// check session layer first
session_fullpath(fpath, path);

// check if there is .deleted marker in session layer
char deleted_marker[PATH_MAX];
snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
if (access(deleted_marker, F_OK) == 0) {
    // file marked/masked as deleted
    return -ENOENT;
}

res = lstat(fpath, stbuf);
if (res == 0) return 0;

// check each base layer
for (int i = 0; i < num_base_layers; i++) {
    // creating path in base layer
    if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
        snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

    // check if there is .deleted marker
    if (access(deleted_marker, F_OK) == 0) {
        // file marked/masked as deleted
        return -ENOENT;
    }

    res = lstat(fpath, stbuf);
    if (res == 0) return 0;
}

return -ENOENT;
}

static int myfs_access(const char *path, int mask) {
    printf("access called on path: %s with mask: %d\n", path, mask);  // debug 

    // can always return success
    return 0;
}

// define struct filename_node
struct filename_node {
    char *name;
    struct filename_node *next;
};

// helper function to check if a filename is already in linked list
static int is_in_list(struct filename_node *filename_list, const char *name) {
    struct filename_node *current = filename_list;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0)
            return 1;
        current = current->next;
    }
    return 0;
}

// helper function to add a filename to linked list
static void add_to_list(struct filename_node **filename_list_ptr, const char *name) {
    struct filename_node *new_node = malloc(sizeof(struct filename_node));
    if (new_node == NULL) {
        perror("malloc");
        return;
    }
    new_node->name = strdup(name);
    if (new_node->name == NULL) {
        perror("strdup");
        free(new_node);
        return;
    }
    new_node->next = *filename_list_ptr;
    *filename_list_ptr = new_node;
}

// readdir operation function implementation
static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    printf("myfs_readdir called on path: %s\n", path);
    fflush(stdout);

    struct filename_node *filename_list = NULL;
    DIR *dp;
    struct dirent *de;
    char fpath[PATH_MAX];
    char marker_fpath[PATH_MAX];
    struct filename_node *current;
    struct filename_node *next;

    // root directory for default virtual filesystems 
    if (strcmp(path, "/") == 0) {
        // Add standard entries
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);

        // include "dev" directory
        if (!is_in_list(filename_list, "dev")) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_mode = S_IFDIR | 0755;
            if (filler(buf, "dev", &st, 0))
                goto cleanup;
            add_to_list(&filename_list, "dev");
        }
    } else if (strcmp(path, "/dev") == 0) {
        // manage "/dev" directory
        // add standard entries
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);

        // include "cpu" file
        if (!is_in_list(filename_list, "cpu")) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_mode = S_IFREG | 0444;  // regular file with read-only permissions
            if (filler(buf, "cpu", &st, 0))
                goto cleanup;
            add_to_list(&filename_list, "cpu");
        }
        goto cleanup; // "/dev" only contains "cpu" so we can return here
    }

    // read files from session layer first
    session_fullpath(fpath, path);
    dp = opendir(fpath);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            // skip hidden files and .deleted markers
            if (de->d_name[0] == '.' || strstr(de->d_name, ".deleted") != NULL)
                continue;

            // skip if already in linked list
            if (is_in_list(filename_list, de->d_name))
                continue;

            // add filename to linked list
            add_to_list(&filename_list, de->d_name);

            // fill directory entry
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (filler(buf, de->d_name, &st, 0))
                break;
        }
        closedir(dp);
    }

    // read files from all base layers, minding .deleted markers and duplicates
    for (int i = 0; i < num_base_layers; i++) {
        // create path for the current base layer
        snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);
        dp = opendir(fpath);
        if (dp == NULL)
            continue;

        while ((de = readdir(dp)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // skip if already in linked list
            if (is_in_list(filename_list, de->d_name))
                continue;

            // .deleted marker path for deleted files in session layer
            session_fullpath(marker_fpath, path);
            snprintf(marker_fpath, PATH_MAX, "%s/%s.deleted", marker_fpath, de->d_name);

            // skip files that are marked .deleted
            if (access(marker_fpath, F_OK) == 0)
                continue;

            // create full path to check if file exists in the session layer
            char session_file_path[PATH_MAX];
            session_fullpath(session_file_path, path);
            snprintf(session_file_path, PATH_MAX, "%s/%s", session_file_path, de->d_name);

            // skip files that already exist in session layer
            if (access(session_file_path, F_OK) == 0)
                continue;

            // add filename to linked list
            add_to_list(&filename_list, de->d_name);

            // fill directory entry
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (filler(buf, de->d_name, &st, 0))
                break;
        }
        closedir(dp);
    }

cleanup:
    // clean the filename list
    current = filename_list;
    while (current != NULL) {
        next = current->next;
        free(current->name);
        free(current);
        current = next;
    }

    return 0;
}

// open operation function implementation
static int myfs_open(const char *path, struct fuse_file_info *fi)
{
    printf("open called on path: %s\n", path);  // debug

    // Handle opening /dev/cpu
    if (strcmp(path, "/dev/cpu") == 0) {
        // since /dev/cpu is a virtual file, no need to open file descriptor
        return 0;
    }

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

    // check if there is .deleted marker in session layer
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
    if (access(deleted_marker, F_OK) == 0) {
     // file is marked/masked as deleted
     return -ENOENT;
    }

    // if not in session layer and not masked, try inside base layer
    if (base_fullpath_func(fpath, path) == 0) {
     res = open(fpath, fi->flags);
     if (res == -1)
         return -errno;
     close(res);
     return 0;
    }

return -ENOENT;
}

static int myfs_statfs(const char *path, struct statvfs *stbuf) {
    printf("myfs_statfs called on path: %s\n", path);
    fflush(stdout);

    // fill statvfs structure
    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = 1024 * 1024;
    stbuf->f_bfree = 1024 * 512;
    stbuf->f_bavail = 1024 * 512;
    stbuf->f_files = 1024 * 1024;
    stbuf->f_ffree = 1024 * 512;
    stbuf->f_namemax = 255;

    return 0;
}


// read operation function implementation
static int myfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    printf("myfs_read called on path: %s\n", path);  // Debug statement
    fflush(stdout);

    // for reading from /dev/cpu
    if (strcmp(path, "/dev/cpu") == 0) {
        // generate CPU stats 
        char cpu_info[1024];
        int len;

        // get CPU brand string
        char cpu_brand[256];
        size_t len_cpu_brand = sizeof(cpu_brand);

        if (sysctlbyname("machdep.cpu.brand_string", cpu_brand, &len_cpu_brand, NULL, 0) == -1) {
            perror("sysctlbyname");
            return -EIO;
        }

        printf("Retrieved CPU brand: %s\n", cpu_brand);
        fflush(stdout);

        len = snprintf(cpu_info, sizeof(cpu_info), "CPU Brand: %s\n", cpu_brand);

        // for offset
        if (offset < len) {
            if (offset + size > len)
                size = len - offset;
            memcpy(buf, cpu_info + offset, size);
        } else {
            size = 0;
        }

        printf("Returning size: %zu\n", size);
        fflush(stdout);

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

    // check if there is .deleted marker in the session layer
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
    if (access(deleted_marker, F_OK) == 0) {
        // masked as deleted
        return -ENOENT;
    }

    // if not in session layer and not masked, try each base layer
    for (int i = 0; i < num_base_layers; i++) {
     // full path for current base layer
     if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
        else
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

            // check if file exists in current base layer
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

// file not found in any layer
return -ENOENT;
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
    .access   = myfs_access, 
    .read     = myfs_read,
    .write    = myfs_write,
    .truncate = myfs_truncate,
    .create   = myfs_create,
    .statfs   = myfs_statfs,
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
    char *base_dirs = getenv("BASE_LAYER_DIRS"); // multiple base directories "," separated
    if (base_dirs) {
        char *token = strtok(base_dirs, ",");
        while (token && num_base_layers < MAX_BASE_LAYERS) {
            strncpy(base_paths[num_base_layers], token, PATH_MAX - 1);
            base_paths[num_base_layers][PATH_MAX - 1] = '\0';
            num_base_layers++;
            token = strtok(NULL, ",");
        }
    } else {
        strncpy(base_paths[0], base_path_initial, PATH_MAX - 1);
        base_paths[0][PATH_MAX - 1] = '\0';
        num_base_layers = 1;
    }

    return fuse_main(argc, argv, &myfs_oper, NULL);
}