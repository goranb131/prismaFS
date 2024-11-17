# PrismaFS

**PrismaFS** is a lightweight, layered userspace filesystem inspired by Plan 9. It allows you to overlay a base filesystem with session-specific layers, providing isolated views and changes without affecting the original data.

## Features

- Layered filesystem design inspired by Plan 9's namespace philosophy.
- Per-session modifications that do not alter the base layer.
- Transparent file and directory operations.
- Implemented as a user-space filesystem using macFUSE.

---

## Requirements

To use PrismaFS, you need:

- **macFUSE**: Install it from [https://osxfuse.github.io/](https://osxfuse.github.io/).
- A POSIX-compliant environment (macOS or a UNIX-like system).
- **Clang** or `cc` (preferred). **GCC** is supported but not recommended.

---

## Installation

### **Building PrismaFS**

To build PrismaFS from the source, use the provided `Makefile`:

```make```

If you prefer to use GCC, you can override the default compiler by running:

```make CC=gcc```

### **Installing PrismaFS**

To install PrismaFS to `/usr/local/bin` (or another directory in your `PATH`):

```sudo make install```

You can now run `prismafs`.

### **Uninstalling PrismaFS**

To remove the installed binary:

```sudo make uninstall```

---

## Compile and Development

- **Preferred (Clang)**:

```clang -Wall -o prismafs prismafs.c -D_FILE_OFFSET_BITS=64 -I/usr/local/include /usr/local/lib/libosxfuse.2.dylib```

- **GCC**:

```gcc -Wall -o prismafs prismafs.c -D_FILE_OFFSET_BITS=64 -I/usr/local/include /usr/local/lib/libosxfuse.2.dylib```

### Notes:

When using macFUSE, its libraries are placed in `/usr/local/lib`:

```find /usr/local/lib -name "libosxfuse*"```
```/usr/local/lib/libosxfuse.2.dylib```
```/usr/local/lib/libosxfuse_i64.2.dylib```

Headers are located in:

```sudo ls /usr/local/include```
```fuse  fuse.h  node```

---

## Usage

### **Running PrismaFS**

1. Set up the required environment variables:
   - `BASE_LAYER_DIR`: The directory to be used as the base layer (e.g., `/`).
   - `SESSION_LAYER_DIR`: The directory to store session-specific changes (e.g., `/tmp/prismafs_session`).

   Example:

```export BASE_LAYER_DIR=/```
```export SESSION_LAYER_DIR=/tmp/prismafs_session```

2. Mount PrismaFS to a directory:

```mkdir /tmp/prismafs_mount```
```./prismafs /tmp/prismafs_mount```

3. Use the mounted directory (`/tmp/prismafs_mount`) to view and modify files:
   - Changes in this directory are isolated to the session layer.
   - Base layer files remain unmodified.

### **Cleaning Up**

To unmount PrismaFS:

```umount /tmp/prismafs_mount```

Or:

```fusermount -u /tmp/prismafs_mount```

---

## License

PrismaFS is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.