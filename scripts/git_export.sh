#!/usr/bin/env bash
#
# Exports Fossil history to the Git mirror, then leaves that mirror clean.
#
# 'fossil git export' leaves two pieces of state behind:
#   1. It feeds the mirror with git fast-import. The index lags HEAD, so a plain
#      'git status' in the mirror reports staged edits and deletions.
#   2. The autopush runs 'git push --mirror <url>'. That takes a URL, not a
#      remote name, so refs/remotes/origin/* stay behind and 'git branch -vv'
#      shows 'main [ahead N]'.
#
# This script runs the export and then fixes both. It prints the mirror status at
# the end. Pass extra arguments through to 'fossil git export'.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ ! -e .fslckout ]; then
  echo "[git-export] ERROR: no Fossil checkout in $ROOT" >&2
  exit 1
fi

FOSSIL="fossil"
if [ -x ./fossil ]; then
  FOSSIL="./fossil"
fi

echo "[git-export] fossil sync"
"$FOSSIL" sync

echo "[git-export] fossil git export"
"$FOSSIL" git export "$@"

# Match the index to the exported tip. This does not touch the working tree.
git reset -q

# Update refs/remotes/origin/*, which the autopush leaves behind.
if git remote get-url origin >/dev/null 2>&1; then
  git fetch -q origin
fi

echo "[git-export] mirror status"
"$FOSSIL" git status
