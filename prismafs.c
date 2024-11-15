#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>    // For close, pread
#include <limits.h>    // For PATH_MAX
#include <sys/stat.h>

static const char *root_path = "/"; // The root path to mirror

// Helper function to construct the real path
static void fullpath(char fpath[PATH_MAX], const char *path)
{
    snprintf(fpath, PATH_MAX, "%s%s", root_path, path);
}

static int myfs_getattr(const char *path, struct stat *stbuf)
{
    char fpath[PATH_MAX];
    fullpath(fpath, path);

    int res = lstat(fpath, stbuf);
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
    fullpath(fpath, path);

    dp = opendir(fpath);
    if (dp == NULL)
        return -errno;

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
    return 0;
}

static int myfs_open(const char *path, struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    fullpath(fpath, path);

    int res = open(fpath, fi->flags);
    if (res == -1)
        return -errno;

    close(res);
    return 0;
}

static int myfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    fullpath(fpath, path);

    int fd = open(fpath, O_RDONLY);
    if (fd == -1)
        return -errno;

    int res = pread(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return res;
}

// Define operations
static struct fuse_operations myfs_oper = {
    .getattr = myfs_getattr,
    .readdir = myfs_readdir,
    .open    = myfs_open,
    .read    = myfs_read,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &myfs_oper, NULL);
}