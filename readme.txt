Compile:

gcc -Wall -o prismafs prismafs.c -D_FILE_OFFSET_BITS=64 -I/usr/local/include /usr/local/lib/libosxfuse.2.dylib

This is because, when having macFUSE installed, we are using its libraries which are placed in and /usr/local/lib

# find /usr/local/lib -name "libosxfuse*"
/usr/local/lib/libosxfuse.2.dylib
/usr/local/lib/libosxfuse_i64.2.dylib

And header in:

# sudo ls /usr/local/include
fuse	fuse.h	node

