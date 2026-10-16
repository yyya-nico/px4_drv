#!/bin/bash

set -euo pipefail
umask 022

if (( $# > 1 )) || [[ ${1:-} == -* ]]; then
    echo "Usage: $0 [output-directory]" >&2
    exit 1
fi

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ifd_dir="$repo_dir/userland/ifd-px4"
output_dir=${1:-"$repo_dir/.."}

# dkms.conf 自体はシェルとして実行せず、共有の版番号だけを取得する。
version=$(sed -n 's/^PACKAGE_VERSION="\([0-9][0-9.]*\)"$/\1/p' "$repo_dir/dkms.conf")
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Invalid PACKAGE_VERSION in dkms.conf: $version" >&2
    exit 1
fi

for tool in dpkg-buildpackage dh gcc make; do
    if ! command -v "$tool" >/dev/null; then
        echo "Missing $tool. Install build-essential debhelper dpkg-dev libpcsclite-dev." >&2
        exit 1
    fi
done

build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
source_dir="$build_dir/px4-ifd-$version"

# 必要なソースだけを隔離し、既存の .o/.so や作業ツリーへ影響を与えずに梱包する。
mkdir -p "$source_dir/userland/ifd-px4" "$source_dir/include"
cp -- "$ifd_dir/Makefile" "$ifd_dir/ifdhandler.c" \
    "$ifd_dir/Info.plist" "$ifd_dir/README.md" "$source_dir/userland/ifd-px4/"
cp -- "$repo_dir/include/px4_card_ioctl.h" "$source_dir/include/"
cp -R -- "$ifd_dir/debian" "$source_dir/debian"

cat > "$source_dir/debian/changelog" <<EOF
px4-ifd ($version) unstable; urgency=medium

  * Build the PX4 PC/SC IFD handler from px4_drv $version.

 -- yyya-nico <yyya-nico@users.noreply.github.com>  $(LC_ALL=C date -u -R)
EOF

(
    cd -- "$source_dir"
    dpkg-buildpackage -b -us -uc
)

mkdir -p -- "$output_dir"
cp -- "$build_dir"/*.deb "$output_dir/"
printf 'Debian package created in: %s\n' "$(cd -- "$output_dir" && pwd)"
