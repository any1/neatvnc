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
meson build
ninja -C build
```
