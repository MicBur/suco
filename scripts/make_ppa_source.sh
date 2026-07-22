#!/usr/bin/env bash
# Build a signed source package for a Launchpad PPA upload.
# Run on a Debian/Ubuntu box with: sudo apt install devscripts debhelper cmake g++ libssl-dev libzstd-dev libsqlite3-dev
#
#   scripts/make_ppa_source.sh <series> [rev]      e.g. scripts/make_ppa_source.sh noble 1
#
# Then:  dput ppa:<you>/suco ../suco_<version>~<series><rev>_source.changes
set -euo pipefail
SERIES="${1:?usage: make_ppa_source.sh <ubuntu-series> [rev]   (e.g. noble)}"
REV="${2:-1}"
VER="$(grep -oE 'VERSION [0-9.]+' CMakeLists.txt | head -1 | awk '{print $2}')"
FULL="${VER}~${SERIES}${REV}"
cat > debian/changelog <<CL
suco (${FULL}) ${SERIES}; urgency=medium

  * SUCO ${VER} for ${SERIES}.

 -- Michael Burzlaff <micbur1488@gmail.com>  $(date -R)
CL
echo "Building signed source package suco ${FULL} for ${SERIES}..."
debuild -S -sa
echo
echo "Done. Upload with:"
echo "  dput ppa:YOURNAME/suco ../suco_${FULL}_source.changes"
