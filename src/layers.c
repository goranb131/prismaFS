/* ============================================================
   PrismaFS - layers.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */
#include "prismafs.h"

// multiple base layers can be combined for session view in single mount
char base_paths[MAX_BASE_LAYERS][PATH_MAX];
int  num_base_layers = 0;

char session_path[PATH_MAX]; // session layer

// helper func to construct full path in the session layer
void session_fullpath(char fpath[PATH_MAX], const char *path)
{
    // snprintf builds string into a buffer
    if (session_path[strlen(session_path) - 1] == '/' && path[0] == '/')
        // PATH_MAX avoids buffer overflow
        // path + 1 skips leading "/" on path to avoid double slash //
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path);
}

// walks through all base layers in order and builds the full path to the file.
// returns 0 and fills fpath if the file is found in any base layer, -1 if not found in any.
int base_fullpath_func(char fpath[PATH_MAX], const char *path) {
    for (int i = 0; i < num_base_layers; i++) {
        // avoid double slash on joining base and file path
        if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
        else
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

        // check if the file actually exists at this location
        if (access(fpath, F_OK) == 0) {
            return 0; // found it, fpath is now set to the real location
        }
    }
    return -1; // not found in any base layer
}

// helper func for checking if filename is in linked list
int is_in_list(struct filename_node *filename_list, const char *name) {
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
        if (strcmp(current->name, name) == 0)
            return 1;
        current = current->next;
    }
    return 0;
}

// helper to add filename to linked list
void add_to_list(struct filename_node **filename_list_ptr, const char *name) {
    // memory chunk for filename_node struct, malloc returns pointer to allocated memory, if NULL then allocation failed
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


/* 
-------------------------------------------------
FIX: If we consider case where disk is almost full, and if we would
try to write or chmod large file that exists in base layer then CoW copy would fail but silently. 
it would appear to succeed without error but it would be corrupted.

nw != nr catches incomplete writes and errors and unlink() any corruption left in session
-------------------------------------------------

- copies file from src to destination (dst) with given mode.
- check write() return on each chunk, if failure ,remove partial dest
- so no corrupt half-written file is left in the session layer.
- 0 = success, -errno = failure. */
int cow_file(const char *src, const char *dst, mode_t mode)
{
    // opening source file for reading 
    int src_fd = open(src, O_RDONLY);

    if (src_fd == -1)
        return -errno;

    // open dest file for writing
    // O_CREAT = create if doesnt exist, O_TRUNC = if exists, wipe
    // "mode" is file permissions 
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (dst_fd == -1) {
        int err = errno;
        close(src_fd);
        return -err;
    }

    char buf[8192]; // chunked 8kb buffer
    ssize_t nr, nw; // bytes read and bytes written
    int ret = 0; // ret != 0 for error

    // read...
    while ((nr = read(src_fd, buf, sizeof(buf))) > 0) {
        nw = write(dst_fd, buf, (size_t)nr);
        if (nw != nr) {
           // if write returned less bytes than expected theres disk issue
           // so should break with error 
            ret = -EIO;
            break;
        }
    }
    if (nr == -1)
        ret = -errno; // read fail

    close(src_fd);
    close(dst_fd);

    if (ret != 0)
        unlink(dst); // design decision = delete incomplete or corrupt content

    return ret;
}

/* copies all extended attributes (regular files, directories, symlinks) from src to dest
  - for symlinks need to make sure to copy symlink xattrs , not the targets pointed to by symlink - using
    XATTR_NOFOLLOW on macOS and l prefix functions on Linux.

 called after cow_file() so the session copy inherits base-layer xattrs
 before setxattr/removexattr modifies them.
 4/6-arg xattr syscalls with XATTR_NOFOLLOW on macOS, and
 lgetxattr/lsetxattr on Linux. 

Function per platform: 

note: just like lstat is used instead of stat, Linux has functions with l prefix, macOS uses XATTR_NOFOLLOW flag

// macOS:
// getxattr(src, name, val, val_size, 0, XATTR_NOFOLLOW)
//                                 
//                                 

// Linux: 
// lgetxattr(src, name, val, val_size)
// no position and no options, symlink is handled by l-prefix
 */

int cow_xattrs(const char *src, const char *dst)
{
#ifdef __APPLE__
    // get the total size needed for the full xattr name list
    ssize_t list_size = listxattr(src, NULL, 0, XATTR_NOFOLLOW);
   
    if (list_size <= 0) return 0; // no xattrs or error 

    char *list = malloc(list_size);

    if (!list) return -ENOMEM;

    if (listxattr(src, list, list_size, XATTR_NOFOLLOW) == -1) {
        free(list);
        return -errno;
    }

    // list is sequence like: "name1\0name2\0..."
    // loop and copy attr values to dest
    char *name = list;

    while (name < list + list_size) {
        // NULL buffer and size 0. just checking value byte size:
        ssize_t val_size = getxattr(src, name, NULL, 0, 0, XATTR_NOFOLLOW);
       
        // if its not 0 or negative(error), allocate buff size:
        if (val_size > 0) 
        {
            char *val = malloc(val_size);
        
            // malloc fail guard:
            if (val) {
                // now with buffer, reading value into val, with return to equal val_size for read confirmation
                if ( getxattr(src, name, val, val_size, 0, XATTR_NOFOLLOW) == val_size)
                 
                    // write above value to dest file using name
                    setxattr(dst, name, val, val_size, 0, XATTR_NOFOLLOW);
                
                 free(val);
            }
        }
        name += strlen(name) + 1; // next name in list
    }
    free(list);
#else
    ssize_t list_size = llistxattr(src, NULL, 0);

    if (list_size <= 0) return 0;

    char *list = malloc(list_size);

    if (!list) return -ENOMEM;

    if (llistxattr(src, list, list_size) == -1) {
        free(list);
        return -errno;
    }

    char *name = list;
    while (name < list + list_size) {
        ssize_t val_size = lgetxattr(src, name, NULL, 0);
        if (val_size > 0) {
            char *val = malloc(val_size);
            if (val) {
                if (lgetxattr(src, name, val, val_size) == val_size)
                    lsetxattr(dst, name, val, val_size, 0);
                free(val);
            }
        }
        name += strlen(name) + 1;
    }
    free(list);
#endif
    return 0;
}
