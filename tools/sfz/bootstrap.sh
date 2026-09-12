#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
sfizioso_dir="$repo_dir/deps/sfizioso"
expected_commit=86f218eb2b3409ace8b1a17aa6f7ceb15ec24fe5

git -C "$repo_dir" submodule update --init --recursive deps/sfizioso

actual_commit=$(git -C "$sfizioso_dir" rev-parse HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    echo "sfizioso pin mismatch: expected $expected_commit, got $actual_commit" >&2
    exit 1
fi

bad_submodule=$(git -C "$sfizioso_dir" submodule status --recursive | sed -n '/^[-+U]/p')
if [ -n "$bad_submodule" ]; then
    echo "sfizioso nested submodule pin mismatch:" >&2
    echo "$bad_submodule" >&2
    exit 1
fi

echo "sfizioso $actual_commit is ready (recursive pins verified)."
