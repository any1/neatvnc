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
 * aml - https://github.com/any1/aml/
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

## Client Compatibility
 Name    | ZRLE Encoding | Tight Encoding | Crypto & Auth | SSH Tunneling
---------|---------------|----------------|---------------|--------------
bVNC     |           Yes |              ? |           Yes |          Yes
RealVNC  |           Yes |              ? |             ? |            ?
Remmina  |           Yes |            Yes |             ? |          Yes
TigerVNC |           Yes |            Yes |           Yes |            ?
TightVNC |            No |            Yes |             ? |            ?
UltraVNC |             ? |              ? |             ? |            ?
