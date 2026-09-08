/*
 * This file is for Linux exclusive command "prismafs ns"
 *
 * example:
 *  SESSION_LAYER_DIR=/tmp/session BASE_LAYER_DIRS=/tmp/base \
 *   prismafs ns /home/alice -- bash
 *
 * open bash shell and /home/alice looks like normal alice home but every write
 * you do goes to /tmp/session, and every read thats not in
 * session goes through /tmp/base. IRL example:install package, edit config,
 * delete file, and it doesn't affect real /home/alice. opening
 * second terminal without doing "prismafs ns" still sees
 * real /home/alice, so changes made inside ns session are
 * invisible outside of it. exit that bash shell and /home/alice is 
 * what it was before.
 *
 *   - fork off child, that gets own user and mount namespace
 *   - if session or base layer is under <path>, bind
 *      it somewhere else first (otherwise it would be hidden)
 *   - mount prismafs itself in private temp dir
 *   - bind mount that temp dir on top of <path>
 *   - exec command, wait for it, and unmount and cleanup behind
 */

#define _GNU_SOURCE
#include "prismafs.h"

// prismafs ns needs mount namespaces 
// its Linux only, so for non-linux builds its only stubs
#ifndef __linux__

int ns_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "prismafs ns: only available on Linux (mount namespaces)\n");
    return 1;
}

#else

#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <linux/magic.h>
#include <time.h>

// debug/trace line for ns
// its prefixed so its clear in logs where it comes from
static void run_dbg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "prismafs ns: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void print_run_usage(FILE *out)
{
    fprintf(out, "Usage: prismafs ns [-c <config>] [-n <name>] [--desc <description>] "
                 "<path> [--] <command> [args...]\n");
}

// *helper*
// only writes string into /proc file and closes it. 
// Used for uid/gid below
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

// get user and mount namespace so mounts here dont leak 
// need uid/gid map before new namespace can be used = setgroups/gid_map/uid_map 
// fall back to mount namespace only if unprivileged user namespaces not allowed (some distros disable them)
static int enter_private_mount_ns(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char buf[64];

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0) {

        if (write_file("/proc/self/setgroups", "deny") != 0) {
            run_dbg("setgroups deny failed: %s", strerror(errno));

            return -1;
        }

        snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)gid);

        if (write_file("/proc/self/gid_map", buf) != 0) {
            run_dbg("gid_map failed: %s", strerror(errno));

            return -1;
        }

        snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)uid);

        if (write_file("/proc/self/uid_map", buf) != 0) {
            run_dbg("uid_map failed: %s", strerror(errno));

            return -1;
        }

        run_dbg("unshare user+mount ok (host uid %u, now %u)",
                (unsigned)uid, (unsigned)getuid());

        return 0;
    }
    run_dbg("unshare user+mount failed: %s", strerror(errno));

    if (unshare(CLONE_NEWNS) == 0) {
        
        run_dbg("unshare mount-only ok");

        return 0;
    }

    run_dbg("unshare mount-only failed: %s", strerror(errno));

    return -1;
}

// fuse_main forks and mounts in background, so poll statfs until FUSE_SUPER_MAGIC shows.
// waits 5 seconds, and exits early if FUSE helper exits before mounting
static int wait_fuse_mount(const char *path, pid_t fuse_pid)
{
    struct timespec ts = { 0, 100000000L };

    for (int i = 0; i < 50; i++) {
        int status;

        if (waitpid(fuse_pid, &status, WNOHANG) == fuse_pid) {
            run_dbg("fuse helper exited before mount was ready");

            return -1;
        }

        struct statfs sf;

        if (statfs(path, &sf) == 0 && sf.f_type == FUSE_SUPER_MAGIC) {
            run_dbg("fuse mounted at %s", path);

            return 0;
        }

        nanosleep(&ts, NULL);
    }

    run_dbg("timed out waiting for fuse at %s", path);

    return -1;
}

// shutdown of FUSE helper process after command is done
static void stop_fuse(pid_t fuse_pid)
{
    if (fuse_pid > 0) {
        kill(fuse_pid, SIGTERM);
        waitpid(fuse_pid, NULL, 0);
    }
}

// true if path is root or anywhere inside root
static int path_equals_or_under(const char *path, const char *root)
{
    size_t n = strlen(root);

    if (strcmp(path, root) == 0)

        return 1;
    if (n > 0 && strncmp(path, root, n) == 0 && path[n] == '/')

        return 1;

    return 0;
}

// if session/base layer is path that will be covered by FUSE mount, bind it
// somewhere first, or else prismafs would read its own mount, not
// real layer. path is replaced to point to safe copy
static int preserve_layer(char path[PATH_MAX], const char *cover,
                          const char *keep_root, int idx)
{
    char dest[PATH_MAX];

    if (!path_equals_or_under(path, cover)) {
        run_dbg("layer %s stays as-is", path);
        return 0;
    }

    if (snprintf(dest, sizeof(dest), "%s/%d", keep_root, idx) >= (int)sizeof(dest))
    
        return -1;

    if (mkdir(dest, 0700) != 0 && errno != EEXIST)
  
        return -1;

    if (mount(path, dest, NULL, MS_BIND | MS_REC, NULL) != 0) {
        run_dbg("preserve bind %s -> %s failed: %s", path, dest, strerror(errno));
  
        return -1;
    }

    run_dbg("preserved %s -> %s", path, dest);

    if (snprintf(path, PATH_MAX, "%s", dest) >= PATH_MAX)

        return -1;

    return 0;
}

// calls preserve_layer 
// which takes idx arg to build subdir under keep_root like keep/0, keep/1, etc.
// session layer always gets index 0, and base layers i+1
static int keeplayers(const char *cover, const char *keep_root)
{
    if (preserve_layer(session_path, cover, keep_root, 0) != 0)
        return -1;

    for (int i = 0; i < num_base_layers; i++) {
        if (preserve_layer(base_paths[i], cover, keep_root, i + 1) != 0)

            return -1;
    }

    return 0;
}

// removes keep_root/0, keep_root/1, etc, that keeplayers created, and keep_root itself.
// doesnt check if rmdir worked, no real point, because mount namespace and everything in it
// is about to disappear when this process exits
static void rmdir_keep(const char *keep_root)
{
    char dest[PATH_MAX];

    for (int i = 0; i <= MAX_BASE_LAYERS; i++) {
        if (snprintf(dest, sizeof(dest), "%s/%d", keep_root, i) < (int)sizeof(dest))
            rmdir(dest);
    }

    rmdir(keep_root);
}

// entry point for "prismafs ns <path> -- <command> [args...]"
// idea is, mount prismafs somewhere private, bind it on top of <path> inside private
// mount namespace, then exec command. only that process (and its children) sees the
// layered view, everything else outside still sees <path> as is
int ns_command(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *session_name = NULL;
    const char *session_desc = NULL;
    int i = 2;

    while (i < argc) {

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
            i++;
            continue;
        }

        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            session_name = argv[++i];
            i++;
            continue;
        }

        if (strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
            session_desc = argv[++i];
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
        fprintf(stderr, "prismafs ns: path must be absolute, got '%s'\n", argv[i]);
       
        return 1;
    }
    i++;

    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;

    if (i >= argc) {
        fprintf(stderr, "prismafs ns: missing command\n");
        print_run_usage(stderr);

        return 1;
    }

    char **cmd = argv + i;

    /* 
      cover is any <path> or real directory passed from user on CLI 
      (example: /home/alice). prismafs is bind mounted over it, 
      covering over real content with layered view as long as "ns" runs
    */


    // covering / would hide filesystem from child process, so refuse
    if (strcmp(cover, "/") == 0) {
        fprintf(stderr, "prismafs ns: refusing to cover /\n");
        return 1;
    }

    struct stat st;

    if (stat(cover, &st) != 0) {
        fprintf(stderr, "prismafs ns: '%s': %s\n", cover, strerror(errno));
       
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "prismafs ns: '%s' is not a directory\n", cover);
       
        return 1;
    }

    // same config resolution as any normal mount: 1) -c flag, 2) env vars, 3) default.conf
    if (configure_layers(config_path) != 0)
       
        return 1;

    if (mkdir_p(session_path, 0755) != 0) {
        fprintf(stderr, "prismafs ns: cannot create session directory '%s': %s\n",
                session_path, strerror(errno));
        return 1;
    }

    // give this session a name/description the first time its used, later
    // runs against the same session dir just leave the manifest as it is
    write_session_manifest(session_name, session_desc);

    // tmpl is where FUSE mount is temporarily before its binded
    char tmpl[PATH_MAX];
    const char *td = getenv("TMPDIR");

    if (!td || td[0] != '/')
        td = "/tmp";

    if (snprintf(tmpl, sizeof(tmpl), "%s/prismafs-ns-XXXXXX", td) >= (int)sizeof(tmpl))
       
        return 1;

    if (!mkdtemp(tmpl)) {
        fprintf(stderr, "prismafs ns: mkdtemp: %s\n", strerror(errno));

        return 1;
    }

    // string path for empty temporary folder name ie. /tmp/prismafs-keep-Ab3xYz
    char keep[PATH_MAX];

    if (snprintf(keep, sizeof(keep), "%s/prismafs-keep-XXXXXX", td) >= (int)sizeof(keep)) {
        rmdir(tmpl);

        return 1;
    }

    if (!mkdtemp(keep)) {
        fprintf(stderr, "prismafs ns: mkdtemp: %s\n", strerror(errno));
        rmdir(tmpl);

        return 1;
    }

    run_dbg("cover=%s session=%s", cover, session_path);

    for (int b = 0; b < num_base_layers; b++)
        run_dbg("base[%d]=%s", b, base_paths[b]);

    run_dbg("fuse dir %s keep dir %s", tmpl, keep);
    run_dbg("command %s", cmd[0]);

    // 1st fork. from here, everything is now in forked child and parent waits for it
    // cleans tmpl/keep when done. isol_pid is the child
    pid_t isol_pid = fork();

    if (isol_pid < 0) {
        fprintf(stderr, "prismafs ns: fork: %s\n", strerror(errno));
        rmdir(tmpl);
        rmdir(keep);

        return 1;
    }

    if (isol_pid == 0) {
        run_dbg("in isolated child, pid %d", (int)getpid());

        if (enter_private_mount_ns() != 0) {
            fprintf(stderr, "prismafs ns: unshare: %s\n", strerror(errno));
            _exit(1);
        }

        // by default, mount namespace shares mount events with namespace originated 
        // from, so to mount or unmount here, it can show up out there
        // too. following 2 calls turn such sharing off for this namespace before mounting
        // anything. FIRST switch to slave (stop sending changes out), THEN to private
        // (stop receiving them). none of later mounts can leak because above is done 
        // before keeplayers/fuse/the cover bind mount
        mount("none", "/", NULL, MS_REC | MS_SLAVE, NULL);

        if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            fprintf(stderr, "prismafs ns: make-rprivate: %s\n", strerror(errno));

            _exit(1);
        }
        run_dbg("mount namespace is private");

        if (keeplayers(cover, keep) != 0) {
            fprintf(stderr, "prismafs ns: preserve %s: %s\n", cover, strerror(errno));
     
            _exit(1);
        }

        // 2nd fork, this child runs FUSE mount at tmpl in the background.
        // its stdio is closed because nothing reads it, and PDEATHSIG will
        // kill it with private mount namespace so it cant continue on after such that it couldn't be unmounted
        pid_t fuse_pid = fork();

        if (fuse_pid < 0) {
            fprintf(stderr, "prismafs ns: fork: %s\n", strerror(errno));
       
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

        run_dbg("fuse helper pid %d, waiting for mount", (int)fuse_pid);

        if (wait_fuse_mount(tmpl, fuse_pid) != 0) {
            fprintf(stderr, "prismafs ns: FUSE mount did not become ready at %s\n", tmpl);
            stop_fuse(fuse_pid);
       
            _exit(1);
        }

        // this is where swap is, cover now shows FUSE mount instead of what was 
        // there, but only for this private namespace, any outside processes see normal folder
        run_dbg("binding %s onto %s", tmpl, cover);

        if (mount(tmpl, cover, NULL, MS_BIND, NULL) != 0) {
            fprintf(stderr, "prismafs ns: bind %s onto %s: %s\n",
                    tmpl, cover, strerror(errno));
            stop_fuse(fuse_pid);
       
            _exit(1);
        }

        run_dbg("cover is now the layered view");

        // 3d fork, this one execs user command inside layered namespace
        pid_t cmd_pid = fork();

        if (cmd_pid < 0) {
            fprintf(stderr, "prismafs ns: fork: %s\n", strerror(errno));
            stop_fuse(fuse_pid);
        
            _exit(1);
        }

        if (cmd_pid == 0) {
            run_dbg("exec %s", cmd[0]);
            execvp(cmd[0], cmd);
            fprintf(stderr, "prismafs ns: exec %s: %s\n", cmd[0], strerror(errno));
        
            _exit(127);
        }
        
        // wait here till command exits, unmount FUSE and exit with code/signal it had
        run_dbg("command pid %d, waiting", (int)cmd_pid);

        int status;

        if (waitpid(cmd_pid, &status, 0) < 0)
            status = 1 << 8;

        if (WIFEXITED(status))
            run_dbg("command exited %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            run_dbg("command killed by signal %d", WTERMSIG(status));

        run_dbg("stopping fuse");
        stop_fuse(fuse_pid);

        if (WIFEXITED(status))
            _exit(WEXITSTATUS(status));
        if (WIFSIGNALED(status))

            _exit(128 + WTERMSIG(status));

            _exit(1);
    }

    int status;
   
    if (waitpid(isol_pid, &status, 0) < 0) {
        fprintf(stderr, "prismafs ns: waitpid: %s\n", strerror(errno));
        rmdir(tmpl);
        rmdir_keep(keep);

        return 1;
    }

    rmdir(tmpl);
    rmdir_keep(keep);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}

#endif
