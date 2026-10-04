#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
container="${TOOLBOX_CONTAINER:-fedora-toolbox-45}"
case "${1:-}" in
    --install)
        if [[ $# -ne 1 ]]; then
            echo "Usage: $0 [--install] (TOOLBOX_CONTAINER selects the container)" >&2
            exit 2
        fi
        toolbox run -c "$container" sudo dnf install -y \
            rpm-build systemd-rpm-macros cmake-rpm-macros
        ;;
    '')
        if [[ $# -ne 0 ]]; then
            echo "Usage: $0 [--install] (TOOLBOX_CONTAINER selects the container)" >&2
            exit 2
        fi
        ;;
    *)
        echo "Usage: $0 [--install] (TOOLBOX_CONTAINER selects the container)" >&2
        exit 2
        ;;
esac

# Keep this list explicit: local databases, host keys, build output, and VCS
# metadata must never become part of a distributable source archive.
sources=(
    CMakeLists.txt
    LICENSE
    README.md
    docs/packaging.md
    include/sshforum/server.hpp
    include/sshforum/store.hpp
    include/sshforum/task.hpp
    include/sshforum/tui.hpp
    packaging/rpm/sshforum.spec
    packaging/systemd/sshforum.service
    scripts/build-srpm.sh
    scripts/build-toolbox.sh
    scripts/test-systemd.sh
    src/main.cpp
    src/server.cpp
    src/store.cpp
    src/tui.cpp
    tests/ssh_smoke.py
    tests/store_test.cpp
    tests/tui_test.cpp
)

for source in "${sources[@]}"; do
    path="$project_dir"
    remainder="$source"
    while [[ "$remainder" == */* ]]; do
        path+="/${remainder%%/*}"
        if [[ -L "$path" ]]; then
            echo "Source path contains a symlink: $source" >&2
            exit 1
        fi
        remainder="${remainder#*/}"
    done
    if [[ ! -f "$project_dir/$source" || -L "$project_dir/$source" ]]; then
        echo "Required regular source file is missing: $source" >&2
        exit 1
    fi
done

version="$(sed -nE 's/^Version:[[:space:]]*([0-9]+(\.[0-9]+){2})[[:space:]]*$/\1/p' \
    "$project_dir/packaging/rpm/sshforum.spec")"
cmake_version="$(sed -nE 's/^project\(sshforum VERSION ([0-9]+(\.[0-9]+){2}) LANGUAGES CXX\)$/\1/p' \
    "$project_dir/CMakeLists.txt")"
if [[ -z "$version" || "$version" != "$cmake_version" ]]; then
    echo "Version mismatch or unsupported format in spec and CMakeLists.txt" >&2
    exit 1
fi
package="sshforum-${version}"
topdir="$project_dir/dist/rpmbuild"

epoch="${SOURCE_DATE_EPOCH:-0}"
if [[ ! "$epoch" =~ ^[0-9]+$ ]]; then
    echo "SOURCE_DATE_EPOCH must be a nonnegative integer" >&2
    exit 2
fi

mkdir -p -- "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS"
tar --create --gzip --file "$topdir/SOURCES/$package.tar.gz" \
    --directory "$project_dir" --sort=name --mtime="@$epoch" \
    --owner=0 --group=0 --numeric-owner \
    --transform="s,^,$package/," -- "${sources[@]}"
cp -- "$project_dir/packaging/rpm/sshforum.spec" "$topdir/SPECS/sshforum.spec"

toolbox run -c "$container" rpmbuild -bs \
    --define "_topdir $topdir" "$topdir/SPECS/sshforum.spec"
echo "SRPM output: $topdir/SRPMS"
