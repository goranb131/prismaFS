/* ============================================================
   PrismaFS - ops_meta.c
   
   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */
#include "prismafs.h"

// getattr operation function implementation
#if FUSE_USE_VERSION >= 30
int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
#else
int myfs_getattr(const char *path, struct stat *stbuf) {
#endif

    memset(stbuf, 0, sizeof(struct stat));

    // handle special /dev directory and /dev/cpu file
    if (strcmp(path, "/") == 0 || strcmp(path, "/dev") == 0) {
        stbuf->st_mode  = S_IFDIR | 0755; // directory permissions
        stbuf->st_nlink = 2;
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

        stbuf->st_mode  = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size  = content_length;
        return 0;
    }

    char fpath[PATH_MAX];
    int res;

    // check session layer
    session_fullpath(fpath, path);

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
    for (int i = 0; i < num_base_layers; i++) {
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

// access operation func implementation
int myfs_access(const char *path, int mask) {
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

    // search base layers for the file and overwrite fpath with path to where it was found
    if (base_fullpath_func(fpath, path) == 0) {
        if (access(fpath, mask) == 0)
            return 0;
        return -errno;
    }

    return -ENOENT;
}

// chmod operation func implementation
#if FUSE_USE_VERSION >= 30
int myfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void) fi;
#else
int myfs_chmod(const char *path, mode_t mode)
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
            int cow_ret = cow_file(base_fpath, fpath, 0644);
            if (cow_ret != 0) return cow_ret;
        }
    }

    if (chmod(fpath, mode) == -1)
        return -errno;
    return 0;
}

// unlink operation func implementation
int myfs_unlink(const char *path)
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

    // when file exists only in base layer
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
int myfs_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi)
{
    (void) fi;
#else
int myfs_utimens(const char *path, const struct timespec ts[2])
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
int myfs_rename(const char *from, const char *to, unsigned int flags)
{
    (void) flags;
#else
int myfs_rename(const char *from, const char *to)
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
        // copy (CoW) source from BASE to new session path
        // keep original mode. old path gets .deleted marker 
        int cow_ret = cow_file(base_from, session_to, st.st_mode & 0666);
        if (cow_ret != 0) return cow_ret;
    }

    // mask the original path in session layer
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", session_from);
    int fd = open(deleted_marker, O_WRONLY | O_CREAT, 0644);
    if (fd != -1) close(fd);

    return 0;
}

// chown operation func implementation
// lchown used so symlinks are handled directly
#if FUSE_USE_VERSION >= 30
int myfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    (void) fi;
#else
int myfs_chown(const char *path, uid_t uid, gid_t gid)
{
#endif
    char fpath[PATH_MAX];
    char base_fpath[PATH_MAX];

    session_fullpath(fpath, path);

    /* chown must not touch the base layer directly.
       if the file only lives in base, CoW it into session first,
       then apply ownership change to the session copy */
    struct stat st;
    if (lstat(fpath, &st) == -1) {
        if (errno != ENOENT)
            return -errno;

        // not in session — look in base layers
        if (base_fullpath_func(base_fpath, path) == -1)
            return -ENOENT;

        // ensure parent directory exists in session layer
        char *dir_end = strrchr(fpath, '/');
        if (dir_end && dir_end != fpath) {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        if (lstat(base_fpath, &st) == -1)
            return -errno;

        if (S_ISLNK(st.st_mode)) {
            // symlink CoW: read target then recreate in session
            char link_target[PATH_MAX];
            ssize_t len = readlink(base_fpath, link_target, sizeof(link_target) - 1);
            if (len == -1) return -errno;
            link_target[len] = '\0';
            if (symlink(link_target, fpath) == -1 && errno != EEXIST)
                return -errno;
        } else if (S_ISDIR(st.st_mode)) {
            if (mkdir(fpath, st.st_mode & 0777) == -1 && errno != EEXIST)
                return -errno;
        } else {
            // CoW copy file to session before chown
            // after that, lchown on session copy.
            int cow_ret = cow_file(base_fpath, fpath, 0644);
            if (cow_ret != 0) return cow_ret;
        }
    }

    // apply ownership change to session copy
    // lchown doesn't go through symlinks. change symlink itself if one
    if (lchown(fpath, uid, gid) == -1)
        return -errno;
    return 0;
}
// symlinks go to session layer never modifying base
int myfs_symlink(const char *target, const char *linkpath)
{
    char session_fpath[PATH_MAX];
    session_fullpath(session_fpath, linkpath);
    /*translates virtual path into real path on disk inside session dir. 
      so for example /mylink becomes /path/to/session/mylink.*/


    // create parent dirs in session (if applicable):

    /*strrchr finds last / in full session path, everything before is parent dir. 
    for example: if session_fpath is /session/subdir/mylink then dir_end points to /mylink part, so dir_path is /session/subdir. 
    mkdir creates it. */
    char *dir_end = strrchr(session_fpath, '/');

    // dir_end != session_fpath avoids creating root itself /
    if (dir_end && dir_end != session_fpath) { 
        char dir_path[PATH_MAX];
        strncpy(dir_path, session_fpath, dir_end - session_fpath);
        dir_path[dir_end - session_fpath] = '\0';
        mkdir(dir_path, 0755);
    }

    if (symlink(target, session_fpath) == -1)
        return -errno;
    return 0;
}

// readlink operation func implementation
// reading symlink target. checks session and .deleted markers, then falls back to base layers
int myfs_readlink(const char *path, char *buf, size_t size)
{
    char fpath[PATH_MAX];

    // check session layer
    session_fullpath(fpath, path);

    // is there a .deleted marker?
    char deleted_marker[PATH_MAX];
    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
    if (access(deleted_marker, F_OK) == 0)
        return -ENOENT;

    /*call readlink on session path. readlink syscall reads what symlink points to 
    and if file exists there then returns number of bytes written, 
    so res != -1 means success. 
    */
    // readlink does not follow the link, reads its target
    ssize_t res = readlink(fpath, buf, size - 1);
    if (res != -1) {
        buf[res] = '\0'; // readlink doesnt null terminate, must do it manually
        return 0;
    }
    if (errno != ENOENT)
        return -errno;

    // base layers
    if (base_fullpath_func(fpath, path) == 0) {
        res = readlink(fpath, buf, size - 1);
        if (res == -1)
            return -errno;
        buf[res] = '\0';
        return 0;
    }

    return -ENOENT;
}

/* 
// helper func - CoW any type of BASE entry into session, 
// copying existing xattrs. used by setxattr and removexattr 
// before modifying xattrs.
// 
*/
static int cow_entry_with_xattrs(const char *path,
                                  const char *session_fpath,
                                  const char *base_fpath)
{
    // create parent dirs in session, find last / in session path 
    // (everything before is parent dir) and create with mkdir
    char *dir_end = strrchr(session_fpath, '/');

    if (dir_end && dir_end != session_fpath/* dont mkdir root / */) 
    {
        char dir_path[PATH_MAX];
    
        strncpy(dir_path, session_fpath, dir_end - session_fpath);
        dir_path[dir_end - session_fpath] = '\0';
        mkdir(dir_path, 0755);
    }

    struct stat st;
    
    // get file type and permissions for what will be CoW copied
    if (lstat(base_fpath, &st) == -1) 
     return -errno;

    if (S_ISLNK(st.st_mode)) // if symlink, cant use open(), read(), etc 
    // instead using readlink() to see what symlink points to and symlink() 
    // to create it in session
    {
        char link_target[PATH_MAX];

        ssize_t len = readlink(base_fpath, link_target, sizeof(link_target) - 1);
    
        if (len == -1) 
         return -errno;
        
         link_target[len] = '\0';
        
         if (symlink(link_target, session_fpath) == -1 && errno != EEXIST)
            return -errno;
    } else if (S_ISDIR(st.st_mode)) { // if directory
        // dirs have nothing to copy, simply create them in session with same permissions
        if (mkdir(session_fpath, st.st_mode & 0777) == -1 && errno != EEXIST)
            return -errno;
    } else { // if regular file
        // cow_file() to copy content 
        if (cow_file(base_fpath, session_fpath, st.st_mode & 0666) != 0)
            return -EIO;
    }

    // copy existing xattrs for session copy
    // before caller of this func modifies or removes
    cow_xattrs(base_fpath, session_fpath);

    return 0;
}

// getxattr operation func implementation
/* reads xattr from session layer 
 macOS FUSE has uint32_t position
 Linux uses lgetxattr() */
#ifdef __APPLE__
int myfs_getxattr(const char *path, const char *name, char *value, size_t size, uint32_t position)
{
    (void) position; // 0 for normal attributes
#else
int myfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
#endif
    char fpath[PATH_MAX];
    session_fullpath(fpath, path);

    // check .deleted marker to not return xattr for deleted files
    char deleted_marker[PATH_MAX];

    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
   
    if (access(deleted_marker, F_OK) == 0)
        return -ENOENT;

    // try session layer
    ssize_t res;

#ifdef __APPLE__
    // read xattr with name from session path to get num of bytes
    // of "value" or -1 on error
    res = getxattr(fpath, name, value, size, 0, XATTR_NOFOLLOW);
#else
    // if non-Apple
    res = lgetxattr(fpath, name, value, size);
#endif
    // success
    if (res != -1) 
     return (int)res;
    
    // error (other than file not found or exists but no xattr with that name)
    if (errno != ENOATTR && errno != ENOENT) 
     return -errno;

    // if xattr not in session, check base layers
    if (base_fullpath_func(fpath, path) == 0) {
#ifdef __APPLE__
        res = getxattr(fpath, name, value, size, 0, XATTR_NOFOLLOW);
#else
        res = lgetxattr(fpath, name, value, size);
#endif
        // found, return bytes 
        if (res != -1) 
         return (int)res;
        
         // exists in base but no xattr or any other error
         return -errno;
    }

    return -ENOATTR;
}

// setxattr operation func implementation
// CoW file into session with existing xattrs then set "attr"
#ifdef __APPLE__
int myfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position)
{
    (void) position;
#else
int myfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
#endif

    char session_fpath[PATH_MAX];
    session_fullpath(session_fpath, path);

    // if file not in session yet, CoW it with its xattrs first
    struct stat st;
    // does file exist in session? if lstat fails, file's not there.
    if (lstat(session_fpath, &st) == -1) {

        // other than "file not found", exit as theres real error
        if (errno != ENOENT) 
         return -errno;

        char base_fpath[PATH_MAX];

        if (base_fullpath_func(base_fpath, path) == -1) 
         return -ENOENT;

        /* copy file into session, with xattrs */
        int ret = cow_entry_with_xattrs(path, session_fpath, base_fpath);
        
        // when above is called session_fpath is on disk with content and xattrs
        if (ret != 0) 
         return ret;
    }

    // once above is done having existing session copy:
#ifdef __APPLE__
    // write new xattr into session copy
    if (setxattr(session_fpath, name, value, size, 0, flags | XATTR_NOFOLLOW) == -1)
        return -errno;
#else
    // for non-Apple:
    if (lsetxattr(session_fpath, name, value, size, flags) == -1)
        return -errno;
#endif
    return 0;
}

/* listxattr operation func implementation
   - different from getxattr as it returns list of xattr names instead single value.

// list xattrs from session layer 
// return bytes 
*/
int myfs_listxattr(const char *path, char *list, size_t size)
{ // size param has 2 modes handled internally by syscall:
  // can be size 0 and list NULL - get only byte size needed by FUSE
  // >0 and list not NULL - fill the buffer and get byte size written
    char fpath[PATH_MAX];
    session_fullpath(fpath, path);

    // check .deleted marker
    char deleted_marker[PATH_MAX];

    snprintf(deleted_marker, PATH_MAX, "%s.deleted", fpath);
    
    if (access(deleted_marker, F_OK) == 0)
        return -ENOENT;

    ssize_t res;

#ifdef __APPLE__
    // on session path
    res = listxattr(fpath, list, size, XATTR_NOFOLLOW);
#else
    // non-Apple
    res = llistxattr(fpath, list, size);
#endif

    // success, get bytes
    if (res != -1) 
     return (int)res;
    
    // stop if anything other than "file not found" in session
    if (errno != ENOENT) 
      return -errno;

    // search base layers
    if (base_fullpath_func(fpath, path) == 0) {
#ifdef __APPLE__
        res = listxattr(fpath, list, size, XATTR_NOFOLLOW);
#else
        res = llistxattr(fpath, list, size);
#endif
       
        if (res != -1) 
         return (int)res;
        
        return -errno; // something went wrong reading xattrs, return what
    }

    return 0; // file not found in any layer, 0 xattrs
}

// removexattr operation func implementation
// CoWs file into session with existing xattrs, then remove "attr"
int myfs_removexattr(const char *path, const char *name)
{
    char session_fpath[PATH_MAX];
    session_fullpath(session_fpath, path);

    // if file not in session yet, CoW it with its xattrs first
    struct stat st;

    if (lstat(session_fpath, &st) == -1) 
    { // does file exist in session

        // if not, stop (assuming error is other than "file not found")
        if (errno != ENOENT) 
         return -errno;
        
        char base_fpath[PATH_MAX];
        
        // not in BASE, return "not found"
        if (base_fullpath_func(base_fpath, path) == -1) 
         return -ENOENT;

        // file in BASE, CoW it into session with xattrs
        int ret = cow_entry_with_xattrs(path, session_fpath, base_fpath);
        
        if (ret != 0) 
         return ret;
    }

    // with above guaranteed, remove xattr from session copy:
#ifdef __APPLE__
    if (removexattr(session_fpath, name, XATTR_NOFOLLOW) == -1)
        return -errno;
#else
    if (lremovexattr(session_fpath, name) == -1)
        return -errno;
#endif
    return 0;
}
