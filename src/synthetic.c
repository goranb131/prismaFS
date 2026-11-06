/* ============================================================
   PrismaFS - synthetic.c

   Copyright 2026 Goran B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
   ============================================================ */
 
 /* 
 * "everything is a file" entries. 
 * Initial case: cpu under /dev is included. More added with "synthetic <name>" 
 *  - registry row & generator func to extend
 *
 **/

#include "prismafs.h"
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int gen_cpu(char *buf, size_t bufsize)
{
    char brand[256];

// macOS syscall:
#ifdef __APPLE__
    size_t bl = sizeof(brand);

    if (sysctlbyname("machdep.cpu.brand_string", brand, &bl, NULL, 0) == -1)
        strncpy(brand, "Unknown CPU", sizeof(brand) - 1);
// Linux/FreBSD:
#else
    struct utsname uts;

    if (uname(&uts) == 0)
        snprintf(brand, sizeof(brand), "%s", uts.machine);
    else
        strncpy(brand, "Unknown CPU", sizeof(brand) - 1);
#endif

    return snprintf(buf, bufsize, "CPU Brand: %s\n", brand);
}

// hostname via gethostname
static int gen_hostname(char *buf, size_t bufsize)
{
    char name[256];

    if (gethostname(name, sizeof(name)) != 0)
        strncpy(name, "unknown", sizeof(name) - 1);

    return snprintf( buf
                   , bufsize
                   , "%s\n"
                   , name);
}

// for reading dev/loadavg
// getloadavg asks kernel for current 1/5/15min load averages
static int gen_loadavg(char *buf, size_t bufsize)
{
    double avg[3];

    if ( getloadavg(avg, 3) == -1)
          return snprintf(buf, bufsize, "unavailable\n");

    return snprintf( buf
                   , bufsize
                   , "%.2f %.2f %.2f\n"
                   , avg[0]
                   , avg[1]
                   , avg[2]
                   );
}

// func to build /net/interfaces content 
// interface/address = one line, cross platform via getifaddrs
static int gen_ifaces(char *buf, size_t bufsize)
{
    struct ifaddrs *ifap, *ifa;

    // libc call to fill linked list of network interfaces and addresses
    // ie. loopback, wifi etc 
    if (getifaddrs(&ifap) != 0)
        return -1;

    size_t off = 0;

    for (ifa = ifap; ifa != NULL; ifa = ifa -> ifa_next) 
    {
        // skip interfaces that might have empty address
        if (ifa -> ifa_addr == NULL)
            continue;

        // what kind of address? IPv4 or IPv6
        int fam = ifa -> ifa_addr -> sa_family;

        // skip non-IP addresses
        if (fam != AF_INET && fam != AF_INET6)
            continue;

        char host[INET6_ADDRSTRLEN];

        // convert IPv4 and IPv6 binary addresses to strings:
        // -------------------
        if (fam == AF_INET)
            
            inet_ntop( AF_INET
                     , &((struct sockaddr_in *)ifa -> ifa_addr) -> sin_addr
                     , host
                     , sizeof(host));
        else
            inet_ntop( AF_INET6
                     , &((struct sockaddr_in6 *)ifa -> ifa_addr) -> sin6_addr
                     , host
                     , sizeof(host));
        // -------------------
        int n = snprintf( buf + off // append above after existing content
                        , bufsize - off // append to remaining space in buffer so can't overflow
                        , "%-10s %-5s %s\n"
                        , ifa -> ifa_name
                        , fam == AF_INET ? "inet" : "inet6"
                        , host);

        if (n < 0 || (size_t)n >= bufsize - off)
            break;

        off += n; //advance for next line in buff
    }

    freeifaddrs(ifap); // free linked list
    return (int)off; // total bytes written
}

// array of structs of synthetic entries, {dir, name, gen}
static const syn_entry_t sytab[] = {
    { "dev", "cpu",        gen_cpu },
    { "dev", "hostname",   gen_hostname },
    { "dev", "loadavg",    gen_loadavg },
    { "net", "interfaces", gen_ifaces },
};

// const for num of rows in sytab
#define SYTAB_N (sizeof(sytab) / sizeof(sytab[0]))

#define MAX_SYN 16
static char syon[MAX_SYN][32] = { "cpu" }; // cpu included by default
static int nsyon = 1;

// checker for enable status of synth entry by name
static int is_on(const char *name)
{
    for (int i = 0; i < nsyon; i++)
        if (strcmp(syon[i], name) == 0) // compare name with syon array enabled entries
            return 1;

    return 0;
}

// add more synthetic entries to enabled list
// note: for now limit to MAX_SYN is 16. TODO: open to dynamic allocation tradeoffs
void syn_en(const char *name)
{
    if (is_on(name))
        return;

    if (nsyon >= MAX_SYN) {
        fprintf(stderr, "prismafs: too many synthetic entries, ignoring '%s'\n", name);
        return;
    }

    strncpy(syon[nsyon], name, sizeof(syon[0]) - 1);
    nsyon++;
}

// user defined command based entries
static syn_entry_t *cytab = NULL;
static int ncytab = 0;
static int capcytab = 0;

// register cmd entry. enabled by default
void syn_add_cmd(const char *dir, const char *name, const char *cmd)
{
    if (ncytab >= capcytab) 
    {
        capcytab = capcytab ? capcytab * 2 : 4;
        cytab    = realloc(cytab, capcytab * sizeof(*cytab));
    }

    cytab[ncytab].dir  = strdup(dir);
    cytab[ncytab].name = strdup(name);
    cytab[ncytab].gen  = NULL;
    cytab[ncytab].cmd  = strdup(cmd);
    ncytab++;
}

// count of enabled synth entries per dir (compiled + command-backed)
int syn_cnt(const char *dir)
{
    int n = 0;

    for (size_t i = 0; i < SYTAB_N; i++)
        if (strcmp(sytab[i].dir, dir) == 0 && is_on(sytab[i].name))
            n++;

    for (int i = 0; i < ncytab; i++)
        if (strcmp(cytab[i].dir, dir) == 0)
            n++;

    return n;
}

// get name of enabled synth entry by dir and index, for example for readdir to
// list all enabled under dev or /net
const char *syn_nm(const char *dir, int idx)
{
    int n = 0;

    for (size_t i = 0; i < SYTAB_N; i++) 
    {
        if (strcmp(sytab[i].dir, dir) == 0 && is_on(sytab[i].name)) 
        {
            if (n == idx)
                return sytab[i].name;

            n++;
        }
    }

    for (int i = 0; i < ncytab; i++)
    {
        if (strcmp(cytab[i].dir, dir) == 0)
        {
            if (n == idx)
                return cytab[i].name;

            n++;
        }
    }

    return NULL;
}

// resolves full FUSE path for registry row so getattr/open/read can stay
// generic instead of IF blocks hardcoding for every synth entry
const syn_entry_t *syn_fp(const char *path)
{
    char full[64];

    for (size_t i = 0; i < SYTAB_N; i++) 
    {
        snprintf(full, sizeof(full), "/%s/%s", sytab[i].dir, sytab[i].name);

        if (strcmp(path, full) == 0 && is_on(sytab[i].name))
            return &sytab[i];
    }

    for (int i = 0; i < ncytab; i++)
    {
        snprintf(full, sizeof(full), "/%s/%s", cytab[i].dir, cytab[i].name);

        if (strcmp(path, full) == 0)
            return &cytab[i];
    }

    return NULL;
}

// central func to handle command defined entries
// stdout for file content when running entry command
static int gen_cmd(const char *cmd, char *buf, size_t bufsize)
{
    // run command in child and provide file to read from as stdout
    FILE *fp = popen(cmd, "r");

    if (!fp)
        return -1;

    size_t n = fread(buf, 1, bufsize - 1, fp);

    pclose(fp);

    buf[n] = '\0';

    return (int)n;
}

// compiled gen or command based
int syn_gen(const syn_entry_t *se, char *buf, size_t bufsize)
{
    if (se -> gen) // does this row have compiled func pointer set
        return se -> gen(buf, bufsize);

    // when above is not true, its command based
    return gen_cmd(se -> cmd, buf, bufsize);
}
