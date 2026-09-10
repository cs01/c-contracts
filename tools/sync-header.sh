#!/bin/sh
# c_contracts.h is the annotation language, and it is shared with the clang
# fork that serves as this tool's differential oracle. One copy is the source of
# truth; this keeps the other honest.
#
#   tools/sync-header.sh            check the vendored copy matches
#   tools/sync-header.sh --update   take the fork's copy
#
# Set C_CONTRACTS_FORK to point at the llvm tree; the default is the sibling
# checkout.
set -eu

DIR=$(cd "$(dirname "$0")/.." && pwd)
FORK=${C_CONTRACTS_FORK:-$DIR/../llvm-contracts}
UPSTREAM=$FORK/clang/lib/Headers/c_contracts.h
VENDORED=$DIR/include/c_contracts.h

if [ ! -f "$UPSTREAM" ]; then
  echo "no fork at $UPSTREAM; set C_CONTRACTS_FORK" >&2
  exit 2
fi

if [ "${1:-}" = "--update" ]; then
  cp "$UPSTREAM" "$VENDORED"
  echo "updated $VENDORED"
  exit 0
fi

if diff -u "$VENDORED" "$UPSTREAM"; then
  echo "in sync"
else
  echo "diverged; run tools/sync-header.sh --update" >&2
  exit 1
fi
