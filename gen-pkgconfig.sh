#!/usr/bin/bash

cat <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: neatvnc
Description: A neat VNC server library
Version: $VERSION
Libs: -L\${libdir} -lneatvnc
Cflags: -I\${includedir}
EOF
