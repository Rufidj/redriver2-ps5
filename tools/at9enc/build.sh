#!/bin/sh
# builds at9enc (needs gcc and libm)
cd "$(dirname "$0")" && gcc -O2 -o at9enc at9enc.c -lm
