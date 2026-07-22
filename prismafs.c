/*
 * PrismaFS: A lightweight, layered filesystem.
 * Version: 1.2.1
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



#if defined(__linux__)
#define FUSE_USE_VERSION 30
#else
#define FUSE_USE_VERSION 29
#endif
#define PRISMAFS_VERSION "1.2.1"
#define MAX_BASE_LAYERS 10


#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>     // open, and fd
#include <dirent.h>
#include <unistd.h>    // read, write, close, pread, pwrite, mkdir, rmdir, unlink
#include <limits.h>    // PATH_MAX
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <sys/sysctl.h> // For sysctl
#endif
#include <sys/utsname.h>
#include <stdint.h>     // for int64_t

static const char *base_path_initial = "/"; // initial base layer path
//static char base_path[PATH_MAX] = "/"; //commented out for older ver single directory per base layer

// for: base_paths and num_base_layers
// multiple base layers can be combined for session view in single mount
static char base_paths[MAX_BASE_LAYERS][PATH_MAX];
static int num_base_layers = 0;

static char session_path[PATH_MAX]; // session layer 

// helper func to construct full path in the session layer
static void session_fullpath(char fpath[PATH_MAX], const char *path)
{

    // snprintf builds string into a buffer
    if (session_path[strlen(session_path) - 1] == '/' && path[0] == '/')

        // PATH_MAX avoids buffer overflow
        // path + 1 skips leading "/" on path to avoid double slash //
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path);
}

// helper func to construct full path in base layer
static int base_fullpath_func(char fpath[PATH_MAX], const char *path) {
    
    for (int i = 0; i < num_base_layers; i++) {

        // avoid double slash on joining base and file path
        if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
        else
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

        // does file exist in current base layer?
        if (access(fpath, F_OK) == 0) {
            return 0; // file found 
        }
    }
    return -1; // file not found 
}

// FUSE operation implementations:

// getattr operation function implementation
#if FUSE_USE_VERSION >= 30
static int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
#else
static int myfs_getattr(const char *path, struct stat *stbuf) {
#endif
 
    memset(stbuf, 0, sizeof(struct stat));

    // handle special /dev directory and /dev/cpu file
    if (strcmp(path, "/") == 0 || strcmp(path, "/dev") == 0) {
        stbuf -> st_mode = S_IFDIR | 0755;  // directory permissions
        stbuf -> st_nlink = 2;
        return 0;
        } else if (strcmp(path, "/dev/cpu") == 0) {

    char cpu_brand[256];
    #ifdef __APPLE__
    size_t len_cpu_brand = sizeof(cpu_brand);

    if (sysctlbyname("machdep.cpu.brand_string", cpu_brand, &len_cpu_brand, NULL, 0) == -1)
        strncpy(cpu_brand, "Unknown CPU", sizeof(cpu_brand) - 1);

    #else
    
    struct utsname uts;
    if (uname(&uts) == 0)
        snprintf(cpu_brand, sizeof(cpu_brand), "%s", uts.machine);
    else
        strncpy(cpu_brand, "Unknown CPU", sizeof(cpu_brand) - 1);
    
    #endif

    size_t content_length = strlen("CPU Brand: ") + strlen(cpu_brand) + 1;

    stbuf -> st_mode = S_IFREG | 0444;
    stbuf -> st_nlink = 1;
    stbuf -> st_size = content_length;
    return 0;
}

    char fpath[PATH_MAX];
    int res;

// check session layer 
session_fullpath( fpath, path );

char deleted_marker[PATH_MAX];

snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);

// is there .deleted marker in session layer?
if (access(deleted_marker, F_OK) == 0) {
    // file marked (masked) as deleted
    return -ENOENT;
}

res = lstat(fpath, stbuf);

if (res == 0) 
  return 0;

// checking every base layer
for ( int i = 0; i < num_base_layers; i++ ) {
   
    // creating path in base layer
    if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
        snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

    // check for .deleted marker
    if (access(deleted_marker, F_OK) == 0) {
        // marked deleted
        return -ENOENT;
    }

    res = lstat(fpath, stbuf);
    if (res == 0) return 0;
}

return -ENOENT;

}

static int myfs_access(const char *path, int mask) {
   
    // root accessible and writable (writes to session layer)
    if (strcmp(path, "/") == 0)
        return 0;
   
    // /dev = synthetic read-only dir, /dev/cpu = synthetic read-only file
    if (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/cpu") == 0)
        return (mask & W_OK) ? -EACCES : 0;

    char fpath[PATH_MAX];
    char deleted_marker[PATH_MAX];

    // checking .deleted marker in session layer
    session_fullpath(fpath, path);
   
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
   
    if (access(deleted_marker, F_OK) == 0)
        return -ENOENT;

    // fpath is pointing at session layer location for this path
    // ask OS if file is there and accessible with requested permission
    if (access(fpath, mask) == 0)
        return 0;
    if (errno != ENOENT)
        return -errno;

    // searches all base layers for the file and overwrites fpath with 
    // path to where it was found 
    if ( base_fullpath_func(fpath, path) == 0 ) {
        if (access(fpath, mask) == 0)
            return 0;
        return -errno;
    }

    return -ENOENT;
}

// struct
struct filename_node {
    char *name;
    struct filename_node *next;
};

// helper func for checking if filename is in linked list
static int is_in_list(struct filename_node *filename_list, const char *name) {
    
    struct filename_node *current = filename_list;
    
    // walking linked list
    // [node1] -> [node2] -> [node3] -> NULL
    //  "foo"      "bar"      "baz"
    /*
    current starts at first node - strcmp compares node name to what its looking for (return 0 when strings are equal)
    if match then return 1, if no match then move to the next node
    when current is at the end of list (NULL), exit loop, func returns 0 (not found)
    */
    while (current != NULL) {
        if (strcmp(current -> name, name) == 0)
            return 1;
        current = current -> next;
    }

    return 0;
}

// helper to add filename to linked list
static void add_to_list(struct filename_node **filename_list_ptr, const char *name) {
   
    // memory chunk fro filename_node struct, malloc returns pointer to allocated memory, if NULL then allocation failed
    struct filename_node *new_node = malloc(sizeof(struct filename_node));
   
    // safeguard check just in case malloc fails
    if (new_node == NULL) {
        perror("malloc");
        return;
    }
   
    // make owned copy of filename string for this node to keep
    // if that copy failed, free the node and return
    new_node->name = strdup(name);
   
    if (new_node->name == NULL) {
        perror("strdup");
        free(new_node);
        return;
    }
   
    new_node->next = *filename_list_ptr;
    *filename_list_ptr = new_node;
}

// filler takes 5 args on FUSE3 (Linux), 4 args on FUSE2 (macOS)
#if FUSE_USE_VERSION >= 30
#define FUSE_FILL(buf, name, st, off) filler(buf, name, st, off, 0)
#else
#define FUSE_FILL(buf, name, st, off) filler(buf, name, st, off)
#endif

// readdir operation function implementation
#if FUSE_USE_VERSION >= 30
static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void) flags;
#else
static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
#endif

    (void) offset;
    (void) fi;

    struct filename_node *filename_list = NULL;
    DIR *dp;
    struct dirent *de;
    char fpath[PATH_MAX];
    char marker_fpath[PATH_MAX];
    struct filename_node *current;
    struct filename_node *next;

    // root dir for default virtual filesystems 
    // filler is FUSE provided callback func filler(buf, name, stat, offset)
    // returns !0 if buffer is full which shouldnt happen here
    if (strcmp(path, "/") == 0) {
        // add standard entries
        // every directory listing must include "." and ".."
        FUSE_FILL(buf, ".", NULL, 0);
        FUSE_FILL(buf, "..", NULL, 0);


        // include "dev" directory
        if (!is_in_list(filename_list, "dev")) {

            struct stat st;
            memset(&st, 0, sizeof(st)); // zero out stat struct 
            st.st_mode = S_IFDIR | 0755; // mark as dir with rwxr-xr-x permissions
            
            if (FUSE_FILL(buf, "dev", &st, 0))
                goto cleanup;
            add_to_list(&filename_list, "dev");
        }
    } else if (strcmp(path, "/dev") == 0) {
        // add std entries
        FUSE_FILL(buf, ".", NULL, 0);
        FUSE_FILL(buf, "..", NULL, 0);


        // "cpu" file
        if (!is_in_list(filename_list, "cpu")) {
            
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_mode = S_IFREG | 0444;  // regular file, read-only permissions

            if (FUSE_FILL(buf, "cpu", &st, 0))
                goto cleanup;
            add_to_list(&filename_list, "cpu");
        }

        goto cleanup; // "/dev" only contains "cpu" 
    }

    // read files from session layer
    session_fullpath(fpath, path);
    dp = opendir(fpath);

    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            // skip hidden files and .deleted markers
            if (de->d_name[0] == '.' || strstr(de->d_name, ".deleted") != NULL)
                continue;

            // skip when already in linked list
            if (is_in_list(filename_list, de->d_name))
                continue;

            // add filename to linked list
            add_to_list(&filename_list, de->d_name);

            // fill directory entry
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (FUSE_FILL(buf, de->d_name, &st, 0))
                break;
        }

        closedir(dp);
    }

    // reading files from all base layers, minding .deleted markers and duplicates
    for (int i = 0; i < num_base_layers; i++) {
        // create path for CURRENT base layer
        snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);
        dp = opendir(fpath);
        if (dp == NULL)
            continue;

        while ((de = readdir(dp)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // skip when already in linked list
            if (is_in_list(filename_list, de->d_name))
                continue;

            // .deleted marker path for deleted files (in session)
            session_fullpath(marker_fpath, path);
            snprintf(marker_fpath, PATH_MAX, "%s/%s.deleted", marker_fpath, de->d_name);

            // skip files marked .deleted
            if (access(marker_fpath, F_OK) == 0)
                continue;

            // full path to check if file exists in session 
            char session_file_path[PATH_MAX];
            session_fullpath(session_file_path, path);
            snprintf(session_file_path, PATH_MAX, "%s/%s", session_file_path, de->d_name);

            // skip files in session layer
            if (access(session_file_path, F_OK) == 0)
                continue;

            // add filename to linked list
            add_to_list(&filename_list, de->d_name);

            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (FUSE_FILL(buf, de->d_name, &st, 0))
                break;
        }
        closedir(dp);
    }

// goto
cleanup:
    //  filename list cleanup
    current = filename_list;
    while (current != NULL) {
        next = current->next;
        free(current->name);
        free(current);
        current = next;
    }

    return 0;
}

// open operation func implementation
static int myfs_open(const char *path, struct fuse_file_info *fi)
{
    // opening /dev/cpu
    if (strcmp(path, "/dev/cpu") == 0) {
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

    // check .deleted marker in session 
    char deleted_marker[PATH_MAX];

    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);

    if (access(deleted_marker, F_OK) == 0) {
     
     // marked/masked as deleted
     return -ENOENT;
    }

    // if not in session layer and not masked, try in base layer
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
   
    // fill statvfs structure
    memset(stbuf, 0, sizeof(struct statvfs));
    
    stbuf -> f_bsize = 4096;
    stbuf -> f_frsize = 4096;
    stbuf -> f_blocks = 1024 * 1024;
    stbuf -> f_bfree = 1024 * 512;
    stbuf -> f_bavail = 1024 * 512;
    stbuf -> f_files = 1024 * 1024;
    stbuf -> f_ffree = 1024 * 512;
    stbuf -> f_namemax = 255;

    return 0;
}


// read operation func implementation
static int myfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    
    // for reading from /dev/cpu
    if (strcmp(path, "/dev/cpu") == 0) {
        // generate CPU stats 
        char cpu_info[1024];
        int len;

        // get CPU brand string
        char cpu_brand[256];
        #ifdef __APPLE__
        size_t len_cpu_brand = sizeof(cpu_brand);

        if (sysctlbyname("machdep.cpu.brand_string", cpu_brand, &len_cpu_brand, NULL, 0) == -1) {
            perror("sysctlbyname");
            return -EIO;
        }

        #else
        
        struct utsname uts;
        if (uname(&uts) == -1) {
            perror("uname");
            return -EIO;
        }
        
        snprintf(cpu_brand, sizeof(cpu_brand), "%s", uts.machine);
        
        #endif


        len = snprintf(cpu_info, sizeof(cpu_info), "CPU Brand: %s\n", cpu_brand);

        // for offset
        if (offset < len) {
            if (offset + size > len)
                size = len - offset;
            memcpy(buf, cpu_info + offset, size);
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
static int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void) fi;
#else
static int myfs_truncate(const char *path, off_t size)
{
#endif

    char fpath[PATH_MAX];

    // truncate happens in session layer
    session_fullpath(fpath, path);

    // copy file from base layer
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

    // truncate file
    int res = truncate(fpath, size);
    if (res == -1)
        return -errno;

    return 0;
}

// create operation func implementation
static int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
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

// mkdir operation func implementation
static int myfs_mkdir(const char *path, mode_t mode) {
  
    char fpath[PATH_MAX];

    // construct full path for directory 
    session_fullpath(fpath, path); 

    int res = mkdir(fpath, mode);
    if (res == -1) {
        return -errno;
    }
    return 0;
}

// rmdir operation func implementation
static int myfs_rmdir(const char *path) {
   
    char session_fpath[PATH_MAX];
    char base_fpath[PATH_MAX];

    session_fullpath(session_fpath, path);

    // directory exists in session layer: remove it
    if (access(session_fpath, F_OK) == 0) {
        
   /* before calling rmdir(2), remove .deleted marker files inside this
      session directory. markers are leftovers from deleted when directory was in use. 
      rmdir(2) requires dir to be empty, so clean them out first or will fail
      with ENOTEMPTY even if directory looks empty to user */

        DIR *dp = opendir(session_fpath);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL) {
                size_t len = strlen(de->d_name);
                if (len > 8 && strcmp(de->d_name + len - 8, ".deleted") == 0) {
                    char marker[PATH_MAX];
                    snprintf(marker, PATH_MAX, "%s/%s", session_fpath, de->d_name);
                    unlink(marker);
                }
            }
            closedir(dp);
        }

        if (rmdir(session_fpath) == -1)
            return -errno;

        // if in base layer, add .deleted marker so deletion is persisting on remounts
        if (base_fullpath_func(base_fpath, path) == 0) {
            char deleted_marker[PATH_MAX];
            snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_fpath);
            int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
            if (fd != -1) close(fd);
        }

        return 0;
    }

    // dir only exists in base layer: mask with .deleted marker
    if (base_fullpath_func(base_fpath, path) == 0) {
        char *dir_end = strrchr(session_fpath, '/');
        if (dir_end) {
            char dir_path[PATH_MAX];
            strncpy(dir_path, session_fpath, dir_end - session_fpath);
            dir_path[dir_end - session_fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        char deleted_marker[PATH_MAX];
        
        snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_fpath);
        
        int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
        
        if (fd == -1)
            return -errno;

        close(fd);
        
        return 0;
    }

    return -ENOENT;
}

// chmod operation func implementation
#if FUSE_USE_VERSION >= 30
static int myfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void) fi;
#else
static int myfs_chmod(const char *path, mode_t mode)
{
#endif

    char fpath[PATH_MAX];
    char base_fpath[PATH_MAX];

    session_fullpath(fpath, path);

   /* chmod needs to modify file, but base layer must not be touched directly.
      if file is only in base layer, copy it into session layer,
      then apply permission change to session copy */
    if (access(fpath, F_OK) == -1) {
        if (base_fullpath_func(base_fpath, path) == -1)
            return -ENOENT;

        // parent directory exists in session layer?
        char *dir_end = strrchr(fpath, '/');
        if (dir_end) {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        struct stat st;
        if (lstat(base_fpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (mkdir(fpath, st.st_mode & 0777) == -1 && errno != EEXIST)
                return -errno;
        } else {
            int src = open(base_fpath, O_RDONLY);
            int dst = open(fpath, O_WRONLY | O_CREAT, 0644);
            if (src != -1 && dst != -1) {
                char buf[8192];
                ssize_t n;
                while ((n = read(src, buf, sizeof(buf))) > 0)
                    write(dst, buf, n);
            }
            if (src != -1) close(src);
            if (dst != -1) close(dst);
        }
    }

    if (chmod(fpath, mode) == -1)
        return -errno;
    return 0;
}

// unlink operation func implementation
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
            return -errno;
        }
        return 0;
    }

    // when file exists only BASE layer
    if (access(base_fpath, F_OK) == 0) {
        // make sure parent directory exists in SESSION layer before creating marker
        char *dir_end = strrchr(session_fpath, '/');
        if (dir_end && dir_end != session_fpath) {
            char dir_path[PATH_MAX];
            strncpy(dir_path, session_fpath, dir_end - session_fpath);
            dir_path[dir_end - session_fpath] = '\0';
            mkdir(dir_path, 0755);
        }
        char deleted_marker[PATH_MAX];
        snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_fpath);
        int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
        if (fd == -1)
            return -errno;
        close(fd);
        return 0;
    }

    return -ENOENT;
}

// utimensat operation func implementation (POSIX)
#if FUSE_USE_VERSION >= 30
static int myfs_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi)
{
    (void) fi;
#else
static int myfs_utimens(const char *path, const struct timespec ts[2])
{
#endif

    char fpath[PATH_MAX];

    // update session layer times
    session_fullpath(fpath, path);

    int res = utimensat(0, fpath, ts, AT_SYMLINK_NOFOLLOW);
    if (res == -1)
        return -errno;

    return 0;
}

// rename operation func implementation
#if FUSE_USE_VERSION >= 30
static int myfs_rename(const char *from, const char *to, unsigned int flags)
{
    (void) flags;
#else
static int myfs_rename(const char *from, const char *to)
{
#endif

    char session_from[PATH_MAX], session_to[PATH_MAX];
    char base_from[PATH_MAX];

    session_fullpath(session_from, from);
    session_fullpath(session_to, to);

    // make sure destination parent directory exists in session layer
    char *dest_dir_end = strrchr(session_to, '/');
    if (dest_dir_end && dest_dir_end != session_to) {
        char dir_path[PATH_MAX];
        strncpy(dir_path, session_to, dest_dir_end - session_to);
        dir_path[dest_dir_end - session_to] = '\0';
        mkdir(dir_path, 0755);
    }

    // source exists in session layer: rename directly
    if (access(session_from, F_OK) == 0) {
        if (rename(session_from, session_to) == -1)
            return -errno;
        // if source also in base layer, mask the old path
        if (base_fullpath_func(base_from, from) == 0) {
            char deleted_marker[PATH_MAX];
            snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_from);
            int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
            if (fd != -1) close(fd);
        }
        return 0;
    }

    // source only in base layer: CoW to new session path + mask old path
    if (base_fullpath_func(base_from, from) == -1)
        return -ENOENT;

    struct stat st;
    if (lstat(base_from, &st) == -1)
        return -errno;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(session_to, st.st_mode & 0777) == -1 && errno != EEXIST)
            return -errno;
    } else {
        int src = open(base_from, O_RDONLY);
        if (src == -1) return -errno;
        int dst = open(session_to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0666);
        if (dst == -1) { close(src); return -errno; }
        char buf[8192];
        ssize_t n;
        while ((n = read(src, buf, sizeof(buf))) > 0)
            write(dst, buf, n);
        close(src);
        close(dst);
    }

    // mask the original path in session layer
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_from);
    int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
    if (fd != -1) close(fd);

    return 0;
}


// parse line format config file.
// directives (one per line, # for comments):
//   session <path>   - session layer directory (required once)
//   base <path>      - base layer directory (required once or more. order = priority)
static int load_config(const char *config_path)
{
    FILE *f = fopen(config_path, "r");
    if (!f) {
        fprintf(stderr, "prismafs: cannot open config file '%s': %s\n",
                config_path, strerror(errno));
        return -1;
    }

    char line[PATH_MAX + 16];
    int found_session = 0;

    while (fgets(line, sizeof(line), f)) {
        // strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        // skip leading whitespace
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        // skip empty lines and comments
        if (*p == '\0' || *p == '#') continue;

        char keyword[16];
        char value[4096];
        if (sscanf(p, "%15s %4095s", keyword, value) != 2) {
            fprintf(stderr, "prismafs: ignoring malformed config line: %s\n", p);
            continue;
        }

        if (strcmp(keyword, "session") == 0) {
            if (found_session) {
                fprintf(stderr, "prismafs: duplicate 'session' directive, ignoring: %s\n", value);
                continue;
            }
            strncpy(session_path, value, PATH_MAX - 1);
            session_path[PATH_MAX - 1] = '\0';
            found_session = 1;
        } else if (strcmp(keyword, "base") == 0) {
            if (num_base_layers >= MAX_BASE_LAYERS) {
                fprintf(stderr, "prismafs: max base layers (%d) reached, ignoring: %s\n",
                        MAX_BASE_LAYERS, value);
                continue;
            }
            strncpy(base_paths[num_base_layers], value, PATH_MAX - 1);
            base_paths[num_base_layers][PATH_MAX - 1] = '\0';
            num_base_layers++;
        } else {
            fprintf(stderr, "prismafs: unknown config directive '%s', ignoring\n", keyword);
        }
    }

    fclose(f);

    if (!found_session) {
        fprintf(stderr, "prismafs: config '%s' is missing a 'session' directive\n", config_path);
        return -1;
    }
    if (num_base_layers == 0) {
        fprintf(stderr, "prismafs: config '%s' has no 'base' directives\n", config_path);
        return -1;
    }

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
    .rmdir    = myfs_rmdir,
    .rename   = myfs_rename
    // extend operations here
};

int main(int argc, char *argv[])
{
    // POSIX version flag
    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("PrismaFS Version: %s\n", PRISMAFS_VERSION);
        return 0;
    }

    // session path is passed via environment variable
    //char *session_dir = getenv("SESSION_LAYER_DIR");
        // scan argv for -c <configfile> and build a clean argv for fuse_main
    // (FUSE doesn't know about -c and would error on it)
    const char *config_path = NULL;
    char **fuse_argv = malloc(argc * sizeof(char *));
    if (!fuse_argv) {
        fprintf(stderr, "prismafs: out of memory\n");
        return 1;
    }
    int fuse_argc = 0;


    // strncpy(session_path, session_dir, PATH_MAX - 1);
    // session_path[PATH_MAX - 1] = '\0'; 

        for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            fuse_argv[fuse_argc++] = argv[i];
        }
    }

    // base path environment variable
   /* char *base_dirs = getenv("BASE_LAYER_DIRS"); // multiple base directories "," separated
    if (base_dirs) {
        char *token = strtok(base_dirs, ",");
        while (token && num_base_layers < MAX_BASE_LAYERS) {
            strncpy(base_paths[num_base_layers], token, PATH_MAX - 1);
            base_paths[num_base_layers][PATH_MAX - 1] = '\0';
            num_base_layers++;
            token = strtok(NULL, ",");*/
    if (config_path) {
        // config file takes full precedence
        if (load_config(config_path) != 0) {
            free(fuse_argv);
           return 1;

        }
    } else {
               // fall back to environment variables for backward compatibility
        char *session_dir = getenv("SESSION_LAYER_DIR");
        if (session_dir == NULL) {
            fprintf(stderr,
                "prismafs: no config given and SESSION_LAYER_DIR is not set.\n"
                "Usage: prismafs -c <config> <mountpoint>\n"
                "       prismafs <mountpoint>   (with SESSION_LAYER_DIR / BASE_LAYER_DIRS set)\n");
            free(fuse_argv);
            return 1;
        }
        strncpy(session_path, session_dir, PATH_MAX - 1);
        session_path[PATH_MAX - 1] = '\0';

        char *base_dirs = getenv("BASE_LAYER_DIRS");
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
    }


    //return fuse_main(argc, argv, &myfs_oper, NULL);
    int ret = fuse_main(fuse_argc, fuse_argv, &myfs_oper, NULL);
    free(fuse_argv);
    return ret;

}