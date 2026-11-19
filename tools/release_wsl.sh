#!/usr/bin/env bash
# Build the Linux release (TotK-Setup-x86_64.AppImage) inside WSL, for tools/release.py.
#
#   release_wsl.sh <windows checkout as /mnt/... path> <commit> <game version> <out dir as /mnt/... path>
#
# The Linux release tool needs a Linux filesystem, a Linux Python with Pillow and the .NET 10 SDK. This script clones
# the commit into ~/zonith-release (replaced each run), copies in the ignored inputs the release reads from the Windows
# checkout (generated/<version>, build/pgo, refs/MacNativeGames; they stay inside WSL), installs the .NET SDK into
# ~/.dotnet when it is missing, runs tools/make_release_linux.py --pgo and copies the AppImage to <out dir>.
set -euo pipefail
src=$1 commit=$2 version=$3 out=$4
work=~/zonith-release
venv=~/zonith-release-venv

if [ ! -x ~/.dotnet/dotnet ] || ! ~/.dotnet/dotnet --list-sdks | grep -q '^10\.'; then
    curl -sSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
    bash /tmp/dotnet-install.sh --channel 10.0 --install-dir ~/.dotnet
fi
if [ ! -x $venv/bin/python ]; then
    python3 -m venv $venv
    $venv/bin/pip -q install pillow
fi
export PATH=~/.dotnet:$PATH DOTNET_ROOT=~/.dotnet

rm -rf $work
git clone -q --no-checkout "$src" $work
git -C $work fetch -q "$src" '+refs/remotes/*:refs/remotes/src/*'   # the commit is on a remote-tracking branch of $src
git -C $work checkout -q --detach "$commit"
mkdir -p $work/generated $work/build/pgo $work/refs $work/build/release
cp -r "$src/generated/$version" $work/generated/
cp "$src"/build/pgo/pgo-*.profdata "$src"/build/pgo/pgo-*.json $work/build/pgo/ 2>/dev/null || true
cp -r "$src/refs/MacNativeGames" $work/refs/
# The download cache survives the clone: keep it beside the work folder.
mkdir -p ~/zonith-release-dl $work/toolchain && ln -sfn ~/zonith-release-dl $work/toolchain/release-dl

cd $work
$venv/bin/python tools/make_release_linux.py --out build/release/TotK-Setup-linux \
    --appimage build/release/TotK-Setup-x86_64.AppImage --pgo
cp build/release/TotK-Setup-x86_64.AppImage "$out/"
