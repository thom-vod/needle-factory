#!/bin/sh
set -eu

root="$1"
repo="$root/repo"
rm -rf "$repo"
mkdir -p "$repo"
git -C "$repo" init -q
git -C "$repo" config user.email "test@example.com"
git -C "$repo" config user.name "needle-tests"
echo "base" > "$repo/shared.txt"
git -C "$repo" add shared.txt
git -C "$repo" commit -m "baseline" -q
