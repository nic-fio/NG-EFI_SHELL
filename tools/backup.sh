#!/bin/sh
# Pack the whole project into one file: a git bundle with every branch, every
# tag and the complete history. It restores without a network and without
# GitHub:
#
#     tools/backup.sh ~/nesh-backup          # writes nesh-YYYY-MM-DD.bundle
#     git clone nesh-2026-09-20.bundle NEW-EFI-SHELL
#     cd NEW-EFI-SHELL && tools/setup-dev.sh --install && make test
#
# Keep a copy off this machine: a bundle on the same disk protects against a
# wrong rm, not against a dead disk.
set -eu

DEST=${1:-.}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$DEST/nesh-$(date +%F).bundle"

[ -d "$DEST" ] || mkdir -p "$DEST"
git -C "$ROOT" bundle create "$OUT" --all
git -C "$ROOT" bundle verify "$OUT" >/dev/null

if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo "warning: uncommitted changes are NOT in the bundle:"
    git -C "$ROOT" status --short
fi
if [ -n "$(git -C "$ROOT" log --oneline '@{upstream}..HEAD' 2>/dev/null)" ]; then
    echo "note: the bundle has commits that are not on the remote yet."
fi

printf '%s: %s, %s commits, tags: %s\n' "$OUT" "$(du -h "$OUT" | cut -f1)" \
    "$(git -C "$ROOT" rev-list --count HEAD)" \
    "$(git -C "$ROOT" tag | tr '\n' ' ')"
