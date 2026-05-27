#!/bin/sh
set -eu

cc ${CFLAGS:-"-O2 -Wall -Wextra"} bpp.c -o bpp -lm
