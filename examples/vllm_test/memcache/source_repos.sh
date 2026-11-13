#!/usr/bin/env bash

readonly MEMCACHE_REPO="https://gitcode.com/GDD_ESCC/memcache.git"
readonly MEMCACHE_BRANCH="trunk_for_escc_v1.2.1"
readonly MEMFABRIC_REPO="https://gitcode.com/GDD_ESCC/memfabric_hybrid.git"
readonly MEMFABRIC_BRANCH="trunk_for_escc_v1.2.0"
readonly VLLM_ASCEND_REPO="https://gitcode.com/GDD_ESCC/vllm-ascend.git"
readonly VLLM_ASCEND_BRANCH="trunk_for_escc_v24.0"

sync_source_repo() {
    local directory="$1" repository="$2" branch="$3"
    if [[ ! -e "$directory/.git" ]]; then
        if [[ -d "$directory" && -n "$(ls -A "$directory")" ]]; then
            echo "[ERROR] Repository directory is not empty: $directory" >&2
            return 1
        fi
        git clone --branch "$branch" "$repository" "$directory" || return 1
    fi
    if [[ -n "$(git -C "$directory" status --porcelain --untracked-files=no --ignore-submodules=all)" ]]; then
        echo "[ERROR] Tracked source files have uncommitted changes: $directory" >&2
        return 1
    fi
    git -C "$directory" fetch "$repository" "$branch" || return 1
    local upstream
    upstream=$(git -C "$directory" rev-parse FETCH_HEAD) || return 1
    if git -C "$directory" show-ref --verify --quiet "refs/heads/$branch"; then
        git -C "$directory" checkout "$branch" || return 1
    else
        git -C "$directory" checkout -b "$branch" "$upstream" || return 1
    fi
    git -C "$directory" merge --ff-only "$upstream" || {
        echo "[ERROR] Cannot fast-forward $directory branch=$branch upstream=$upstream" >&2
        return 1
    }
    echo "[SOURCE] $directory branch=$branch commit=$(git -C "$directory" rev-parse HEAD)"
}
