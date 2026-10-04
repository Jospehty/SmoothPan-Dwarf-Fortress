#!/usr/bin/env bash
# Download gcc15 + gcc15-libs Arch packages and extract them into a local
# prefix. No root, no system modification.
set -uo pipefail
PREFIX="$HOME/.cache/smoothpan-toolchain"
mkdir -p "$PREFIX"
cd "$PREFIX" || exit 1

BASE="https://geo.mirror.pkgbuild.com/extra/os/x86_64"
VER="15.3.0%2Br0.g4db0e8df15be-2"

for name in "gcc15-libs-${VER}" "gcc15-${VER}"; do
    out="$(printf '%s' "$name" | sed 's/%2B/+/')-x86_64.pkg.tar.zst"
    url="$BASE/${name}-x86_64.pkg.tar.zst"
    if [ ! -s "$out" ]; then
        echo "downloading $out"
        curl -fsSL --retry 3 -o "$out" "$url" || { echo "DOWNLOAD FAILED: $url"; exit 1; }
    fi
    if ! file "$out" | grep -qi zstandard; then
        echo "NOT A ZSTD PACKAGE: $out"; file "$out"; exit 1
    fi
    echo "extracting $out"
    tar --use-compress-program=unzstd -xf "$out" -C "$PREFIX" || { echo "EXTRACT FAILED: $out"; exit 1; }
done

echo "--- result ---"
ls "$PREFIX/usr/bin" 2>/dev/null | grep -E 'gcc|g\+\+' | head
"$PREFIX/usr/bin/g++-15" --version 2>&1 | head -1
