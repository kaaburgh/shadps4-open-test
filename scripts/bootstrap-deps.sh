#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/.deps"
LOCK="$ROOT/deps.lock"

# deps.lock is maintained by this repository and contains shell-compatible
# KEY=value pins only.
# shellcheck disable=SC1090
source "$LOCK"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$1" >&2
        exit 2
    }
}

for cmd in git curl tar sha256sum make clang clang++ ld.lld llvm-ar python3 glslc; do
    need "$cmd"
done

mkdir -p "$DEPS/cache"

clone_at() {
    local url="$1"
    local sha="$2"
    local dest="$3"

    if [[ -d "$dest/.git" ]] && [[ "$(git -C "$dest" rev-parse HEAD)" == "$sha" ]]; then
        return
    fi

    rm -rf -- "$dest"
    git init -q "$dest"
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch -q --depth=1 origin "$sha"
    git -C "$dest" checkout -q --detach FETCH_HEAD
}

patchset_digest() {
    local patchdir="$1"
    local patches=("$patchdir"/*.patch)

    if [[ ! -e "${patches[0]}" ]]; then
        printf 'none'
        return
    fi

    local digest rest
    read -r digest rest < <(cat -- "${patches[@]}" | sha256sum)
    printf '%s' "$digest"
}

apply_patchset() {
    local dest="$1"
    local base_sha="$2"
    local patchdir="$3"
    local stamp="$dest/.shadps4-open-test-patch-stamp"
    local digest expected
    digest="$(patchset_digest "$patchdir")"
    expected="$base_sha $digest"

    if [[ -f "$stamp" ]] && [[ "$(<"$stamp")" == "$expected" ]]; then
        printf '0'
        return
    fi

    # clone_at intentionally leaves a matching detached HEAD in place even
    # when tracked files are patched. Reset only when the patch stamp changes,
    # then rebuild from the pinned upstream tree.
    git -C "$dest" reset -q --hard "$base_sha"
    git -C "$dest" clean -q -fdx

    local patch
    for patch in "$patchdir"/*.patch; do
        [[ -e "$patch" ]] || continue
        git -C "$dest" apply --check "$patch"
        git -C "$dest" apply "$patch"
    done

    printf '%s\n' "$expected" >"$stamp"
    printf '1'
}

install_openorbis() {
    local dest="$DEPS/openorbis"
    if [[ -f "$dest/link.x" ]]; then
        return
    fi

    local archive="$DEPS/cache/$OPENORBIS_ARCHIVE"
    local url="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/$OPENORBIS_VERSION/$OPENORBIS_ARCHIVE"

    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 --output "$archive" "$url"
    fi

    printf '%s  %s\n' "$OPENORBIS_SHA256" "$archive" | sha256sum --check -

    local unpack="$DEPS/openorbis.unpack"
    rm -rf -- "$unpack" "$dest"
    mkdir -p "$unpack"
    tar -xzf "$archive" -C "$unpack"

    local link_x
    link_x="$(find "$unpack" -type f -name link.x -print -quit)"
    if [[ -z "$link_x" ]]; then
        printf 'OpenOrbis archive does not contain link.x\n' >&2
        exit 2
    fi

    local toolchain_root
    toolchain_root="$(dirname -- "$link_x")"
    if [[ "$toolchain_root" == "$unpack" ]]; then
        mv -- "$unpack" "$dest"
    else
        mv -- "$toolchain_root" "$dest"
        rm -rf -- "$unpack"
    fi
}

install_openorbis

clone_at "$OPENGNM_REPO" "$OPENGNM_SHA" "$DEPS/opengnm"
clone_at "$OPENGNM_PSBC_REPO" "$OPENGNM_PSBC_SHA" "$DEPS/opengnm-psbc"
clone_at "$SPIRV_HEADERS_REPO" "$SPIRV_HEADERS_SHA" "$DEPS/SPIRV-Headers"
clone_at "$VULKAN_HEADERS_REPO" "$VULKAN_HEADERS_SHA" "$DEPS/Vulkan-Headers"

psbc_patch_changed="$(
    apply_patchset         "$DEPS/opengnm-psbc"         "$OPENGNM_PSBC_SHA"         "$ROOT/patches/opengnm-psbc"
)"

if [[ "$psbc_patch_changed" == 1 ]] || [[ ! -x "$DEPS/opengnm-psbc/opengnm-psbc" ]]; then
    cp -- "$ROOT/tooling/opengnm-psbc-linux.mak" "$DEPS/opengnm-psbc/config.mak"
    # The upstream Makefile does not generate every Mesa codegen output it needs.
    bash "$ROOT/tooling/psbc-codegen.sh" "$DEPS/opengnm-psbc"
    # Its source lists use $(wildcard), expanded before codegen runs, so on a
    # fresh tree generated .c files would be skipped. Generate them first.
    make -C "$DEPS/opengnm-psbc" generated
    make -C "$DEPS/opengnm-psbc" -j"$(nproc)"
fi

if [[ ! -f "$DEPS/opengnm/libopengnm.a" ]]; then
    cp -- "$DEPS/opengnm/config.orbis.mak" "$DEPS/opengnm/config.mak"
    make -C "$DEPS/opengnm" -j"$(nproc)" lib \
        OO_PS4_TOOLCHAIN="$DEPS/openorbis" \
        CC=clang LD=ld.lld AR=llvm-ar
fi

cat >"$DEPS/env.sh" <<EOF
export OO_PS4_TOOLCHAIN="$DEPS/openorbis"
export OPENGNM_PATH="$DEPS/opengnm"
export OPENGNM_PSBC="$DEPS/opengnm-psbc/opengnm-psbc"
EOF

printf 'Dependencies ready under %s\n' "$DEPS"
