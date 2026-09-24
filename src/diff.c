/* ============================================================
   PrismaFS - diff.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */

/*
 * "prismafs diff <session-dir>"
 *
 * State snapshot tool. The idea is not history tracking over time. PrismaFS 
 * uses CoW, so session accumulates every change since session was 
 * created (writes, deletes, ownerships). Without it, there would be no
 * way to see what has changed, so thats what diff command provides
 * insight into.
 *
 * Some use cases: depending on what was touched, changed, etc, provides 
 * info for deciding to export or keep session, or to discard it. Its not
 * keeping logs, but a checkup command.
 *
 * Provides comparison of session vs base layer(s).
 *
 * Base layer paths are in session manifest (where write_session_manifest() 
 * writes to - layers.c), so user only
 * points to session directory, nothing else on command line.
 *
 * Session directory is traversed (not base layers). Base
 * Because of CoW, any writes, deletes or metadata modifs are reflected in
 * session anyway.
 */

#include "prismafs.h"

static void print_diff_usage(FILE *out)
{
    fprintf(out, "Usage: prismafs diff <session-dir>\n");
}

/* rdbases opens manifest file for reading base paths & stores them.
   Used for comparing session and base */
static int rdbases(const char *manifest_path)
{
    FILE *f = fopen ( manifest_path, "r" );

    // reurn -1 if cant open
    if (!f)
        return -1;

    char line[PATH_MAX + 16];
    num_base_layers = 0; // reset num_base_layers to 0 so list can be rebuilt

    // read line by line 
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        // strip \n or \r for string comparing
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        //check if line starts with prefix "base"
        // if matched, its a path
        // skip paths that don't start with prefix
        if (strncmp(line, "base ", 5) == 0 && num_base_layers < MAX_BASE_LAYERS) {
            strncpy(base_paths[num_base_layers], line + 5, PATH_MAX - 1);
            base_paths[num_base_layers][PATH_MAX - 1] = '\0';

            num_base_layers++;
        }
    }

    fclose(f);

    return num_base_layers > 0 ? 0 : -1;
}

// helper used by walk() to categorize entries in session dir
//   - entries are new when not found in any base = "added"
//   - if found in base, compare with fdiff/metadiff if its modified = "modified"
//   - for .deleted markers, confirm its marking deleted what existed before becoming removed = "removed"
static int basefind(const char *relpath, char fpath[PATH_MAX])
{
    for (int i = 0; i < num_base_layers; i++) {

        snprintf(fpath, PATH_MAX, "%s/%s", base_paths[i], relpath);

        if (access(fpath, F_OK) == 0)
           
          return 0;

    }

    return -1;
}

// byte compare 2 regular files
// 1 = content differs, 0 = identical, -1 = one couldnt be opened
static int fdiff(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");

    if (!fa || !fb) {

        if (fa) fclose(fa);
        if (fb) fclose(fb);

        return -1;
    }

    char bufa[8192], bufb[8192];
    int result = 0;

    for (;;) {

        size_t na = fread(bufa, 1, sizeof(bufa), fa);
        size_t nb = fread(bufb, 1, sizeof(bufb), fb);

        if (na != nb || memcmp(bufa, bufb, na) != 0) {

            result = 1;
            break;
        }

        if (na == 0)
            break; // both are out of bytes at same time = match
    }

    fclose(fa);
    fclose(fb);

    return result;
}

// struct to keep count for changes during walk()
//  - example data from diff_command after traversing tree:
//    "3 added, 1 removed, 1 modified, 2 permission/ownership change(s)"
struct dcount {
    int added;
    int removed;
    int modified;
    int meta;
};

// print permission and ownership diffs for some path, count them
static void metadiff(const char *relpath, const struct stat *sess_st,
                      const struct stat *base_st, struct dcount *dc)
{
    int changed = 0;


    // POSIX st_mode for file type = S_IFREG/S_IFDIR/etc, upper bits +
    // permission bits (rwxrwxrwx + setuid/setgid/sticky, lower 12 bits)
    // = one value of 07777 for lower 12 bits (same on every UNIX)
    //
    // bitwise AND. 07777 bitmask = 111 111 111 111 octal
    // must strip file type bits and check for only low 12 permission bits
    // for comparison
    if ((sess_st -> st_mode & 07777) != (base_st -> st_mode & 07777)) {
       
        printf("  permissions changed: %s (%04o -> %04o)\n",
               relpath, 
               base_st -> st_mode & 07777
             , sess_st -> st_mode & 07777);

        changed = 1;

    }

    if (sess_st -> st_uid != base_st -> st_uid 
         || sess_st -> st_gid != base_st -> st_gid) 
    {
       
        printf("  ownership changed:   %s (%d:%d -> %d:%d)\n",
               relpath, 
               base_st -> st_uid
             , base_st -> st_gid
             , sess_st -> st_uid
             , sess_st -> st_gid);

        changed = 1;
    }

    if (changed)

        dc -> meta++;
}

// walks session tree at one dir level and recurses into subdirs,
// than categorizes entries in relation to BASE layer(s)
static void walk(const char *session_root, const char *rel, struct dcount *dc)
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

    while ((de = readdir(dp)) != NULL) {

        if (strcmp(de -> d_name, ".") == 0 
            || strcmp(de -> d_name, "..") == 0)

            continue;

        char entry_rel[PATH_MAX];

        if (rel[0])
            snprintf(entry_rel, PATH_MAX, "%s/%s", rel, de -> d_name);

        else
            snprintf(entry_rel, PATH_MAX, "%s", de -> d_name);

        char entry_path[PATH_MAX];
        snprintf(entry_path, PATH_MAX, "%s/%s", dirpath, de -> d_name);

        size_t name_len = strlen(de -> d_name);
        const char *suffix = ".deleted";
        size_t suffix_len = strlen(suffix);

        // .deleted marker = whats deleted is removed from view
        // reporting only if item existed in base layer 
        if (name_len > suffix_len 
             && strcmp( de -> d_name + name_len - suffix_len, suffix) == 0) 
        {
          
            char target_rel[PATH_MAX];
            size_t target_len = strlen(entry_rel) - suffix_len;

            snprintf(target_rel, PATH_MAX, "%.*s", (int)target_len, entry_rel);

            char base_fpath[PATH_MAX];

            if (basefind(target_rel, base_fpath) == 0) {

                printf("  removed: %s\n", target_rel);

                dc -> removed++;
            }

            continue;
        }

        struct stat sess_st;

        // TOCTOU race check
        if ( lstat(entry_path, &sess_st) != 0)
            continue; // skip 

        char base_fpath[PATH_MAX];
        int in_base = (basefind(entry_rel, base_fpath) == 0);

        if (!in_base) {
            // not in any base layer, so its NEW
            if (S_ISDIR(sess_st.st_mode)) 
            {
                printf("  added:   %s/\n", entry_rel);
    
                dc -> added++;
    
                walk(session_root, entry_rel, dc); // list contents
    
            } else {
                printf("  added:   %s\n", entry_rel);

                dc -> added++;
            }

            continue;
        }

        struct stat base_st;

        if (lstat(base_fpath, &base_st) != 0)
            continue;

        if (S_ISDIR(sess_st.st_mode)) {

            if (!S_ISDIR(base_st.st_mode)) 
            {
            
                printf("  modified (was file, now directory): %s\n", entry_rel);
                dc -> modified++;
            
            } else {
            
                metadiff(entry_rel, &sess_st, &base_st, dc);
            
            }

            walk(session_root, entry_rel, dc);
            continue;

        }

        if (S_ISLNK(sess_st.st_mode)) 
        {
            char sess_target[PATH_MAX] = {0};
            char base_target[PATH_MAX] = {0};

            ssize_t sn = readlink(entry_path, sess_target, sizeof(sess_target) - 1);
            ssize_t bn = readlink(base_fpath, base_target, sizeof(base_target) - 1);

            if (sn < 0 || bn < 0 
                || strcmp(sess_target, base_target) != 0) 
            {
                printf("  modified (symlink target): %s\n", entry_rel);

                dc -> modified++;
            }

            continue;
        }

        // regular files, comparing content and metadata
        int content_diff = fdiff(entry_path, base_fpath);

        if (content_diff != 0)
            printf("  modified: %s\n", entry_rel);

        if (content_diff == 1)
            dc -> modified++;

        else if (content_diff == -1) // cant open either file, no comparison
            dc -> modified++; // but report as "modified"

        metadiff(entry_rel, &sess_st, &base_st, dc);
    }

    closedir(dp);
}

int diff_command(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[2], "-h") == 0 
                 || strcmp(argv[2], "--help") == 0) 
    {

        print_diff_usage(argc < 3 ? stderr : stdout);

        return argc < 3 ? 1 : 0;
    }

    char session_dir[PATH_MAX];

    if (expand_tilde(argv[2], session_dir, sizeof(session_dir)) != 0 
        || session_dir[0] != '/') 
    {
    
        fprintf(stderr, "prismafs diff: session dir must be absolute path, got '%s'\n", argv[2]);
    
        return 1;
    }

    struct stat st;

    if (stat(session_dir, &st) != 0 || !S_ISDIR(st.st_mode)) 
    {
    
        fprintf(stderr, "prismafs diff: '%s' is not a directory\n", session_dir);
    
        return 1;
    }

    char manifest_path[PATH_MAX];

    if (session_manifest_path(session_dir, manifest_path) != 0)

        return 1;

    if (rdbases(manifest_path) != 0) 
    {
    
        fprintf(stderr, "prismafs diff: no manifest with base layers found next to '%s'\n", session_dir);
       
        return 1;
    }

    printf("diff for session '%s':\n", session_dir);

    struct dcount dc = { 0, 0, 0, 0 };
    walk(session_dir, "", &dc);

    if (dc.added == 0 && dc.removed == 0 
                      && dc.modified == 0 && dc.meta == 0)
                      
        printf("  (no differences from base)\n");

    printf("\n%d added, %d removed, %d modified, %d permission/ownership change(s)\n",
           dc.added, dc.removed, dc.modified, dc.meta);

    return 0;
}
