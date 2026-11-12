#!/bin/sh
# Builds src/loop.c against a stub goose.h and runs the spool-mode tests.
set -e
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -pthread -Itest/stub -o "$out/spool_test" test/spool_test.c src/loop.c src/lifecycle.c src/net.c
"$out/spool_test"
