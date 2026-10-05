#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/.deps"
LOCK="$ROOT/deps.lock"
PSBC_PATCH_DIR="$ROOT/patches/opengnm-psbc"

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

psbc_patch_digest() {
    {
        printf 'base %s\n' "$OPENGNM_PSBC_SHA"
        while IFS= read -r -d '' patch; do
            printf '%s  %s\n' \
                "$(sha256sum "$patch" | awk '{print $1}')" \
                "$(basename -- "$patch")"
        done < <(find "$PSBC_PATCH_DIR" -maxdepth 1 -type f -name '*.patch' -print0 | sort -z)
    } | sha256sum | awk '{print $1}'
}

PSBC_PATCHES_CHANGED=0

apply_psbc_patches() {
    local dest="$DEPS/opengnm-psbc"
    local stamp="$dest/.shadps4-open-test-patches.sha256"
    local digest
    digest="$(psbc_patch_digest)"

    if [[ -f "$stamp" ]] && [[ "$(cat "$stamp")" == "$digest" ]]; then
        return
    fi

    # The checkout is deliberately reset before applying the patch set. This
    # makes a changed patch digest deterministic even when clone_at() kept the
    # existing checkout because HEAD still equals the pinned detached commit.
    git -C "$dest" reset -q --hard "$OPENGNM_PSBC_SHA"
    git -C "$dest" clean -q -fdx

    while IFS= read -r -d '' patch; do
        git -C "$dest" apply --check "$patch"
        git -C "$dest" apply "$patch"
    done < <(find "$PSBC_PATCH_DIR" -maxdepth 1 -type f -name '*.patch' -print0 | sort -z)

    printf '%s\n' "$digest" >"$stamp"
    PSBC_PATCHES_CHANGED=1
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

apply_psbc_patches

if [[ "$PSBC_PATCHES_CHANGED" == 1 || ! -x "$DEPS/opengnm-psbc/opengnm-psbc" ]]; then
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
