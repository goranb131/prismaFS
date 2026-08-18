#define _GNU_SOURCE
#include "prismafs.h"

#ifndef __linux__

int run_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "prismafs run: only available on Linux (mount namespaces)\n");
    return 1;
}

#else

#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <linux/magic.h>
#include <time.h>

static void print_run_usage(FILE *out)
{
    fprintf(out, "Usage: prismafs run [-c <config>] <path> [--] <command> [args...]\n");
}

static int write_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;

    size_t len = strlen(data);
    ssize_t n = write(fd, data, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int enter_private_mount_ns(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char buf[64];

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0) {
        if (write_file("/proc/self/setgroups", "deny") != 0)
            return -1;
        snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)gid);
        if (write_file("/proc/self/gid_map", buf) != 0)
            return -1;
        snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)uid);
        if (write_file("/proc/self/uid_map", buf) != 0)
            return -1;
        return 0;
    }

    if (unshare(CLONE_NEWNS) == 0)
        return 0;

    return -1;
}

static int wait_fuse_mount(const char *path, pid_t fuse_pid)
{
    struct timespec ts = { 0, 100000000L };

    for (int i = 0; i < 50; i++) {
        int status;
        if (waitpid(fuse_pid, &status, WNOHANG) == fuse_pid)
            return -1;

        struct statfs sf;
        if (statfs(path, &sf) == 0 && sf.f_type == FUSE_SUPER_MAGIC)
            return 0;

        nanosleep(&ts, NULL);
    }

    return -1;
}

static void stop_fuse(pid_t fuse_pid)
{
    if (fuse_pid > 0) {
        kill(fuse_pid, SIGTERM);
        waitpid(fuse_pid, NULL, 0);
    }
}

int run_command(int argc, char **argv)
{
    const char *config_path = NULL;
    int i = 2;

    while (i < argc) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_run_usage(stdout);
            return 0;
        }
        break;
    }

    if (i >= argc) {
        print_run_usage(stderr);
        return 1;
    }

    char cover[PATH_MAX];
    if (expand_tilde(argv[i], cover, sizeof(cover)) != 0 || cover[0] != '/') {
        fprintf(stderr, "prismafs run: path must be absolute, got '%s'\n", argv[i]);
        return 1;
    }
    i++;

    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;

    if (i >= argc) {
        fprintf(stderr, "prismafs run: missing command\n");
        print_run_usage(stderr);
        return 1;
    }

    char **cmd = argv + i;

    if (strcmp(cover, "/") == 0) {
        fprintf(stderr, "prismafs run: refusing to cover /\n");
        return 1;
    }

    struct stat st;
    if (stat(cover, &st) != 0) {
        fprintf(stderr, "prismafs run: '%s': %s\n", cover, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "prismafs run: '%s' is not a directory\n", cover);
        return 1;
    }

    if (configure_layers(config_path) != 0)
        return 1;

    if (mkdir_p(session_path, 0755) != 0) {
        fprintf(stderr, "prismafs run: cannot create session directory '%s': %s\n",
                session_path, strerror(errno));
        return 1;
    }

    char tmpl[PATH_MAX];
    const char *td = getenv("TMPDIR");
    if (!td || td[0] != '/')
        td = "/tmp";
    if (snprintf(tmpl, sizeof(tmpl), "%s/prismafs-run-XXXXXX", td) >= (int)sizeof(tmpl))
        return 1;
    if (!mkdtemp(tmpl)) {
        fprintf(stderr, "prismafs run: mkdtemp: %s\n", strerror(errno));
        return 1;
    }

    pid_t isol_pid = fork();
    if (isol_pid < 0) {
        fprintf(stderr, "prismafs run: fork: %s\n", strerror(errno));
        rmdir(tmpl);
        return 1;
    }
    if (isol_pid == 0) {
        if (enter_private_mount_ns() != 0) {
            fprintf(stderr, "prismafs run: unshare: %s\n", strerror(errno));
            _exit(1);
        }
        if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            fprintf(stderr, "prismafs run: make-rprivate: %s\n", strerror(errno));
            _exit(1);
        }

        pid_t fuse_pid = fork();
        if (fuse_pid < 0) {
            fprintf(stderr, "prismafs run: fork: %s\n", strerror(errno));
            _exit(1);
        }
        if (fuse_pid == 0) {
            int n = open("/dev/null", O_RDWR);
            if (n >= 0) {
                dup2(n, 0);
                dup2(n, 1);
                dup2(n, 2);
                if (n > 2)
                    close(n);
            }
            setsid();
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            _exit(prismafs_fuse_foreground(tmpl) == 0 ? 0 : 1);
        }

        if (wait_fuse_mount(tmpl, fuse_pid) != 0) {
            fprintf(stderr, "prismafs run: FUSE mount did not become ready at %s\n", tmpl);
            stop_fuse(fuse_pid);
            _exit(1);
        }

        if (mount(tmpl, cover, NULL, MS_BIND, NULL) != 0) {
            fprintf(stderr, "prismafs run: bind %s onto %s: %s\n",
                    tmpl, cover, strerror(errno));
            stop_fuse(fuse_pid);
            _exit(1);
        }

        pid_t cmd_pid = fork();
        if (cmd_pid < 0) {
            fprintf(stderr, "prismafs run: fork: %s\n", strerror(errno));
            stop_fuse(fuse_pid);
            _exit(1);
        }
        if (cmd_pid == 0) {
            execvp(cmd[0], cmd);
            fprintf(stderr, "prismafs run: exec %s: %s\n", cmd[0], strerror(errno));
            _exit(127);
        }

        int status;
        if (waitpid(cmd_pid, &status, 0) < 0)
            status = 1 << 8;

        stop_fuse(fuse_pid);

        if (WIFEXITED(status))
            _exit(WEXITSTATUS(status));
        if (WIFSIGNALED(status))
            _exit(128 + WTERMSIG(status));
        _exit(1);
    }

    int status;
    if (waitpid(isol_pid, &status, 0) < 0) {
        fprintf(stderr, "prismafs run: waitpid: %s\n", strerror(errno));
        rmdir(tmpl);
        return 1;
    }
    rmdir(tmpl);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

#endif
