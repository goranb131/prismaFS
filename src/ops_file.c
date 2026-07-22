#include "prismafs.h"

// open operation func implementation
int myfs_open(const char *path, struct fuse_file_info *fi)
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

    /* file is in base layer — writes will CoW into session via myfs_write/truncate.
     * do NOT open with fi->flags here: flags may contain O_WRONLY|O_TRUNC which
     * would truncate the base file directly. Just confirm existence and return. */
    if (base_fullpath_func(fpath, path) == 0)
        return 0;
    

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
            if (offset + size > (size_t)len)
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

        if (source_fd != -1) close(source_fd);
        if (dest_fd   != -1) close(dest_fd);
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

        if (source_fd != -1) close(source_fd);
        if (dest_fd   != -1) close(dest_fd);
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
