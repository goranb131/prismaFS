/* ============================================================
   PrismaFS - main.c
   
   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */
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

        char resolved[PATH_MAX];
        resolved[0] = '\0';

        if (strcmp(keyword, "session") == 0 || strcmp(keyword, "base") == 0) {
            if (expand_tilde(value, resolved, sizeof(resolved)) != 0
                || resolved[0] != '/') {
                
                    fprintf(stderr,
                        "prismafs: '%s' path in '%s' must be absolute, got '%s'\n",
                        keyword, config_path, value);
                
                fclose(f);
        
                return -1;
            }
        }

        if (strcmp(keyword, "session") == 0) {
            if (found_session) {
                fprintf(stderr, "prismafs: duplicate 'session' directive, ignoring: %s\n", value);
                continue;
            }
            strncpy(session_path, resolved, PATH_MAX - 1);
            session_path[PATH_MAX - 1] = '\0';
            found_session = 1;
        } else if (strcmp(keyword, "base") == 0) {
            if (num_base_layers >= MAX_BASE_LAYERS) {
                fprintf(stderr, "prismafs: max base layers (%d) reached, ignoring: %s\n",
                        MAX_BASE_LAYERS, value);
                continue;
            }
            strncpy(base_paths[num_base_layers], resolved, PATH_MAX - 1);
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

/* ----------------------------------
// INTERACTIVE PROMPTING FUNCTION 
// 
// ask user 2 questions and create ready default config:
// ~/.config/prismafs/default.conf
// user thus doesnt need to write config by hand or set env vars
// -----------------------------------
*/

static int run_init(void)
{
    // resolve where HOME is (everything is relative to home, so need to set it)
    char *home = getenv("HOME");
   
    if (!home) {
         fprintf(stderr, "prismafs init: HOME is not set\n");
        
      return 1;
    }

    char conf_dir[PATH_MAX];
    char conf_path[PATH_MAX];

    // building target paths for config:
    snprintf(conf_dir,  sizeof(conf_dir),  "%s/.config/prismafs", home);
    snprintf(conf_path, sizeof(conf_path), "%s/default.conf", conf_dir);

    // if config is already there, so confirm overwrite:
    if (access(conf_path, F_OK) == 0) {

        printf("Config already exists at %s\nOverwrite? [y/N]: ", conf_path);
        fflush(stdout);
        
        char ans[8];
        
        if ( !fgets(ans, sizeof(ans), stdin) 
             || (ans[0] != 'y' && ans[0] != 'Y')) {
           
            printf("Aborted.\n");
       
            return 0;
        }
    }

    char session[PATH_MAX];

    printf("Session layer directory for writes: ");
    fflush(stdout);
    
    // session layer path input turned into string
    if ( !fgets(session, sizeof(session), stdin) ) {
        fprintf(stderr, "prismafs init: failed to read input\n");
      
        return 1;
    }

    session[strcspn(session, "\n")] = '\0';
    
    if ( strlen(session) == 0) {
        fprintf(stderr, "prismafs init: session directory cannot be empty\n");
      
        return 1;
    }

    char session_abs[PATH_MAX];

    if (expand_tilde(session, session_abs, sizeof(session_abs)) != 0
        || session_abs[0] != '/') {
        fprintf(stderr,
                "prismafs init: session directory must be an absolute path, got '%s'\n",
                session);

        return 1;
    }

    char base[PATH_MAX];

    // input for BASE layer dir 
    printf("Base layer directory (READ-ONLY, example: /): ");
    fflush(stdout);
    
    if (!fgets(base, sizeof(base), stdin)) {
        fprintf(stderr, "prismafs init: fail to read input\n");
    
        return 1;
    }
    
    base[strcspn(base, "\n")] = '\0';
    
    if (strlen(base) == 0) {
        fprintf(stderr, "prismafs init: at least one base directory is required\n");
    
        return 1;
    }

    char base_abs[PATH_MAX];

    if (expand_tilde(base, base_abs, sizeof(base_abs)) != 0
        || base_abs[0] != '/') {

        fprintf(stderr,
                "prismafs init: base directory must be an absolute path, got '%s'\n",
                base);

        return 1;
    }

    struct stat base_st;

    if (stat(base_abs, &base_st) == -1) {
        fprintf(stderr, "prismafs init: base directory '%s' does not exist: %s\n",
                base_abs, strerror(errno));

        return 1;
    }

    if (!S_ISDIR(base_st.st_mode)) {
        fprintf(stderr, "prismafs init: base '%s' is not a directory\n", base_abs);

        return 1;
    }

    if (mkdir_p(session_abs, 0755) != 0) {
        fprintf(stderr, "prismafs init: cannot create session directory '%s': %s\n",
                session_abs, strerror(errno));

        return 1;
    }

    // create ~/.config and ~/.config/prismafs/ 
    char config_parent[PATH_MAX];
    snprintf(config_parent, sizeof(config_parent), "%s/.config", home);
   
    mkdir(config_parent, 0755);
   
    if (mkdir(conf_dir, 0755) == -1 && errno != EEXIST) {
        fprintf(stderr, "prismafs init: cannot create %s: %s\n", conf_dir, strerror(errno));
        return 1;
    }

    // write config file with user input:
    FILE *f = fopen(conf_path, "w");
   
    if (!f) {
        fprintf(stderr, "prismafs init: cannot write %s: %s\n", conf_path, strerror(errno));
        return 1;
    }

    fprintf(f, "# PrismaFS config generated by prismafs init\n");
    fprintf(f, "# Session layer: all writes go here (needs to be writable)\n");
    fprintf(f, "session %s\n\n", session_abs);
    fprintf(f, "# Base layer is read-only source (more lines for multiple base layers)\n");
    fprintf(f, "base %s\n", base_abs);
    fclose(f);

    printf("\nConfig written to %s\n", conf_path);
    printf("Mount with: prismafs <mountpoint>\n");
    printf("        or: prismafs -c %s <mountpoint>\n", conf_path);
   
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
    .chown    = myfs_chown,
    .mkdir    = myfs_mkdir,
    .rmdir    = myfs_rmdir,
    .rename   = myfs_rename,
    .symlink  = myfs_symlink,
    .readlink = myfs_readlink,
    .getxattr    = myfs_getxattr,
    .setxattr    = myfs_setxattr,
    .listxattr   = myfs_listxattr,
    .removexattr = myfs_removexattr
    // extend operations here
};

int main(int argc, char *argv[])
{
    // prismafs init - interactive config wizard, never reaches FUSE
    if (argc > 1 && strcmp(argv[1], "init") == 0)
        return run_init();

    // POSIX version flag
    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("PrismaFS Version: %s\n", PRISMAFS_VERSION);
        return 0;
    }

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: prismafs [-c <config>] <mountpoint>\n"
               "       prismafs init\n"
               "       prismafs -v\n"
               "\n"
               "Options:\n"
               "  -c <config>  load layer configuration from <config>\n"
               "  -v           print version and exit\n"
               "  -h           print this help and exit\n"
               "\n"
               "Environment:\n"
               "  SESSION_LAYER_DIR  directory where writes are stored\n"
               "  BASE_LAYER_DIRS    comma-separated read-only base directories,\n"
               "                     listed in priority order\n"
               "\n"
               "Configuration is read from -c, then the environment,\n"
               "then ~/.config/prismafs/default.conf. All paths must be absolute.\n");
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
        if (load_config(config_path) != 0) {
            free(fuse_argv);
            return 1;
        }
    } else {
        // priority: 
        // 1. env vars 
        // 2. ~/.config/prismafs/default.conf 
        // 3. error
        char *session_dir = getenv("SESSION_LAYER_DIR");

        if (session_dir != NULL) {
            if (expand_tilde(session_dir, session_path, PATH_MAX) != 0
                || session_path[0] != '/') {
    
                fprintf(stderr,
                        "prismafs: SESSION_LAYER_DIR must be an absolute path, got '%s'\n",
                        session_dir);
                free(fuse_argv);

                return 1;
            }

            // , separated base layer dirs list
            // env which shouldnt be modified directly:
            char *base_dirs_env = getenv("BASE_LAYER_DIRS");
            
            if (base_dirs_env) {

                // modifying process environment block pointed to by pointer from getenv directly is undefined behavior. 
                // strtok modifies input to overwrite separators with \0, strdup creates private heap copy first 
                // so strtok can mutate it in safe way not affecting original 
                char *base_dirs = strdup(base_dirs_env);
                
                if (base_dirs) {
                    char *token = strtok(base_dirs, ",");

                    while (token && num_base_layers < MAX_BASE_LAYERS) {
                        if (expand_tilde(token, base_paths[num_base_layers], PATH_MAX) != 0
                            || base_paths[num_base_layers][0] != '/') {
                           
                            fprintf(stderr,
                                    "prismafs: BASE_LAYER_DIRS entry must be an absolute path, got '%s'\n",
                                    token);
                            free(base_dirs);
                            free(fuse_argv);
                       
                            return 1;
                        }

                        num_base_layers++;
                        token = strtok(NULL, ",");
                    }

                    free(base_dirs);
                }
            }
             // if session layer set, but BASE wasnt , default is "/" 
             else {
                strncpy(base_paths[0], base_path_initial, PATH_MAX - 1);
                base_paths[0][PATH_MAX - 1] = '\0';
                num_base_layers = 1;
            }
        } else {
            // when no env vars, auto detect ~/.config/prismafs/default.conf:
            int loaded = 0;
            char *home = getenv("HOME"); 

            if (home) {
                char default_conf[PATH_MAX];

                //build path for config
                snprintf(default_conf, sizeof(default_conf),
                         "%s/.config/prismafs/default.conf", home);
               
                // check it exists and load it filling session_path and base_paths
                if (access(default_conf, F_OK) == 0) {
                    if (load_config(default_conf) != 0) {
                        free(fuse_argv);
      
                        return 1;
                    }

                    loaded = 1; // success loading
                }
            }
            if (!loaded) {
                fprintf(stderr,
                    "prismafs: no config found. Options:\n"
                    "  prismafs init              - create ~/.config/prismafs/default.conf\n"
                    "  prismafs -c <config> <mnt> - use a specific config file\n"
                    "  prismafs <mnt>             - set SESSION_LAYER_DIR / BASE_LAYER_DIRS\n");
                free(fuse_argv);
                return 1;
            }
        }
    }

    int ret = fuse_main(fuse_argc, fuse_argv, &myfs_oper, NULL);
    free(fuse_argv);
    return ret;
}
