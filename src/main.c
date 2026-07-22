#include "prismafs.h"

static const char *base_path_initial = "/"; // default base layer path fallback

// parse line format config file.
// directives (one per line, # for comments):
//   session <path>   - session layer directory (required once)
//   base <path>      - base layer directory (required once or more. order = priority)
static int load_config(const char *config_path)
{
    FILE *f = fopen(config_path, "r");
    if (!f) {
        fprintf(stderr, "prismafs: cannot open config file '%s': %s\n",
                config_path, strerror(errno));
        return -1;
    }

    char line[PATH_MAX + 16];
    int found_session = 0;

    while (fgets(line, sizeof(line), f)) {
        // strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        // skip leading whitespace
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        // skip empty lines and comments
        if (*p == '\0' || *p == '#') continue;

        char keyword[16];
        char value[4096];
        if (sscanf(p, "%15s %4095s", keyword, value) != 2) {
            fprintf(stderr, "prismafs: ignoring malformed config line: %s\n", p);
            continue;
        }

        if (strcmp(keyword, "session") == 0) {
            if (found_session) {
                fprintf(stderr, "prismafs: duplicate 'session' directive, ignoring: %s\n", value);
                continue;
            }
            strncpy(session_path, value, PATH_MAX - 1);
            session_path[PATH_MAX - 1] = '\0';
            found_session = 1;
        } else if (strcmp(keyword, "base") == 0) {
            if (num_base_layers >= MAX_BASE_LAYERS) {
                fprintf(stderr, "prismafs: max base layers (%d) reached, ignoring: %s\n",
                        MAX_BASE_LAYERS, value);
                continue;
            }
            strncpy(base_paths[num_base_layers], value, PATH_MAX - 1);
            base_paths[num_base_layers][PATH_MAX - 1] = '\0';
            num_base_layers++;
        } else {
            fprintf(stderr, "prismafs: unknown config directive '%s', ignoring\n", keyword);
        }
    }

    fclose(f);

    if (!found_session) {
        fprintf(stderr, "prismafs: config '%s' is missing a 'session' directive\n", config_path);
        return -1;
    }
    if (num_base_layers == 0) {
        fprintf(stderr, "prismafs: config '%s' has no 'base' directives\n", config_path);
        return -1;
    }

    return 0;
}

// FUSE operations table
static struct fuse_operations myfs_oper = {
    .getattr  = myfs_getattr,
    .readdir  = myfs_readdir,
    .open     = myfs_open,
    .access   = myfs_access,
    .read     = myfs_read,
    .write    = myfs_write,
    .truncate = myfs_truncate,
    .create   = myfs_create,
    .statfs   = myfs_statfs,
    .utimens  = myfs_utimens,
    .unlink   = myfs_unlink,
    .chmod    = myfs_chmod,
    .mkdir    = myfs_mkdir,
    .rmdir    = myfs_rmdir,
    .rename   = myfs_rename
    // extend operations here
};

int main(int argc, char *argv[])
{
    // POSIX version flag
    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("PrismaFS Version: %s\n", PRISMAFS_VERSION);
        return 0;
    }

    // scan argv for -c <configfile> and build a clean argv for fuse_main
    // (FUSE doesn't know about -c and would error on it)
    const char *config_path = NULL;
    char **fuse_argv = malloc(argc * sizeof(char *));
    if (!fuse_argv) {
        fprintf(stderr, "prismafs: out of memory\n");
        return 1;
    }
    int fuse_argc = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            fuse_argv[fuse_argc++] = argv[i];
        }
    }

    if (config_path) {
        // config file takes full precedence
        if (load_config(config_path) != 0) {
            free(fuse_argv);
            return 1;
        }
    } else {
        // fall back to environment variables for backward compatibility
        char *session_dir = getenv("SESSION_LAYER_DIR");
        if (session_dir == NULL) {
            fprintf(stderr,
                "prismafs: no config given and SESSION_LAYER_DIR is not set.\n"
                "Usage: prismafs -c <config> <mountpoint>\n"
                "       prismafs <mountpoint>   (with SESSION_LAYER_DIR / BASE_LAYER_DIRS set)\n");
            free(fuse_argv);
            return 1;
        }
        strncpy(session_path, session_dir, PATH_MAX - 1);
        session_path[PATH_MAX - 1] = '\0';

        char *base_dirs = getenv("BASE_LAYER_DIRS");
        if (base_dirs) {
            char *token = strtok(base_dirs, ",");
            while (token && num_base_layers < MAX_BASE_LAYERS) {
                strncpy(base_paths[num_base_layers], token, PATH_MAX - 1);
                base_paths[num_base_layers][PATH_MAX - 1] = '\0';
                num_base_layers++;
                token = strtok(NULL, ",");
            }
        } else {
            strncpy(base_paths[0], base_path_initial, PATH_MAX - 1);
            base_paths[0][PATH_MAX - 1] = '\0';
            num_base_layers = 1;
        }
    }

    int ret = fuse_main(fuse_argc, fuse_argv, &myfs_oper, NULL);
    free(fuse_argv);
    return ret;
}
