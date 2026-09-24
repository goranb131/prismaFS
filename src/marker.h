/*
 * PrismaFS: A lightweight, layered filesystem.
 * Copyright 2026 Goran Bunić
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PRISMAFS_MARKER_H
#define PRISMAFS_MARKER_H

#include <string.h>

/* The single definition of "is this name a deletion marker?".
 *
 * It lives in its own dependency-free header so tests/test_marker.c can
 * compile it without FUSE. Every site that asks the question must call
 * this, so the answer cannot drift between call sites again.
 *
 * A marker is "<name>.deleted", and <name> is never empty: a file called
 * exactly ".deleted" is a dotfile, not a marker. That is what rmdir()
 * already meant by "len > 8".
 */
#define PRISMAFS_MARKER_SUFFIX ".deleted"

static inline int is_deleted_marker(const char *name)
{
    size_t len = strlen(name);
    size_t suffix_len = sizeof(PRISMAFS_MARKER_SUFFIX) - 1;

    return len > suffix_len &&
           strcmp(name + len - suffix_len, PRISMAFS_MARKER_SUFFIX) == 0;
}

#endif /* PRISMAFS_MARKER_H */
