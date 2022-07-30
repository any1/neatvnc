# Neat VNC

## Introduction
This is a liberally licensed VNC server library that's intended to be fast and
neat.

## Goals
 * Speed.
 * Clean interface.
 * Interoperability with the Freedesktop.org ecosystem.

## Building

### Runtime Dependencies
 * pixman
 * aml - https://github.com/any1/aml/
 * zlib
 * gnutls (optional)
 * libturbojpeg (optional)

### Build Dependencies
 * meson
 * pkg-config
 * libdrm

To build just run:
```
meson build
ninja -C build
```
