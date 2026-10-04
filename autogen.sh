#!/bin/sh
# script to prepare PearPC sources
# Bootstrap always runs in the source tree, even when invoked from elsewhere.
CDPATH= cd "$(dirname "$0")" || exit 1
aclocal -I . \
&& autoheader \
&& automake --add-missing \
&& autoconf \
|| exit 1

echo PearPC sources are now prepared. To build here, run:
echo " ./configure"
echo " make"

echo "For a separate build directory, see README.md (Shadow builds)."
