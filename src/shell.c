/* ============================================================
   PrismaFS - shell.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */

/*
 * Command for interactive shell in PrismaFS mount session:
 * "prismafs shell [-c <config>] [-n <name>] [--desc <description>] <mountpoint>"
 *
 * mounts prismafs to <mountpoint> (normal mount), opens interactive subshell. 
 * cd into with session name prompt
 * - $PRISMAFS_SESSION set 
 * - exit subshell and on unmount, diff is shown, asking to keep or discard
 *
 * FLOW:
 * - parse args -c <config>, -n <name>, --desc <desc>. mountpoint shouldnt be missing and must be absolute.
 * - configure_layers loads/builds config. mkdir_p to cover that session dir and mountpoint exist. 
 *   write_session_manifest writes name/created/description/base into .prismafs-session if fresh session
 * - choose a name to show in prompt
 * - drop to session
 * - when subshell exits, stopfuse called - forks child to run unmount or fusermount -u
 * - prompt to keep or discard changes. if kept, keeps session dir, rest is wiped with rmrf
 *
 *  */

#define _GNU_SOURCE
#include "prismafs.h"
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static void shusage(FILE *out)
{
    fprintf(out, "Usage: prismafs shell [-c <config>] [-n <name>] [--desc <description>] "
                 "<mountpoint>\n");
}

// debug helper to show repeatable prefixes/lines 
static void shdbg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "prismafs shell: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

// during past fixed hang bug, SIGTERM wasnt able to unmount and exit.
// now, unmount at kernel level, then helper's read()
// will returen and main loop exit 
static void stopfuse(const char *mnt, pid_t fuse_pid)
{
    // safeguard - if 0 then nothing to stop, bail out
    if (fuse_pid <= 0)
        return;

    // fork process with sole purpose to exec unmount command
    pid_t um_pid = fork();

    // fork() will return 0 in child
    if (um_pid == 0) {
#ifdef __linux__
        // on linux attempt FUSE3 unmount or fallback to FUSE2
        execlp("fusermount3", "fusermount3", "-u", mnt, (char *)NULL);
        execlp("fusermount", "fusermount", "-u", mnt, (char *)NULL);
#else
        // on macOS/FreeBSD use direct umount command
        execlp("umount", "umount", mnt, (char *)NULL);
#endif
      // following shouldn't happen:
        _exit(127); // if every execlp fails, end child with 127 code (command not found)
    }

    // following shouldn't happen:

    if (um_pid > 0)
        waitpid(um_pid, NULL, 0); // prevent zombie

    kill(fuse_pid, SIGTERM);
    waitpid(fuse_pid, NULL, 0);
}

// recursive delete of session dir for discard. missing path is not an error
static int rmrf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return 0;

    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    DIR *dp = opendir(path);

    if (dp) {
        struct dirent *de;

        while ((de = readdir(dp)) != NULL) 
        {
            if (  strcmp(de -> d_name, ".") == 0 
               || strcmp(de -> d_name, "..") == 0)
                continue;

            char child[PATH_MAX];

            snprintf(child, sizeof(child), "%s/%s", path, de -> d_name);

            rmrf(child);
        }

        closedir(dp);
    }

    return rmdir(path);
}

// purpose: wait by polling every 100ms when child process exists but mount is 
// not ready yet until mount is live (kernel sees mnt to be different)
//
// called once, after fuse helper is forked before subshell starts.
// mount is not synchronous. fuse helper forks into background. 
// poll mountpoint st_dev until different than before mounting 
static int mntwait(const char *mnt, dev_t before, pid_t fuse_pid)
{
    struct timespec ts = { 0, 100000000L };

    for (int i = 0; i < 50; i++) 
    {
        int status;

        if (waitpid(fuse_pid, &status, WNOHANG) == fuse_pid) {
            shdbg("fuse helper exited before mount was ready");

            return -1;
        }

        struct stat st;

        if (stat(mnt, &st) == 0 && st.st_dev != before) 
        {
            shdbg("fuse mounted at %s", mnt);

            return 0;
        }

        nanosleep(&ts, NULL);
    }

    shdbg("timed out waiting for fuse at %s", mnt);

    return -1;
}

// subshell name
// params: explicit - passed via -n <name> flag
//         out - 256 byte buffer for result
//
// priority order:
//  1. -n flag wins
//  2. manifest (from session first mount)
//  3. session dir name
static void shnm(const char *explicit, char out[256])
{
    // 1st priority with -n flag, that is, if explicit != NULL
    if (explicit && explicit[0]) 
    {
        snprintf(out, 256, "%s", explicit);

        return;
    }

    char manifest_path[PATH_MAX];

    // 2nd priority with manifest
    if (session_manifest_path(session_path, manifest_path) == 0) 
    {
        FILE *f = fopen(manifest_path, "r"); // open manifest for reading

        if (f) {
            char line[512];

            // look for lines with "name" prefix
            while (fgets(line, sizeof(line), f)) 
            {
                if (strncmp(line, "name ", 5) == 0) 
                {
                    line[strcspn(line, "\r\n")] = '\0';

                    // copy name val into out (skip 5 chars for "name ")
                    snprintf(out, 256, "%s", line + 5); 

                    // close file
                    fclose(f);

                    return;
                }
            }

            fclose(f);
        }
    }

    // 3d priority with session dir name
    const char *base = strrchr(session_path, '/');

    snprintf(out, 256, "%s", base ? base + 1 : session_path);
}

// build argv for interactive subshell. 
// - needed to launch user interface, important part being that (prismafs:name)
//   prompt tag survives regardless of shell used
static char **shargv(const char *dispname)
{
    // determine shell via &SHELL env var, if empty default to sh
    const char *sh = getenv("SHELL");

    if (!sh || !sh[0])
        sh = "/bin/sh";

    // find last / in path
    const char *base = strrchr(sh, '/');
    // everything after it
    base = base ? base + 1 : sh;

    // build prompt tag
    // formatted as (prismafs:<dispname>)
    char tag[256];
    snprintf(tag, sizeof(tag), "(prismafs:%s) ", dispname);

    // allocate result array for up to 5 pointers
    char **argv = calloc(5, sizeof(char *));

    if (!argv)
        return NULL;

    // if bash
    if (strcmp(base, "bash") == 0) 
    {
        // unique name temp file -- XXXXXX random chars -- and open it
        char tmpl[] = "/tmp/prismafs-rc-XXXXXX";
        int fd = mkstemp(tmpl);

        // if fail, free array and return NULL
        if (fd < 0) 
        {
            free(argv);

            return NULL;
        }

        FILE *f = fdopen(fd, "w");

        // write 2 shell script lines into temp file to:
        // - source users existing bashrc 
        // - set PS1 with tag
        if (f) 
        {
            fprintf(f, "[ -f ~/.bashrc ] && source ~/.bashrc\n");
            fprintf(f, "PS1=\"%s$PS1\"\n", tag);

            // flush and close
            fclose(f);
        }

        // building argv array for launching bash
        // example:
        // /bin/bash --rcfile /tmp/prismafs-rc-aB3xYz -i
        argv[0] = strdup(sh);
        argv[1] = strdup("--rcfile");
        argv[2] = strdup(tmpl);
        argv[3] = strdup("-i");

        return argv;
    }

    // if zsh
    // - doesnt use rcfile flag like bash, uses ZDOTDIR to locate config.
    // - create temp directory for ZDOTDIR, not file this time:
    if (strcmp(base, "zsh") == 0) {
        char tmpl[] = "/tmp/prismafs-zdot-XXXXXX";

        if (!mkdtemp(tmpl)) 
        {
            free(argv);

            return NULL;
        }

        // next create .zshrc file inside ZDOTDIR dir 
        // source it and override PROMPT with tag (similar to PS1 in bash)
        char rcpath[PATH_MAX];

        snprintf(rcpath, sizeof(rcpath), "%s/.zshrc", tmpl);

        FILE *f = fopen(rcpath, "w");

        if (f) 
        {
            fprintf(f, "[ -f ~/.zshrc ] && source ~/.zshrc\n");
            fprintf(f, "PROMPT=\"%s$PROMPT\"\n", tag);
            fclose(f);
        }

        // set ZDOTDIR to temp directory 
        // when zsh starts it will find this instead of real ~/.zshrc
        setenv("ZDOTDIR", tmpl, 1);

        argv[0] = strdup(sh);
        argv[1] = strdup("-i");

        return argv;
    }

    // anything else.
    // read PS1 in interactive mode and launch
    setenv("PS1", tag, 1);

    argv[0] = strdup(sh);
    argv[1] = strdup("-i");

    return argv;
}

// main flow for the command
int shell_command(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *session_name = NULL;
    const char *session_desc = NULL;

    // start at argv[2] because 0 is "prismafs" and 1 is "shell"
    int i = 2;

    // parse flags
    while ( i < argc ) 
    {

        if ( strcmp(argv[i], "-c") == 0 && i + 1 < argc ) 
        {
            config_path = argv[++i];
            i++;

            continue;
        }

        if ( strcmp(argv[i], "-n") == 0 && i + 1 < argc ) 
        {
            session_name = argv[++i];
            i++;

            continue;
        }

        if ( strcmp(argv[i], "--desc") == 0 && i + 1 < argc ) 
        {
            session_desc = argv[++i];
            i++;

            continue;
        }

        if (  strcmp(argv[i], "-h") == 0 
           || strcmp(argv[i], "--help") == 0) 
        {
            shusage(stdout);

            return 0;
        }

        break;
    }

    // if nothing was after flags, no mountpoint was provided, error
    if (i >= argc) 
    {
        shusage(stderr);

        return 1;
    }

    char mnt[PATH_MAX];

    // expand_tilde() resolves ~ and result must start with / (only absolute paths)
    if (expand_tilde(argv[i], mnt, sizeof(mnt)) != 0 || mnt[0] != '/') 
    {
        fprintf(stderr, "prismafs shell: mountpoint must be absolute, got '%s'\n", argv[i]);
        
        return 1;
    }

    // load base/session paths from config or env/defaults, create session dir and
    // mountpoint if not there, provide session name and desc, resolve dispname for prompt:

    if (configure_layers(config_path) != 0)
        return 1;

    if (mkdir_p(session_path, 0755) != 0) 
    {
        fprintf(stderr, "prismafs shell: cannot create session directory '%s': %s\n",
                session_path, strerror(errno));

        return 1;
    }

    if (mkdir_p(mnt, 0755) != 0) 
    {
        fprintf(stderr, "prismafs shell: cannot create mountpoint '%s': %s\n",
                mnt, strerror(errno));

        return 1;
    }

    // session gets name/description on first mount, used on later runs
    write_session_manifest(session_name, session_desc);

    char dispname[256];
    shnm(session_name, dispname);

    // dev id before mounting
    struct stat pre_st;

    if (stat(mnt, &pre_st) != 0) 
    {
        fprintf(stderr, "prismafs shell: '%s': %s\n", mnt, strerror(errno));

        return 1;
    }

    shdbg("session=%s mount=%s name=%s", session_path, mnt, dispname);

    // fuse helper mounts at mnt in the background. nothing reads stdio so close
    // it. 
    pid_t fuse_pid = fork();

    if (fuse_pid < 0) 
    {
        fprintf(stderr, "prismafs shell: fork: %s\n", strerror(errno));

        return 1;
    }

    if (fuse_pid == 0) 
    {
        // redirect stdio to /dev/null
        int n = open("/dev/null", O_RDWR);

        if (n >= 0) 
        {
            dup2(n, 0);
            dup2(n, 1);
            dup2(n, 2);

            if (n > 2)
                close(n);
        }

        setsid();

        _exit(prismafs_fuse_foreground(mnt) == 0 ? 0 : 1);
    }

    if (mntwait(mnt, pre_st.st_dev, fuse_pid) != 0) 
    {
        fprintf(stderr, "prismafs shell: FUSE mount did not become ready at %s\n", mnt);
        
        stopfuse(mnt, fuse_pid);

        return 1;
    }

    setenv("PRISMAFS_SESSION", dispname, 1);

    // this child cds into mount and execs interactive subshell
    pid_t sh_pid = fork();

    if (sh_pid < 0) 
    {
        fprintf(stderr, "prismafs shell: fork: %s\n", strerror(errno));
        stopfuse(mnt, fuse_pid);

        return 1;
    }

    if (sh_pid == 0) 
    {
        if (chdir(mnt) != 0)
            fprintf(stderr, "prismafs shell: chdir %s: %s\n", mnt, strerror(errno));

        char **sargv = shargv(dispname);

        if (!sargv) 
        {
            fprintf(stderr, "prismafs shell: cannot set up subshell\n");

            _exit(1);
        }

        execvp(sargv[0], sargv);

        fprintf(stderr, "prismafs shell: exec %s: %s\n", sargv[0], strerror(errno));

        _exit(127);
    }

    printf("prismafs shell: entering session '%s' at %s (exit to leave)\n", dispname, mnt);

    int status;

    if (waitpid(sh_pid, &status, 0) < 0)
        status = 1 << 8;

    shdbg("stopping fuse");
    stopfuse(mnt, fuse_pid);

    // show session state, then let user commit or discard 
    char *diff_argv[] = { (char *)"prismafs", (char *)"diff", session_path };

    diff_command(3, diff_argv);

    printf("\nkeep this session? [y/N] ");

    fflush(stdout);

    char resp[16] = {0};

    if (fgets(resp, sizeof(resp), stdin) && (resp[0] == 'y' || resp[0] == 'Y'))
        shdbg("session kept at %s", session_path);
    else {
        rmrf(session_path);
        shdbg("session discarded (%s removed)", session_path);
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 0;
}
