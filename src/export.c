/* ============================================================
   PrismaFS - export.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */

/*
 * This source file is for "prismafs export" and "prismafs import"
 *
 * Session is essentially a directory and small manifest file
 * (write_session_manifest() in layers.c). export packs them
 * into one TAR archive and so session can be copied to another
 * machine, attached to bug report, used as backup, etc. import
 * does opposite, unpacks tar back into session dir and its
 * manifest, using original layout.
 *
 */

#include "prismafs.h"
#include <sys/wait.h>

static void print_export_usage(FILE *out)
{
    fprintf(out, "Usage: prismafs export [-o <archive.tar>] <session-dir>\n");
}

static void print_import_usage(FILE *out)
{
    fprintf(out, "Usage: prismafs import <archive.tar> <dest-dir>\n");
}



// split "/a/b/c" as "/a/b" and base "c", so tar can be 
// started from parent dir and archive/extract only last path part
static int split_parent_base(const char *path, char parent[PATH_MAX], char base[PATH_MAX])
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        len--;

    if ((size_t)snprintf(tmp, sizeof(tmp), "%.*s", (int)len, path) >= sizeof(tmp))
        return -1;

    char *slash = strrchr(tmp, '/');

    if (!slash || slash == tmp) {

        // path is under root or has no / 
        snprintf(parent, PATH_MAX, "/");
        snprintf(base, PATH_MAX, "%s", slash ? slash + 1 : tmp);

        return 0;
    }

    *slash = '\0';
    snprintf(parent, PATH_MAX, "%s", tmp);
    snprintf(base, PATH_MAX, "%s", slash + 1);

    return 0;
}


// runs tar with provided argv (NULL-terminated, argv[0] is "tar") and awaits
// so export/import dont need own archive read/write code
static int run_tar(char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0) {

        fprintf(stderr, "prismafs: fork: %s\n", strerror(errno));

        return -1;
    }

    if (pid == 0) {
        execvp("tar", argv);
        fprintf(stderr, "prismafs: exec tar: %s\n", strerror(errno));

        _exit(127);
    }

    int status;

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "prismafs: waitpid: %s\n", strerror(errno));

        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)

        return 0;

    return -1;
}

// prints manifest to user after export/import
static void print_manifest(const char *manifest_path)
{
    FILE *f = fopen(manifest_path, "r");

    if (!f)
        return;

    char line[PATH_MAX + 32];

    printf("session info:\n");

    while (fgets(line, sizeof(line), f))
        printf("  %s", line);

    fclose(f);
}

// "prismafs export [-o <archive.tar>] <session-dir>"
//
// - pack session directory and manifest into tar archive.
// - original names in archive 
int export_command(int argc, char **argv)
{
    const char *output = NULL;
    int i = 2;

    // walking optional flags 
    while (i < argc) {

        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
            i++;

            continue;
        }

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_export_usage(stdout);

            return 0;
        }

        break;
    }

    if (i >= argc) {
        print_export_usage(stderr);

        return 1;
    }

    char session_dir[PATH_MAX];

    if (expand_tilde(argv[i], session_dir, sizeof(session_dir)) != 0 || session_dir[0] != '/') {
        fprintf(stderr, "prismafs export: session dir must be absolute path, got '%s'\n", argv[i]);

        return 1;
    }

    struct stat st;


    // reject archiving TAR for anything thats not session directory
    if (stat(session_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "prismafs export: '%s' is not a directory\n", session_dir);

        return 1;
    }

    char manifest_path[PATH_MAX];


    if (session_manifest_path(session_dir, manifest_path) != 0)

        return 1;

    int have_manifest = (access(manifest_path, F_OK) == 0);

    if (!have_manifest)
        fprintf(stderr, "prismafs export: no session manifest found for '%s', "
                         "exporting session data only\n", session_dir);

    char session_parent[PATH_MAX], session_base[PATH_MAX];
    char manifest_parent[PATH_MAX], manifest_base[PATH_MAX];

    if (split_parent_base(session_dir, session_parent, session_base) != 0)

        return 1;

    if (have_manifest && split_parent_base(manifest_path, manifest_parent, manifest_base) != 0)

        return 1;

    char output_path[PATH_MAX];

    if (output) {
        if (expand_tilde(output, output_path, sizeof(output_path)) != 0) {
            fprintf(stderr, "prismafs export: invalid output path '%s'\n", output);

            return 1;
        }
    } else {
        // no -o, default to <session-name>.tar in current dir
        if ((size_t)snprintf(output_path, sizeof(output_path), "%s.tar", session_base) >= sizeof(output_path))

            return 1;
    }

    char *tar_argv[16];
    int n = 0;

    // build TAR command as manifest args are optional
    tar_argv[n++] = "tar";
    tar_argv[n++] = "-cf";
    tar_argv[n++] = output_path;
    tar_argv[n++] = "-C";
    tar_argv[n++] = session_parent;
    tar_argv[n++] = session_base;

    if (have_manifest) {

        tar_argv[n++] = "-C";
        tar_argv[n++] = manifest_parent;
        tar_argv[n++] = manifest_base;

    }

    tar_argv[n] = NULL;

    if (run_tar(tar_argv) != 0) {
        fprintf(stderr, "prismafs export: tar failed\n");

        return 1;
    }

    printf("exported '%s' to '%s'\n", session_dir, output_path);


    if (have_manifest)
        print_manifest(manifest_path);

    return 0;
}

// "prismafs import <archive.tar> <dest-dir>"
//
// - extract session archive into dest-dir 
// - recreate session folder and manifest as packed by export
int import_command(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0) {
       
        print_import_usage(argc < 4 ? stderr : stdout);

        return argc < 4 ? 1 : 0;
    }

    char archive[PATH_MAX];

    if (expand_tilde(argv[2], archive, sizeof(archive)) != 0) {
        fprintf(stderr, "prismafs import: invalid archive path '%s'\n", argv[2]);

        return 1;
    }

    // exist early with error if TAR is failing
    if (access(archive, F_OK) != 0) {
        fprintf(stderr, "prismafs import: '%s': %s\n", archive, strerror(errno));

        return 1;
    }

    char dest_dir[PATH_MAX];

    if (expand_tilde(argv[3], dest_dir, sizeof(dest_dir)) != 0 || dest_dir[0] != '/') {
      
        fprintf(stderr, "prismafs import: dest dir must be an absolute path, got '%s'\n", argv[3]);

        return 1;
    }

    // dest dir must be there before extracting there
    if (mkdir_p(dest_dir, 0755) != 0) {
       
        fprintf(stderr, "prismafs import: cannot create '%s': %s\n", dest_dir, strerror(errno));
       
       
        return 1;
    }

    char *tar_argv[] = { "tar", "-xf", archive, "-C", dest_dir, NULL };

    if (run_tar(tar_argv) != 0) {
        fprintf(stderr, "prismafs import: tar failed\n");
       
        return 1;
    }

    printf("imported '%s' into '%s'\n", archive, dest_dir);

    // find *.prismafs-session file that was extracted and show it
    // one per archive
    DIR *dp = opendir(dest_dir);

    if (dp) {
        struct dirent *de;

        while ((de = readdir(dp)) != NULL) {

            size_t len = strlen(de->d_name);
            const char *suffix = ".prismafs-session";
            size_t suffix_len = strlen(suffix);

            if (len > suffix_len && strcmp(de->d_name + len - suffix_len, suffix) == 0) {
                char found[PATH_MAX];

                snprintf(found, sizeof(found), "%s/%s", dest_dir, de->d_name);
                print_manifest(found);

                break;
            }
        }

        closedir(dp);
    }

    return 0;
}
