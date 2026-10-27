/* ============================================================
   PrismaFS - sign.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */

/*
 * Applicable commands overview:
 *
 * "prismafs keygen" - create local Ed25519 identity
 * "prismafs sign <session-dir>" - sign session manifest
 * "prismafs verify <session-dir> [-k <pubkey-file>]" - check signature
 *
 */

#include "prismafs.h"
#include <sodium.h>

static void sign_usg(FILE *out)
{
    fprintf(out, "Usage: prismafs sign <session-dir>\n");
}

static void vrfy_usg(FILE *out)
{
    fprintf(out, "Usage: prismafs verify <session-dir> [-k <pubkey-file>]\n");
}

// build path strings for public and private keys
// ~/.config/prismafs/identity.{key,pub} 
// config dir for identity file is same as for run_init() 
static int idpaths(char keyp[PATH_MAX], char pubp[PATH_MAX])
{
    char *home = getenv("HOME");

    if (!home) {

        fprintf(stderr, "prismafs: HOME is not set\n");

        return -1;
    }

    char confd[PATH_MAX];

    snprintf(confd, sizeof(confd), "%s/.config/prismafs", home);
    snprintf(keyp, PATH_MAX, "%s/identity.key", confd);
    snprintf(pubp, PATH_MAX, "%s/identity.pub", confd);

    return mkdir_p(confd, 0755);
}

// called from keygen_command() to generate keys
// params:
//   path - where to create file
//   hex - ASCII hex string going into file
//   mode - permission mode
// - set bits for permission mode in same syscall to avoid having weaker permissions
// if file were created first and then chmod-ed
//
static int wrhex(const char *path, const char *hex, mode_t mode)
{
    int fd = open( path
                 , O_WRONLY | O_CREAT | O_TRUNC
                 , mode);

    if (fd < 0)
        return -1;

    FILE *f = fdopen(fd, "w");

    if (!f) {
        close(fd);

        return -1;
    }

    fprintf(f, "%s\n", hex);
    fclose(f);

    return 0;
}

// - called from sign_command() and verify_command()
//
// inverse of wrhex() - read ASCII hex from file and decode to raw binary bytes
// params:
//   path - file read from
//   bin - buffer for storing decoded bytes
//   bin_len - buff size, also used for sodium_hex2bin as byte length
static int rdhex(const char *path, unsigned char *bin, size_t bin_len)
{
    FILE *f = fopen(path, "r");

    if (!f)
        return -1;

    char hex[256];

    if (!fgets(hex, sizeof(hex), f)) {

        fclose(f);

        return -1;
    }

    fclose(f);

    size_t len = strlen(hex);

    while ( len > 0 && (hex[len - 1] == '\n' 
          || hex[len - 1] == '\r') )
        hex[--len] = '\0';

    return sodium_hex2bin( bin
                         , bin_len
                         , hex
                         , len
                         , NULL
                         , NULL
                         , NULL
                         );
}

// to generate Ed25519 key pair
int keygen_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (sodium_init() < 0) {

        fprintf(stderr, "prismafs keygen: libsodium init failed\n");

        return 1;
    }

    char keyp[PATH_MAX], pubp[PATH_MAX];

    if ( idpaths(keyp, pubp) != 0 ) {

        fprintf( stderr, "prismafs keygen: cannot create config dir: %s\n"
               , strerror(errno));

        return 1;
    }

    if (access(keyp, F_OK) == 0) {

        printf( "Identity already exists at %s\nOverwrite? [y/N]: "
              , keyp);

        fflush(stdout);

        char ans[8];

        if ( !fgets(ans, sizeof(ans), stdin) 
           || (ans[0] != 'y' && ans[0] != 'Y')) 
        {
            printf("Aborted.\n");

            return 0;
        }
    }

    // binary byte buffers for keys
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];

    // call libsodium to generate Ed25519 key pair
    // 32 random bytes for private key, 64 bytes for secret key
    crypto_sign_keypair(pk, sk);

    // byte becomes 2 hex chars + terminator '\0'
    char pk_hex[crypto_sign_PUBLICKEYBYTES * 2 + 1];
    // 64*2+1
    char sk_hex[crypto_sign_SECRETKEYBYTES * 2 + 1];

    // raw bytes to hex
    sodium_bin2hex(pk_hex, sizeof(pk_hex), pk, sizeof(pk));
    sodium_bin2hex(sk_hex, sizeof(sk_hex), sk, sizeof(sk));

    // secret key = 0600, public key = readable 0644
    if (wrhex(keyp, sk_hex, 0600) != 0
        || wrhex(pubp, pk_hex, 0644) != 0) {

        fprintf( stderr
               , "prismafs keygen: cannot write identity files: %s\n"
               , strerror(errno));

        // secret key wiping from memory before potential error exit
        sodium_memzero(sk, sizeof(sk));
        sodium_memzero(sk_hex, sizeof(sk_hex));

        return 1;
    }

    // secret key wipe from memory once its on disk
    sodium_memzero(sk, sizeof(sk));
    sodium_memzero(sk_hex, sizeof(sk_hex));

    printf("Identity created.\n");
    printf("  private key: %s (keep this secret)\n", keyp);
    printf("  public key:  %s\n", pubp);
    printf("  public key hex: %s\n", pk_hex);

    return 0;
}

// split manifest into "body" with existing 
// hash/pubkey/signature lines 
static int splitman(const char *manifest_path, char **body_out, size_t *body_len_out,
                     char *hash_hex, size_t hash_hex_sz,
                     char *pubkey_hex, size_t pubkey_hex_sz,
                     char *sig_hex, size_t sig_hex_sz)
{
    // reset
    hash_hex[0] = pubkey_hex[0] = sig_hex[0] = '\0';

    FILE *f = fopen(manifest_path, "r");

    if (!f)
        return -1;

    size_t cap = 65536, len = 0;
    char *body = malloc(cap);

    if (!body) {

        fclose(f);

        return -1;
    }

    char line[PATH_MAX + 32];

    while (fgets(line, sizeof(line), f)) {

        // pull out "line" which is hash value (not copy to body) 
        if (strncmp(line, "hash ", 5) == 0) 
        {
            // in hash read by fgets, skip 5 chars (hash prefix), copy that 
            // and newline into hash_hex ( -1 for null terminator)
            strncpy(hash_hex, line + 5, hash_hex_sz - 1);
            
            // guard in edge case when manifest was hand edited 
            // which would make hash_hex unterminated
            hash_hex[hash_hex_sz - 1] = '\0';

            // trim trailing newline
            size_t n = strlen(hash_hex);

            while (n > 0 && (hash_hex[n - 1] == '\n' 
                   || hash_hex[n - 1] == '\r'))
                hash_hex[--n] = '\0';

            continue;
        }

        // same as above, for pubkey prefix
        if (strncmp(line, "pubkey ", 7) == 0) 
        {
            strncpy(pubkey_hex, line + 7, pubkey_hex_sz - 1);

            pubkey_hex[pubkey_hex_sz - 1] = '\0';

            size_t n = strlen(pubkey_hex);

            while (n > 0 && (pubkey_hex[n - 1] == '\n' 
                  || pubkey_hex[n - 1] == '\r'))
                pubkey_hex[--n] = '\0';

            continue;
        }

        // for "signature " prefix
        if (strncmp(line, "signature ", 10) == 0) 
        {
            strncpy(sig_hex, line + 10, sig_hex_sz - 1);

            sig_hex[sig_hex_sz - 1] = '\0';

            size_t n = strlen(sig_hex);

            while (n > 0 && (sig_hex[n - 1] == '\n' 
                  || sig_hex[n - 1] == '\r'))
                sig_hex[--n] = '\0';

            continue;
        }

        // growing body buffer if needed depending on cap:
        size_t llen = strlen(line);

        if ( len + llen >= cap ) 
        {
            cap *= 2;

            char *grown = realloc(body, cap);

            if (!grown) 
            {
                free(body);
                fclose(f);
                return -1;
            }

            body = grown;
        }

        memcpy(body + len, line, llen);
        len += llen;
    }

    fclose(f);

    body[len] = '\0';
    *body_out = body;
    *body_len_out = len;

    return 0;
}

// filter out . and .. 
static int nodots(const struct dirent *de)
{
    return strcmp(   de -> d_name, ".") != 0 
                  && strcmp(de->d_name, "..") != 0;
}

// walk directory tree and identify entries then put them in hash incrementally
// alphasort to sort content alphabetically to be same on every system
static int hwalk(crypto_hash_sha256_state *st, const char *root, const char *rel)
{
    char dirpath[PATH_MAX];

    if (rel[0])
        snprintf(dirpath, PATH_MAX, "%s/%s", root, rel);
    else
        snprintf(dirpath, PATH_MAX, "%s", root);

    struct dirent **namelist;
    int n = scandir(dirpath, &namelist, nodots, alphasort);

    if (n < 0)
        return -1;

    for (int i = 0; i < n; i++) {

        char entry_rel[PATH_MAX];

        if (rel[0])
            snprintf(entry_rel
                    , PATH_MAX
                    , "%s/%s"
                    , rel
                    , namelist[i] -> d_name);
        else
            snprintf(entry_rel
                    , PATH_MAX
                    , "%s"
                    , namelist[i] -> d_name);

        char entry_path[PATH_MAX];
        snprintf(entry_path, PATH_MAX, "%s/%s", dirpath, namelist[i] -> d_name);

        struct stat st_entry;

        if (lstat(entry_path, &st_entry) != 0) 
        {
            free(namelist[i]);
            continue;
        }

        char mode_str[8];

        snprintf(mode_str
                , sizeof(mode_str)
                , "%04o"
                , st_entry.st_mode & 07777);

        // relative path of every entry into hash 
        crypto_hash_sha256_update(st, (const unsigned char *)entry_rel, strlen(entry_rel));

        if (S_ISDIR(st_entry.st_mode)) {

            // type tag and mode and recurse into subdir
            crypto_hash_sha256_update(st, (const unsigned char *)"D", 1);
            crypto_hash_sha256_update(st, (const unsigned char *)mode_str, strlen(mode_str));

            hwalk(st, root, entry_rel);

        } else if (S_ISLNK(st_entry.st_mode)) {

            // when symlink, readlink() finds target path for actual content
            // if target string is changed it was tampered. important for verify
            char target[PATH_MAX] = {0};
            ssize_t tn = readlink(entry_path, target, sizeof(target) - 1);

            crypto_hash_sha256_update(st, (const unsigned char *)"L", 1);
            crypto_hash_sha256_update(st, (const unsigned char *)mode_str, strlen(mode_str));

            if (tn > 0)
                crypto_hash_sha256_update(st, (const unsigned char *)target, (size_t)tn);

        } else {

            // regular file, stream content through hash in chunks
            crypto_hash_sha256_update(st, (const unsigned char *)"F", 1);
            crypto_hash_sha256_update(st, (const unsigned char *)mode_str, strlen(mode_str));

            FILE *f = fopen(entry_path, "rb");

            if (f) {
                unsigned char buf[8192];
                size_t nr;

                while ( (nr = fread(buf, 1, sizeof(buf), f)) > 0 )
                    crypto_hash_sha256_update(st, buf, nr);

                fclose(f);
            }
        }

        free(namelist[i]);
    }

    free(namelist);

    return 0;
}

// produce fingerpring (SHA-256) 
// which is full state of session (manifest data and file contents)
// as one hash value
static void sesshash(const char *session_dir, const char *body, size_t body_len,
                      unsigned char out_hash[crypto_hash_sha256_BYTES])
{
    crypto_hash_sha256_state st;

    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, (const unsigned char *)body, body_len);
    hwalk(&st, session_dir, "");
    crypto_hash_sha256_final(&st, out_hash);
}

int sign_command(int argc, char **argv)
{
    // session dir arg
    if (argc < 3) {
        sign_usg(stderr);

        return 1;
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "prismafs sign: libsodium init failed\n");
        
        return 1;
    }

    // resolve ~. need absolute path
    char session_dir[PATH_MAX];

    if ( expand_tilde(argv[2], session_dir, sizeof(session_dir) ) != 0
        || session_dir[0] != '/') {

        fprintf(stderr, "prismafs sign: session dir must be absolute path, got '%s'\n", argv[2]);

        return 1;
    }

    // session dir needs to exist
    struct stat st;

    if (stat(session_dir, &st) != 0 || !S_ISDIR(st.st_mode)) 
    {
        fprintf(stderr, "prismafs sign: '%s' is not a directory\n", session_dir);
      
        return 1;
    }

    // manifest needs to exist, after first mount
    char manifest_path[PATH_MAX];

    if (session_manifest_path(session_dir, manifest_path) != 0)
        return 1;

    if (access(manifest_path, F_OK) != 0) 
    {
        fprintf(stderr, "prismafs sign: no session manifest found for '%s' "
                        "(mount it first)\n", session_dir);
        return 1;
    }

    // identity to sign with
    char keyp[PATH_MAX], pubp[PATH_MAX];

    if (idpaths(keyp, pubp) != 0)
        return 1;

    // load secret key for signing
    unsigned char sk[crypto_sign_SECRETKEYBYTES];

    if (rdhex(keyp, sk, sizeof(sk)) != 0) 
    {
        fprintf(stderr, "prismafs sign: no identity found, run 'prismafs keygen' first\n");
       
        return 1;
    }

    char *body;
    size_t body_len;
    char old_hash[128], old_pubkey[128], old_sig[384];

    // strip previous hash/pubkey/signature lines
    if (splitman(manifest_path, &body, &body_len,
                 old_hash, sizeof(old_hash),
                 old_pubkey, sizeof(old_pubkey),
                 old_sig, sizeof(old_sig)) != 0) {

        fprintf(stderr, "prismafs sign: cannot read manifest '%s'\n", manifest_path);
       
        sodium_memzero(sk, sizeof(sk));

        return 1;
    }

    unsigned char hash[crypto_hash_sha256_BYTES];

    sesshash(session_dir, body, body_len, hash);

    // derive public key from secret key
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    crypto_sign_ed25519_sk_to_pk(pk, sk);

    // sign the hash (not raw tree data)
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_detached(sig, NULL, hash, sizeof(hash), sk);

    sodium_memzero(sk, sizeof(sk));

    char hash_hex[crypto_hash_sha256_BYTES * 2 + 1];
    char pk_hex[crypto_sign_PUBLICKEYBYTES * 2 + 1];
    char sig_hex[crypto_sign_BYTES * 2 + 1];

    sodium_bin2hex(hash_hex, sizeof(hash_hex), hash, sizeof(hash));
    sodium_bin2hex(pk_hex, sizeof(pk_hex), pk, sizeof(pk));
    sodium_bin2hex(sig_hex, sizeof(sig_hex), sig, sizeof(sig));

    // for signing, rewrite manifest instead of appending to old crypto lines
    FILE *f = fopen(manifest_path, "w");

    if (!f) 
    {
        fprintf(stderr, "prismafs sign: cannot write manifest '%s': %s\n",
                manifest_path, strerror(errno));
       
                free(body);
       
        return 1;
    }

    fwrite(body, 1, body_len, f);
    fprintf(f, "hash %s\n", hash_hex);
    fprintf(f, "pubkey %s\n", pk_hex);
    fprintf(f, "signature %s\n", sig_hex);
    fclose(f);

    free(body);

    printf("Signed session '%s'.\n", session_dir);
    printf("  hash:      %s\n", hash_hex);
    printf("  signed by: %s\n", pk_hex);

    return 0;
}

int verify_command(int argc, char **argv)
{
    if (argc < 3) {
        vrfy_usg(stderr);
        return 1;
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "prismafs verify: libsodium init failed\n");
        return 1;
    }

    // optional -k flag to use specific pub key
    const char *pinp = NULL;

    for (int i = 3; i < argc; i++) 
    {
        if ( strcmp(argv[i], "-k") == 0 && i + 1 < argc )
            pinp = argv[++i];
    }

    // resolve ~. must be absolute path
    char session_dir[PATH_MAX];

    if (expand_tilde(argv[2], session_dir, sizeof(session_dir)) != 0
        || session_dir[0] != '/') {

        fprintf(stderr, "prismafs verify: session dir must be absolute path, got '%s'\n", argv[2]);
      
        return 1;
    }

    char manifest_path[PATH_MAX];

    if (session_manifest_path(session_dir, manifest_path) != 0)
        return 1;

    char *body;
    size_t body_len;
    char hash_hex[128], pubkey_hex[128], sig_hex[384];

    // same split like in sign_command, body is everything except crypto lines
    if (splitman(manifest_path, &body, &body_len,
                 hash_hex, sizeof(hash_hex),
                 pubkey_hex, sizeof(pubkey_hex),
                 sig_hex, sizeof(sig_hex)) != 0) {

        fprintf(stderr, "prismafs verify: cannot read manifest for '%s'\n", session_dir);
        
        return 1;
    }

    if (!hash_hex[0] || !pubkey_hex[0] || !sig_hex[0]) 
    {
        fprintf(stderr, "prismafs verify: session is not signed, run 'prismafs sign' first\n");
       
        free(body);
       
        return 1;
    }

    unsigned char stored_hash[crypto_hash_sha256_BYTES];
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sig[crypto_sign_BYTES];

    // decode hex from manifest to raw bytes
    if (sodium_hex2bin(stored_hash, sizeof(stored_hash), hash_hex, strlen(hash_hex), NULL, NULL, NULL) != 0
        || sodium_hex2bin(pk, sizeof(pk), pubkey_hex, strlen(pubkey_hex), NULL, NULL, NULL) != 0
        || sodium_hex2bin(sig, sizeof(sig), sig_hex, strlen(sig_hex), NULL, NULL, NULL) != 0) {

        fprintf(stderr, "prismafs verify: manifest has malformed hash/pubkey/signature\n");
        
        free(body);
        
        return 1;
    }

    unsigned char recomputed[crypto_hash_sha256_BYTES];

    // rehash tree and compare with hash that was stored when signing
    sesshash(session_dir, body, body_len, recomputed);

    free(body);

    if (sodium_memcmp(recomputed, stored_hash, sizeof(recomputed)) != 0) 
    {
        printf("INVALID: session contents do not match with signed hash\n");
        printf("  (files, permissions, or manifest changed since 'prismafs sign')\n");
       
        return 1;
    }

    // hash matches. check signature was created by embedded pubkey
    if (crypto_sign_verify_detached(sig, recomputed, sizeof(recomputed), pk) != 0) 
    {
        printf("INVALID: signature does not match\n");

        return 1;
    }

    // identity check with -k option (pubkey provided by -k)
    if (pinp) 
    {
        unsigned char expected_pk[crypto_sign_PUBLICKEYBYTES];

        // read pubkey hex and decode to raw bytes
        if (rdhex(pinp, expected_pk, sizeof(expected_pk)) != 0) 
        {
            fprintf(stderr, "prismafs verify: cannot read pubkey file '%s'\n", pinp);
            
            return 1;
        }

        // compare pubkey to expected that was embedded in manifest
        if (sodium_memcmp(expected_pk, pk, sizeof(pk)) != 0) 
        {
            printf("INVALID: signed by different identity than expected (-k mismatch)\n");
            
            return 1;
        }
    }

    printf("VALID: session matches its signature\n");
    printf("  signed by: %s\n", pubkey_hex);

    return 0;
}
