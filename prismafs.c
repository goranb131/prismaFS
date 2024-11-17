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

static const char *base_path_initial = "/"; // Initial base layer path
static char base_path[PATH_MAX] = "/";
static char session_path[PATH_MAX]; // The session layer (writable per pane)

// Helper function to construct the full path in the session layer
static void session_fullpath(char fpath[PATH_MAX], const char *path)
{
    if (session_path[strlen(session_path) - 1] == '/' && path[0] == '/')
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path);
}

// Helper function to construct the full path in the base layer
static void base_fullpath_func(char fpath[PATH_MAX], const char *path)
{
    if (base_path[strlen(base_path) - 1] == '/' && path[0] == '/')
        snprintf(fpath, PATH_MAX, "%s%s", base_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", base_path, path);
}

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

static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
    DIR *dp_session, *dp_base;
    struct dirent *de_session, *de_base;
    char session_fpath[PATH_MAX];
    char base_fpath[PATH_MAX];
    char marker_fpath[PATH_MAX];
    char session_file_path[PATH_MAX];

    // Step 1: Read files from the session layer
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

    // Step 2: Read files from the base layer, masking files in the session layer
    base_fullpath_func(base_fpath, path);
    dp_base = opendir(base_fpath);
    if (dp_base == NULL)
        return -errno;

    while ((de_base = readdir(dp_base)) != NULL)
    {
        // Skip hidden files
        if (de_base->d_name[0] == '.')
            continue;

        // Construct the path to the `.deleted` marker in the session layer
        snprintf(marker_fpath, PATH_MAX, "%s/%s.deleted", session_fpath, de_base->d_name);

        // If a `.deleted` marker exists, skip this file
        if (access(marker_fpath, F_OK) == 0)
            continue;

        // Construct the full path to the file in the session layer
        snprintf(session_file_path, PATH_MAX, "%s/%s", session_fpath, de_base->d_name);

        // If the file exists in the session layer, skip to avoid duplicates
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
    base_fullpath_func(fpath, path);
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
        base_fullpath_func(base_fpath, path);

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
        base_fullpath_func(base_fpath, path);

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

static int myfs_chmod(const char *path, mode_t mode)
{
    char fpath[PATH_MAX];

    // Try to change mode in the session layer first
    session_fullpath(fpath, path);
    if (access(fpath, F_OK) == 0) {
        if (chmod(fpath, mode) == -1) {
            perror("chmod: Error changing mode in session layer");
            return -errno;
        }
        printf("chmod: Changed mode in session layer: %s\n", fpath);
        return 0;
    }

    // If not found in session layer, try the base layer
    base_fullpath_func(fpath, path);
    if (access(fpath, F_OK) == 0) {
        if (chmod(fpath, mode) == -1) {
            perror("chmod: Error changing mode in base layer");
            return -errno;
        }
        printf("chmod: Changed mode in base layer: %s\n", fpath);
        return 0;
    }

    // If the file does not exist in either layer
    printf("chmod: File not found: %s\n", path);
    return -ENOENT;
}

static int myfs_unlink(const char *path)
{
    char session_fpath[PATH_MAX];
    char base_fpath[PATH_MAX];

    // Construct full paths
    session_fullpath(session_fpath, path);
    base_fullpath_func(base_fpath, path);

    // Case 1: File exists in the session layer
    if (access(session_fpath, F_OK) == 0) {
        if (unlink(session_fpath) == -1) {
            perror("Error deleting from session layer");
            return -errno; // Propagate error
        }
        printf("File deleted from session layer: %s\n", session_fpath);
        return 0; // Successfully unlinked
    }

    // Case 2: File exists only in the base layer
    // In the simplified design, we do nothing for files in the base layer.
    printf("File not found in session layer, ignoring base layer: %s\n", path);
    return -ENOENT; // File does not exist in the session layer
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
    .unlink   = myfs_unlink,
    .chmod    = myfs_chmod
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
    strncpy(session_path, session_dir, PATH_MAX - 1);
    session_path[PATH_MAX - 1] = '\0'; // Ensure null-termination

    // Optionally, you can set base_path via an environment variable
    char *base_dir = getenv("BASE_LAYER_DIR");
    if (base_dir != NULL)
    {
        strncpy(base_path, base_dir, PATH_MAX - 1);
        base_path[PATH_MAX - 1] = '\0'; // Ensure null-termination
    }
    else
    {
        strncpy(base_path, base_path_initial, PATH_MAX - 1);
        base_path[PATH_MAX - 1] = '\0'; // Ensure null-termination
    }

    return fuse_main(argc, argv, &myfs_oper, NULL);
}