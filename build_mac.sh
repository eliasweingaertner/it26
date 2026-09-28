#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$repo_dir/build"

cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DITED_SDL=ON
cmake --build "$build_dir" --target ited --config Release
cd "$repo_dir"
nohup "$build_dir/ited" >/dev/null 2>&1 &
