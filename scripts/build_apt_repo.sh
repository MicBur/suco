#!/bin/bash
# Build a signed APT repository from the SUCO .deb, so users can:
#
#     curl -fsSL https://<host>/suco-archive-keyring.asc | sudo tee /etc/apt/keyrings/suco.asc >/dev/null
#     echo "deb [signed-by=/etc/apt/keyrings/suco.asc] https://<host> stable main" | sudo tee /etc/apt/sources.list.d/suco.list
#     sudo apt update && sudo apt install suco
#
# Usage:
#   scripts/build_apt_repo.sh <deb-file> <output-repo-dir> [gpg-key-id]
#
# If no key id is given, a signing key must already be the default in $GNUPGHOME.
# For a REAL public repo, generate your own key once and keep the SECRET key safe:
#   gpg --full-generate-key            # choose ed25519
#   gpg --armor --export <keyid> > suco-archive-keyring.asc   # publish this PUBLIC part
# Then host <output-repo-dir> behind any static web server (GitHub Pages, S3,
# nginx, a $5 VPS). The repo is just files — no server-side software needed.
set -euo pipefail

DEB="${1:?path to suco_*.deb}"
REPO="${2:?output repo dir}"
KEYID="${3:-}"
BASEURL="${4:-https://micbur.github.io/suco}"   # public URL the repo is served from

command -v dpkg-scanpackages >/dev/null || { echo "need dpkg-dev (apt install dpkg-dev)"; exit 2; }
command -v gpg >/dev/null || { echo "need gnupg"; exit 2; }

rm -rf "$REPO"
mkdir -p "$REPO/pool/main" "$REPO/dists/stable/main/binary-amd64"
cp "$DEB" "$REPO/pool/main/"

cd "$REPO"
dpkg-scanpackages --arch amd64 pool/ > dists/stable/main/binary-amd64/Packages
gzip -9c dists/stable/main/binary-amd64/Packages > dists/stable/main/binary-amd64/Packages.gz

# Release with checksums for every metadata file (apt verifies these).
{
    cat <<EOF
Origin: SUCO
Label: SUCO
Suite: stable
Codename: stable
Architectures: amd64
Components: main
Description: SUCO distributed C/C++ compiler
Date: $(date -Ru)
EOF
    for algo in MD5Sum:md5sum SHA256:sha256sum; do
        echo "${algo%%:*}:"
        prog="${algo##*:}"
        ( cd dists/stable
          for f in main/binary-amd64/Packages main/binary-amd64/Packages.gz; do
              printf " %s %16d %s\n" "$($prog "$f" | cut -d' ' -f1)" "$(stat -c%s "$f")" "$f"
          done )
    done
} > dists/stable/Release

KEYARG=()
[ -n "$KEYID" ] && KEYARG=(--default-key "$KEYID")
gpg "${KEYARG[@]}" -abs  -o dists/stable/Release.gpg dists/stable/Release
gpg "${KEYARG[@]}" --clearsign -o dists/stable/InRelease   dists/stable/Release
gpg "${KEYARG[@]}" --armor --export ${KEYID:+"$KEYID"} > suco-archive-keyring.asc

# One-line installer, served next to the repo so users can:
#   curl -fsSL <BASEURL>/install.sh | sudo sh
cat > "$REPO/install.sh" <<EOF
#!/bin/sh
# One-line SUCO installer — sets up the signed APT repo and installs suco.
#   curl -fsSL $BASEURL/install.sh | sudo sh
set -e
if [ "\$(id -u)" -ne 0 ]; then echo "Run with sudo: curl -fsSL $BASEURL/install.sh | sudo sh"; exit 1; fi
KEYRING=/etc/apt/keyrings/suco.asc
mkdir -p /etc/apt/keyrings
if command -v curl >/dev/null 2>&1; then curl -fsSL "$BASEURL/suco-archive-keyring.asc" -o "\$KEYRING"
elif command -v wget >/dev/null 2>&1; then wget -qO "\$KEYRING" "$BASEURL/suco-archive-keyring.asc"
else echo "need curl or wget"; exit 2; fi
echo "deb [signed-by=\$KEYRING] $BASEURL stable main" > /etc/apt/sources.list.d/suco.list
apt-get update
apt-get install -y suco
echo
echo "SUCO installed (\$(suco --version 2>/dev/null)). Nothing starts automatically — enable a role:"
echo "  sudo systemctl enable --now suco-worker                    # compile node"
echo "  sudo systemctl enable --now suco-coordinator suco-worker   # head node"
EOF
chmod +x "$REPO/install.sh"

echo "Repo built at $REPO"
echo "  dists/stable/{Release,Release.gpg,InRelease} signed"
echo "  public key: $REPO/suco-archive-keyring.asc  (publish this alongside the repo)"
echo "  one-line installer: $REPO/install.sh  (curl -fsSL $BASEURL/install.sh | sudo sh)"
echo "Host the whole $REPO directory behind any static web server."
