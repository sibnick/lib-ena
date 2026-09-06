#!/usr/bin/env bash
#
# Applies local patches to git submodules before building.
#
# Patch file convention (in this directory):
#   <submodule-name>-<base-commit-sha8>.patch
# e.g. unikraft-e31b2c44.patch was generated against .unikraft/unikraft @ e31b2c44.
#
# Idempotent: a patch already applied in the working tree is detected and
# skipped, a patch that applies cleanly is applied, anything else aborts
# with instructions.
#
# To (re)generate a patch for a submodule with local modifications:
#   git -C <submodule-path> diff > \
#     patches/<name>-$(git -C <submodule-path> rev-parse --short=8 HEAD).patch

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$ROOT/patches"

shopt -s nullglob

for p in "$PATCH_DIR"/*.patch; do
  name="$(basename "$p" .patch)"

  case "$name" in
    unikraft-*) sub=".unikraft/unikraft" ;;
    lib-lwip-*) sub=".libs/lib-lwip" ;;
    lib-ena-*)  sub=".libs/lib-ena" ;;
    *)
      echo "[patches] skipping unknown submodule '$name'"
      continue
      ;;
  esac

  dir="$ROOT/$sub"
  if [ ! -e "$dir/.git" ]; then
    echo "[patches] $sub not checked out (run 'git submodule update --init'); skipping"
    continue
  fi

  expected="${name##*-}"
  actual="$(git -C "$dir" rev-parse --short=8 HEAD 2>/dev/null || true)"
  if [ -n "$expected" ] && [ "$expected" != "$actual" ]; then
    echo "[patches] warning: $sub is at $actual, patch was generated for $expected"
  fi

  if git -C "$dir" apply --check --reverse "$p" 2>/dev/null; then
    echo "[patches] $name: already applied"
  elif git -C "$dir" apply --check "$p" 2>/dev/null; then
    git -C "$dir" apply "$p"
    echo "[patches] $name: applied"
  else
    echo "[patches] ERROR: $name does not apply to $sub at $actual" >&2
    echo "[patches] The submodule has conflicting local modifications." >&2
    echo "[patches] Discard them and retry:  git -C $sub checkout -- ." >&2
    echo "[patches] Or regenerate the patch from your modifications:" >&2
    echo "    git -C $sub diff > $p" >&2
    exit 1
  fi
done
