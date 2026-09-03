/*
 * Regression test for the ".deleted" marker predicate.
 *
 * Background: readdir() used strstr(name, ".deleted") while rmdir() used a
 * suffix check. The two disagreed, so a real file named budget.deleted.pdf
 * was created and stored normally but never appeared in readdir(). This test
 * pins the one definition both sites now share.
 *
 * No FUSE and no mount needed: it is pure logic on names.
 *
 *   cc -Wall -Isrc -o test_marker tests/test_marker.c && ./test_marker
 */

#include <stdio.h>
#include "marker.h"

struct case_t {
    const char *name;
    int         is_marker;   /* what the filesystem must answer */
    const char *why;
};

static const struct case_t cases[] = {
    /* real markers: these MUST be recognised, or deletion stops working.
       They are the positive control: a predicate that always answers "no"
       fails here. */
    { "notes.deleted",          1, "marker for notes" },
    { "a.deleted",              1, "shortest real marker" },
    { "my.notes.odt.deleted",   1, "marker for a name with dots" },

    /* regressions: ordinary files that the old readdir() hid.
       A predicate that always answers "yes" fails here. */
    { "budget.deleted.pdf",     0, "user file, .deleted in the middle" },
    { "archive.deleted_2024",   0, "user file, suffix does not match" },
    { "photo.deletedX",         0, "user file, one trailing char" },
    { "my.deleted.notes.odt",   0, "user file, .deleted in the middle" },
    { "rapport.deleted.txt",    0, "user file, .deleted in the middle" },

    /* boundaries */
    { ".deleted",               0, "a dotfile, not a marker: no name in front" },
    { "deleted",                0, "no dot, no suffix" },
    { "deleted.txt",            0, "ordinary file" },
    { "",                       0, "empty name" },
};

int main(void)
{
    int failures = 0;
    size_t n = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < n; i++) {
        int got = is_deleted_marker(cases[i].name) ? 1 : 0;

        if (got != cases[i].is_marker) {
            printf("FAIL  \"%s\": expected %d, got %d  (%s)\n",
                   cases[i].name, cases[i].is_marker, got, cases[i].why);
            failures++;
        }
    }

    printf("%zu cases, %d failures\n", n, failures);
    return failures == 0 ? 0 : 1;
}
