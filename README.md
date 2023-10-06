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
 * aml - https://github.com/any1/aml/
 * ffmpeg (optional)
 * gbm (optional)
 * gnutls (optional)
 * libdrm (optional)
 * libturbojpeg (optional)
 * nettle (optional)
 * hogweed (optional)
 * gmp (optional)
 * pixman
 * zlib

### Build Dependencies
 * libdrm
 * meson
 * pkg-config

To build just run:
```
meson build
ninja -C build
```
