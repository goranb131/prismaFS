#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>    // For close, pread, pwrite
#include <limits.h>    // For PATH_MAX
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>

static const char *base_path = "/"; // The base layer (real root filesystem)
static char session_path[PATH_MAX]; // The session layer (writable per pane)

// Helper function to construct the full path in the session layer
static void session_fullpath(char fpath[PATH_MAX], const char *path)
{
    snprintf(fpath, PATH_MAX, "%s%s", session_path, path);
}

// Helper function to construct the full path in the base layer
static void base_fullpath(char fpath[PATH_MAX], const char *path)
{
    snprintf(fpath, PATH_MAX, "%s%s", base_path, path);
}

static int myfs_getattr(const char *path, struct stat *stbuf)
{
    char fpath[PATH_MAX];
    int res;

    // First, check if the file exists in the session layer
    session_fullpath(fpath, path);
    res = lstat(fpath, stbuf);
    if (res == 0)
        return 0;

    // If not, check in the base layer
    base_fullpath(fpath, path);
    res = lstat(fpath, stbuf);
    if (res == -1)
        return -errno;

    return 0;
}

static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
    DIR *dp;
    struct dirent *de;
    char fpath[PATH_MAX];

    // Read from session layer first
    session_fullpath(fpath, path);
    dp = opendir(fpath);
    if (dp != NULL)
    {
        while ((de = readdir(dp)) != NULL)
        {
            // Skip hidden files
            if (de->d_name[0] == '.')
                continue;

            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (filler(buf, de->d_name, &st, 0))
                break;
        }
        closedir(dp);
    }

    // Now read from base layer, skipping files already in session layer
    base_fullpath(fpath, path);
    dp = opendir(fpath);
    if (dp == NULL)
        return -errno;

    while ((de = readdir(dp)) != NULL)
    {
        // Skip hidden files
        if (de->d_name[0] == '.')
            continue;

        // Skip files that exist in the session layer
        char session_file[PATH_MAX];
        session_fullpath(session_file, path);
        strcat(session_file, "/");
        strcat(session_file, de->d_name);
        if (access(session_file, F_OK) == 0)
            continue;

        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;

        if (filler(buf, de->d_name, &st, 0))
            break;
    }
    closedir(dp);

    return 0;
}

static int myfs_open(const char *path, struct fuse_file_info *fi)
{
    int res;
    char fpath[PATH_MAX];

    // First, try to open the file in the session layer
    session_fullpath(fpath, path);
    res = open(fpath, fi->flags);
    if (res != -1)
    {
        close(res);
        return 0;
    }

    // If not in session layer, try the base layer
    base_fullpath(fpath, path);
    res = open(fpath, fi->flags);
    if (res == -1)
        return -errno;

    close(res);
    return 0;
}

static int myfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    int fd;
    int res;
    char fpath[PATH_MAX];

    // Try to read from the session layer first
    session_fullpath(fpath, path);
    fd = open(fpath, O_RDONLY);
    if (fd == -1)
    {
        // If not in session layer, read from base layer
        base_fullpath(fpath, path);
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

static int myfs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
    int fd;
    int res;
    char fpath[PATH_MAX];

    // Write operations should happen in the session layer
    session_fullpath(fpath, path);

    // If the file doesn't exist in the session layer, copy it from the base layer
    if (access(fpath, F_OK) == -1)
    {
        char base_fpath[PATH_MAX];
        base_fullpath(base_fpath, path);

        // Create directories if needed
        char *dir_end = strrchr(fpath, '/');
        if (dir_end)
        {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        // Copy the file from the base layer
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

    // Now open the file in the session layer for writing
    fd = open(fpath, O_WRONLY);
    if (fd == -1)
        return -errno;

    res = pwrite(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return res;
}

static int myfs_truncate(const char *path, off_t size)
{
    char fpath[PATH_MAX];

    // Truncate should happen in the session layer
    session_fullpath(fpath, path);

    // If the file doesn't exist in the session layer, copy it from the base layer
    if (access(fpath, F_OK) == -1)
    {
        char base_fpath[PATH_MAX];
        base_fullpath(base_fpath, path);

        // Create directories if needed
        char *dir_end = strrchr(fpath, '/');
        if (dir_end)
        {
            char dir_path[PATH_MAX];
            strncpy(dir_path, fpath, dir_end - fpath);
            dir_path[dir_end - fpath] = '\0';
            mkdir(dir_path, 0755);
        }

        // Copy the file from the base layer
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

    // Now truncate the file in the session layer
    int res = truncate(fpath, size);
    if (res == -1)
        return -errno;

    return 0;
}

static int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int res;
    char fpath[PATH_MAX];

    // Create the file in the session layer
    session_fullpath(fpath, path);

    // Create directories if needed
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

static int myfs_utimens(const char *path, const struct timespec ts[2])
{
    char fpath[PATH_MAX];

    // Update times in the session layer
    session_fullpath(fpath, path);

    int res = utimensat(0, fpath, ts, AT_SYMLINK_NOFOLLOW);
    if (res == -1)
        return -errno;

    return 0;
}

// Define operations
static struct fuse_operations myfs_oper = {
    .getattr  = myfs_getattr,
    .readdir  = myfs_readdir,
    .open     = myfs_open,
    .read     = myfs_read,
    .write    = myfs_write,
    .truncate = myfs_truncate,
    .create   = myfs_create,
    .utimens  = myfs_utimens,
    // You can add more operations as needed
};

int main(int argc, char *argv[])
{
    // The session path is passed via an environment variable
    char *session_dir = getenv("SESSION_LAYER_DIR");
    if (session_dir == NULL)
    {
        fprintf(stderr, "SESSION_LAYER_DIR environment variable is not set.\n");
        exit(1);
    }
    strncpy(session_path, session_dir, PATH_MAX);

    // Optionally, you can set base_path via an environment variable
    char *base_dir = getenv("BASE_LAYER_DIR");
    if (base_dir != NULL)
    {
        base_path = base_dir;
    }

    return fuse_main(argc, argv, &myfs_oper, NULL);
}