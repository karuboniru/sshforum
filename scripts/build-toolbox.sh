#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
container="${TOOLBOX_CONTAINER:-fedora-toolbox-45}"

if [[ "${1:-}" == "--install" ]]; then
    toolbox run -c "$container" sudo dnf install -y \
        gcc-c++ cmake ninja-build pkgconf-pkg-config libssh-devel sqlite-devel openssl-devel python3-paramiko
elif [[ $# -gt 0 ]]; then
    echo "Usage: $0 [--install] (TOOLBOX_CONTAINER selects the container)" >&2
    exit 2
fi

toolbox run -c "$container" cmake -S "$project_dir" -B "$project_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DPython3_EXECUTABLE=/usr/bin/python3
toolbox run -c "$container" cmake --build "$project_dir/build" --parallel
toolbox run -c "$container" ctest --test-dir "$project_dir/build" --output-on-failure
