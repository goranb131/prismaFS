# PrismaFS

PrismaFS is a userspace filesystem, built on FUSE and macFUSE. First step is mounting directory or directories from where you can point PrismaFS to your HOME path, project tree, read-only volumes, or any other path, and then read, write, create, delete, and rename files. Multiple source directories can exist, with your session working directory having top priority. So, "session" is type of a mounted view of one or more sources, with dedicated writable/working space for all changes made in that session. One of the main points is that the original source directory is never touched.

When you work with files/directories over the mount, PrismaFS first looks for it in your session directory. If its not there, it reads from source. If you write to it, PrismaFS copies it into your session first, then writes there. If you delete any files found only in source, PrismaFS tracks that in session space (hidden marker file), and those file disappear from the mounted view. In all three cases, source is unmodified.

What this gives you is a working view of any source directory with full read/write access, and every actual change stays isolated. When you are done, unmount and delete your working directory. Nothing you did affected the source.

This model of layered design is inspired by Plan 9 model of filesystem namespaces. However, Plan 9 applied this at operating system / kernel design level (per-process). On standard UNIX and UNIX-like operating systems, filesystem namespace is global and shared. Getting the isolated working copy of directory, be it a home folder, config tree, read-only volume, or any other path, normally means you need a container, VM, or bind mount with required root access.

In Plan 9, every process has a private namespace, which is its own view of the filesystem. A process can bind any directory into any other path, combine multiple directories together at the same mount point, and/or replace what any path points to, but never affecting another process. For example, you could give one process view of / that has different bin, different usr, different etc, while everything else on your system sees the original. Any terminal session is a process. In one terminal you could have different content in the same HOME path, for that session, and other terminals opened outside of namespace session will see originals. This isolation at filesystem level was built into operating system itself.

Plan 9 is not UNIX but intended to take the UNIX philosophy to the next step with distributed computing concept and namespace ideas. It is still developed today as 9front.

UNIX came before these ideas and implies decades of software, tooling, and institutional use that would make kernel rewrite in this way impossible. So instead, filesystem isolation on UNIX took the form of different solutions, such as chroot, BSD jails, Solaris zones, Linux namespaces and cgroups, eventually Docker and other container runtimes, as well as VMs for hardware separation and isolation. Some of these are good solutions, but are often heavyweight, complicated, require elevated privileges, and do not feel like small composable CLI tools. For large class of tasks, in order to get isolated view of directory tree, test a dependency without affecting your real environment, or simply experiment with directories without risk, the overhead is real. There is also inconvenience for the fact that these solutions are not unified, standardized, or POSIX compliant between different UNIX-like operating systems and environments.

PrismaFS aims to bring this filesystem aspect to userspace on macOS, Linux, and FreeBSD, as closely and effectively as possible.

Apache-2.0 licensed.

## Platform support

- **macOS** - FUSE2 (macFUSE). Tested.
- **Linux** - FUSE3 (libfuse3). Tested on openSUSE Tumbleweed (native build) and via static linked libfuse3 binary built in Debian bookworm container, verified with `fusermount3 3.18.2`.
- **FreeBSD** - in Roadmap. Not built or tested yet.
- **Architecture** - x86_64 and Apple Silicon (ARM64). Linux binaries are currently x86_64. Linux ARM64 will become priority in future.

For Linux binary distributions: glibc-linked binaries only run on glibc-based distros (Ubuntu, Debian, Fedora, openSUSE, Arch, RHEL family, etc.). musl-based distros (Alpine) need a separate musl build, and glibc binary will not run there at all, because musl and glibc are different C library implementations with different dynamic loaders.

## Requirements

**macOS**
- macFUSE, installed separately (kernel extension approval and reboot required on first install). See instructions on official macFUSE website to complete this only required preliminary step: https://macfuse.github.io/
- Homebrew is the simplest install approach. Building from source requires C compiler (cc, clang, or GCC) as usual, and *make*.

**Linux**
- libfuse3 and its development headers to build from source (`libfuse3-dev` / `fuse3-devel` depending on distro).
- `fusermount3` available on system (part of `fuse3` package).

## Installation

### macOS, via Homebrew

```
brew install goranb131/ithas-prismafs/prismafs
```

Installing by this qualified name trusts only this formula, so no separate `tap` or `trust` step is needed.

Longer approach, if you want to have tap added by name explicitly:

```
brew tap goranb131/ithas-prismafs
brew trust goranb131/ithas-prismafs
brew install prismafs
```

`brew trust` is required now since Homebrew 6.x. Without it, `brew install` fails with "Refusing to load formula ... from untrusted tap".

### Linux, build from source

```
git clone git@github.com:goranb131/ITHAS-prismaFS.git
cd ITHAS-prismaFS
make
sudo make install
```

`make install` places the binary at `/usr/local/bin/prismafs` and *man page* under `/usr/local/share/man/man1`.

### Linux, prebuilt binary

Prebuilt, static linked (with libfuse3) x86_64 binary is available as alternative to building from source, attached to each [GitHub Release](https://github.com/goranb131/prismaFS/releases). It has no dependency on host's libfuse3 shared library version. Only glibc, `fusermount3`, and kernel FUSE module are requirement on target machine. This is early stage development extra, because while MacOS version uses Homebrew, Linux has the binaries option, before packages are distributed in other formats later in Roadmap.

To install:

```
mkdir -p ~/.local/bin
cp prismafs-<version>-linux-x86_64-static ~/.local/bin/prismafs
chmod +x ~/.local/bin/prismafs
```

Or system global:

```
sudo cp prismafs-<version>-linux-x86_64-static /usr/local/bin/prismafs
sudo chmod +x /usr/local/bin/prismafs
```

At this earlier stage of development, PrismaFS also minds Alpine and other musl-based distros, so separate static binary is attached to same [GitHub Release](https://github.com/goranb131/prismaFS/releases): `prismafs-<version>-linux-x86_64-static-musl`. It has musl and libfuse3 compiled in, so it has no dependency on host libc or libfuse3 version, and runs on any Linux distro, not only Alpine. Install steps are the same, just with this filename instead.

## Configuration

PrismaFS resolves its layer configuration in this order:

1. `-c <config-file>` CLI flag if provided.
2. `SESSION_LAYER_DIR` and `BASE_LAYER_DIRS` environment variables, if `-c` is not provided.
3. config file by auto-detection at `~/.config/prismafs/default.conf`, when above two points dont apply or environment vars are not provided.
4. If above dont resolve, PrismaFS prints usage guide and exits without mounting.

### Guided setup

```
prismafs init
```

Prompts for SESSION directory and BASE directory, then writes `~/.config/prismafs/default.conf`. After this, running `prismafs <mountpoint>` with no other arguments uses this file automatically.

### Config file format

Plain text, one directive per line. Lines starting with `#` are comments, empty lines ignored.

```
# my analysis environment
session /tmp/session
base /opt/project-base
base /usr/local/share/common
```

- `session <path>`: required, exactly once. Directory used for all writes.
- `base <path>`: required, at least once. Can be repeated. Order determines reading priority, first listed is top priority if conflicting at BASE layers.

### Environment variable format

```
export SESSION_LAYER_DIR=/tmp/session
export BASE_LAYER_DIRS=/tmp/base1,/tmp/base2
```

`BASE_LAYER_DIRS` is comma separated list. Order determines reading priority, same as in case of config file.

## Command-line reference

```
prismafs [-c <config>] [-v | -h] <mountpoint>
prismafs init
```

- `<mountpoint>`: directory to mount PrismaFS to. Must exist.
- `-c <config>`: use provided config file instead of environment variables or auto-detection.
- `-v`: print version, exit.
- `-h`: print usage, exit.
- `init`: interactive wizard, creates and fills `~/.config/prismafs/default.conf`.

## Usage example

```
mkdir -p ~/pfs-base ~/pfs-session ~/pfs-mnt
echo hello > ~/pfs-base/hello.txt

export SESSION_LAYER_DIR=~/pfs-session
export BASE_LAYER_DIRS=~/pfs-base
prismafs ~/pfs-mnt

cat ~/pfs-mnt/hello.txt        # reads from base layer
echo world > ~/pfs-mnt/new.txt # writting to session layer only
ls ~/pfs-session                # new.txt appears here, hello.txt does not
```

## Unmounting

**macOS**

```
umount <mountpoint>
```

**Linux**

```
fusermount3 -u <mountpoint>
```

Unmounting does not delete the SESSION layer directory or its contents. Deleting SESSION layer directory afterward discards every change made during that session, BASE layers are unaffected regardless.

## Known limitations

Current version.

- Single session layer per mount, no nested or stacked session layers yet.
- No built-in encryption of session-layer contents yet.
- No network filesystem support, local directories only, for both base and session layers.

## License

Apache-2.0. See [LICENSE](LICENSE).
