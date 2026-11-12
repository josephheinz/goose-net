#!/bin/sh
# Builds and runs every test/c/test_*.c against the C sources. Usage: test/c/run.sh
cd "$(dirname "$0")/../.." || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
status=0
for t in test/c/test_*.c; do
    name=$(basename "$t" .c)
    echo "== $name"
    if cc -std=c11 -Wall -Wextra -Werror -pthread -I test/c "$t" src/lifecycle.c src/loop.c src/net.c -o "$tmp/$name"; then
        "$tmp/$name" || status=1
    else
        echo "FAIL $name did not compile"
        status=1
    fi
done
exit $status
