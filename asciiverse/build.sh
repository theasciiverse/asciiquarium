#!/bin/sh
# Builds the C port of asciiquarium as ./asciiquarium-bin (the original
# Perl script keeps the name asciiquarium in this repo). Linux: run inside
# alpine:3.22 for a fully static binary (ncurses linked in); macOS: run on
# the runner with CC_ARCH set to x86_64 or arm64, linking the system's
# ncurses. Other Linux (local testing) links ncursesw shared.
#   docker run --rm -v "$PWD":/src -w /src alpine:3.22 sh asciiverse/build.sh
# src/art.h is generated from the original by tools/extract-art.pl and
# committed, so the build needs no Perl.
set -eu
if [ -f /etc/alpine-release ]; then
  apk add --no-cache build-base ncurses-dev ncurses-static >/dev/null
  libs="-static -lncursesw"
elif [ "$(uname)" = Darwin ]; then
  libs="-lncurses"
else
  libs="-lncursesw"
fi
cc="cc${CC_ARCH:+ -arch $CC_ARCH}"
rm -f asciiquarium-bin
$cc -O2 -Wall -o asciiquarium-bin src/asciiquarium.c $libs
strip asciiquarium-bin
