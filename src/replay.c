/* ============================================================
   PrismaFS - replay.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */

/*
 *  "prismafs replay <session-dir> <target-dir>"
 *
 * Useful tool/cmd to turns session work and changes into permanent, 
 * filesystem directory, outside PrismaFS. A type of snapshot applied
 * to any target directory specified by user.
 *
 *  - copies all changed files, dirs, symlinks from session to
 *  target-dir (including deleting anything in session marked .deleted).
 *
 * NOTE: content of target-dir is overwritten with no warning or merges, 
 *  - one-time operation, no careful syncing
 *
 */

#include "prismafs.h"

static void repl_usg(FILE *out)
{
    fprintf(out, "Usage: prismafs replay <session-dir> <target-dir>\n");
}

// struct for counting to print summary 
struct rcount {
    int applied;
    int removed;
    int errors;
};

// recursive deletion of everything in path (file, symlink, tree...)
// - needed for deleting whats marked .deleted 
// - missing path is not error
static int rmrf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return 0;

    if ( !S_ISDIR(st.st_mode) )
        return unlink(path);

    DIR *dp = opendir(path);

    if (dp) {
        struct dirent *de;

        while ( (de = readdir(dp)) != NULL) 
        {
            // when recursing into sub-dirs to delete them, skip 
            // self and parents 
            if (strcmp(de -> d_name, ".") == 0 
                || strcmp(de -> d_name, "..") == 0)

                continue;

            char child[PATH_MAX];

            snprintf(child, PATH_MAX, "%s/%s", path, de->d_name);

            rmrf(child);
        }

        closedir(dp);
    }

    return rmdir(path);
}

// recreate symlink from session in target dir 
//  - called from walk() 
static int mklink(const char *src, const char *dst, const struct stat *st)
{
    char target[PATH_MAX] = {0};
    ssize_t n = readlink(src, target, sizeof(target) - 1);

    if (n < 0)
        return -1;

    target[n] = '\0';

    unlink(dst); // symlink() will fail if destination exists. clear out whats there

    if ( symlink(target, dst) /*create new link */ != 0 ) 
        return -1;

    // set owner of created symlink at dest with user and group ID 
    // st = pointer to struct stat, filled in by OS for file info,
    // populated in walk() in lstat(entry_path, &sess_st)
    //  - not checking ret value as ownershit changing is handled by OS over root, and 
    //    user running prismafs replay is not root
    lchown(dst, st -> st_uid /*user ID*/
              , st -> st_gid /*group ID*/); 

    return 0;
}

// function to walk one directory level in session tree and apply 
// every entry to matching path (target_root) and recurse into 
// subdirs 
static void walk(const char *session_root, const char *target_root,
                  const char *rel, struct rcount *rc)
{
    char dirpath[PATH_MAX];

    if (rel[0])
        snprintf(dirpath, PATH_MAX, "%s/%s", session_root, rel);

    else
        snprintf(dirpath, PATH_MAX, "%s", session_root);

    DIR *dp = opendir(dirpath);

    if (!dp)
        return;

    struct dirent *de;

    // readir iterate to report entries
    while ((de = readdir(dp)) != NULL) 
    {
        if ( strcmp(de -> d_name, ".") == 0 
           || strcmp(de->d_name, "..") == 0)

            continue;

        char entry_rel[PATH_MAX];

        if (rel[0])
            snprintf(entry_rel, PATH_MAX
                              , "%s/%s"
                              , rel, de -> d_name);
        else
            snprintf(entry_rel, PATH_MAX
                              , "%s"
                              , de -> d_name);

        char entry_path[PATH_MAX];

        snprintf(entry_path, PATH_MAX
                           , "%s/%s"
                           , dirpath, de -> d_name);

        size_t name_len = strlen(de -> d_name);
        const char *suffix = ".deleted";
        size_t suffix_len = strlen(suffix);

        // .deleted marker = this path must be removed from target 
        if (name_len > suffix_len 
            && strcmp(de -> d_name + name_len - suffix_len, suffix) == 0) 
        {
            char target_rel[PATH_MAX];
            size_t target_len = strlen(entry_rel) - suffix_len;

            snprintf( target_rel
                    , PATH_MAX
                    , "%.*s"
                    , (int)target_len
                    , entry_rel);

            char target_path[PATH_MAX];
            snprintf( target_path
                    , PATH_MAX
                    , "%s/%s"
                    , target_root
                    , target_rel);

            if (rmrf(target_path) == 0) 
            {
                printf("  removed: %s\n", target_rel);

                rc->removed++;
            }

            continue;
        }

        struct stat sess_st;

        // TOCTOU check to avoid using garbage stat data
        // - between this and prev readir step, processes could modify or removed
        //   previously read files, so skip these entries
        if ( lstat(entry_path, &sess_st) != 0 )
            continue; 

        char target_path[PATH_MAX];

        snprintf( target_path
                , PATH_MAX
                , "%s/%s"
                , target_root
                , entry_rel);

        struct stat tgt_st;
        int target_exists = (lstat(target_path, &tgt_st) == 0);

        // if something (different type) is already there, clear, 
        // so session version can replace it
        if (target_exists 
            && ((tgt_st.st_mode & S_IFMT) 
            != (sess_st.st_mode & S_IFMT)) )

            rmrf(target_path);

        // to (re)create matching directories
        if (S_ISDIR(sess_st.st_mode)) 
        {
            mkdir(target_path, sess_st.st_mode & 07777); // ignore EEXIST, following chmod fixes mode

            chmod(target_path, sess_st.st_mode & 07777);
            chown(target_path, sess_st.st_uid, sess_st.st_gid); 

            //recurssion for own children contents
            walk(session_root, target_root, entry_rel, rc);

            continue; // when done with iterations exit not to reach cow_file 
            // as its not applicable for directories
        }

        // to recreate symlinks
        if (S_ISLNK(sess_st.st_mode)) 
        {
            if ( mklink(entry_path, target_path, &sess_st) == 0 ) 
            {
                printf("  applied: %s\n", entry_rel);

                rc -> applied++;

            } else {

                fprintf(stderr
                       , "prismafs replay: could not recreate symlink '%s': %s\n"
                       , entry_rel
                       , strerror(errno) );

                rc -> errors++;
            }

            continue; // exit as following cow_file() file creation is not applicable for symlinks
        }

        // create file if doesnt exist, or overwrite if does, 
        // copy content and metadata from session
        int cow_ret = cow_file( entry_path
                              , target_path
                              , sess_st.st_mode & 07777);

        if (cow_ret != 0) 
        {
            fprintf( stderr
                   , "prismafs replay: could not apply '%s': %s\n"
                   , entry_rel
                   , strerror(-cow_ret));

            rc -> errors++;

            continue;
        }

        /* if new file was created or overwritten by cow_file(), following
           sets permissions to match session 
           - cow_file() uses open() syscall and its passed mode arg, 
            open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode), and sets
            permissions only if file doesnt exist, but if it does it 
            just opens it so mode gets ignored, hence additional chmod/chown
            bellow */
        chmod( target_path
             , sess_st.st_mode & 07777);

        chown( target_path
             , sess_st.st_uid
             , sess_st.st_gid); 

        cow_xattrs( entry_path
                  , target_path);

        printf("  applied: %s\n", entry_rel);

        rc -> applied++;
    }

    closedir(dp);
}

int replay_command(int argc, char **argv)
{
    if ( argc < 4 
        || strcmp(argv[2], "-h") == 0 
        || strcmp(argv[2], "--help") == 0) 
    {
        repl_usg(argc < 4 ? stderr : stdout);

        return argc < 4 ? 1 : 0;
    }

    char session_dir[PATH_MAX];

    if ( expand_tilde(argv[2], session_dir, sizeof(session_dir)) != 0 
       || session_dir[0] != '/') 
    {
        fprintf(stderr
                , "prismafs replay: session dir must be absolute path, got '%s'\n", argv[2]);

        return 1;
    }

    struct stat st;

    if (stat(session_dir, &st) != 0 
       || !S_ISDIR(st.st_mode) ) 
    {
        fprintf(stderr
               , "prismafs replay: '%s' is not a directory\n", session_dir);

        return 1;
    }

    char target_dir[PATH_MAX];

    if ( expand_tilde(argv[3], target_dir, sizeof(target_dir)) != 0 
       || target_dir[0] != '/') 
    {
        fprintf(stderr
               , "prismafs replay: target dir must be absolute path, got '%s'\n", argv[3]);

        return 1;
    }

    if (mkdir_p(target_dir, 0755) != 0) 
    {
        fprintf( stderr
               , "prismafs replay: cannot create '%s': %s\n", target_dir, strerror(errno));

        return 1;
    }

    printf("replaying '%s' into '%s':\n", session_dir, target_dir);

    struct rcount rc = { 0, 0, 0 };

    walk( session_dir
        , target_dir
        , ""
        , &rc);

    printf("\n%d applied, %d removed, %d error(s)\n"
          , rc.applied
          , rc.removed
          , rc.errors);

    return rc.errors > 0 ? 1 : 0;
}
