#!/bin/sh
# Run this to generate all the initial makefiles, etc.
set -e

srcdir=$(dirname "$0")
test -z "$srcdir" && srcdir=.

olddir=$(pwd)
cd "$srcdir"

mkdir -p m4 build-aux
autoreconf --force --install --verbose

cd "$olddir"
if test -z "$NOCONFIGURE"; then
  exec "$srcdir/configure" "$@"
fi
