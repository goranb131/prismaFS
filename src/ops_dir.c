#include "prismafs.h"

// readdir operation function implementation
#if FUSE_USE_VERSION >= 30
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi,
                 enum fuse_readdir_flags flags) {
    (void) flags;
#else
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
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
            st.st_mode = S_IFREG | 0444; // regular file, read-only permissions

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
            char session_dir[PATH_MAX];
            session_fullpath(session_dir, path);
            snprintf(marker_fpath, PATH_MAX, "%s/%s.deleted", session_dir, de->d_name);

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
    // filename list cleanup
    current = filename_list;
    while (current != NULL) {
        next = current->next;
        free(current->name);
        free(current);
        current = next;
    }

    return 0;
}

// mkdir operation func implementation
int myfs_mkdir(const char *path, mode_t mode) {
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
int myfs_rmdir(const char *path) {
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
