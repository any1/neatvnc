# Neat VNC (Beta)

## Introduction
This is a liberally licensed VNC server library that's intended to be fast and
neat. Note: This is a beta release, so the interface is not yet stable.

## Goals
 * Speed.
 * Clean interface.
 * Interoperability with the Freedesktop.org ecosystem.

## Building

### Runtime Dependencies
 * pixman
 * libuv
 * libpng (only needed for examples and benchmarks)

### Build Dependencies
 * GNU Make
 * GCC or Clang
 * pkg-config
 * libdrm

To build just run:
```
make 
```

### Installing
```
make install
```

### Variables
 * `CFLAGS`: Flags passed to the compiler.
 * `LDFLAGS`: Flags passed to the linker.
 * `BUILD_DIR`: Destination directory for the build.
 * `PREFIX`: System prefix. Default: `/usr/local`.
 * `DESTDIR`: Destination directory for install. This is used by system
   package system such as dpkg, rpm and pacman.
 * `PKGCONFIG`: `pkg-config` executable path.
 * `STRIP`: `strip` executable path.
 * `DONT_STRIP`: Set this is the installed DSO is not to be stripped of its
   debugging symbols.

### Cross-compiling
Generally, it should be enough to set `CC=<architecture-triplet>-gcc` and then
run `make`, e.g.:
```
CC=arm-linux-gnueabihf-gcc make
```
If you have a `pkg-config` wrapper at `<triplet>-pkg-config` it will be run
instead if `pkg-config`, but `pkg-config` can also be overridden by setting the
`PKGCONFIG` environment variable prior to running `make`.
