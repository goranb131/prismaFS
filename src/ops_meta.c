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
