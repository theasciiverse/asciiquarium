#!/bin/sh
# Lays out the av archive in stage/: the binary at the top (no data files;
# the art is compiled in), with the licence, the original README and
# CHANGES, and PATCHES.md, which explains the port.
set -eu
rm -rf stage && mkdir -p stage
cp asciiquarium-bin stage/asciiquarium
cp gpl.txt README CHANGES PATCHES.md stage/
