#!/bin/bash
# Usage: tools/worktree.sh add <name> [--clean] | land <name> [--apply] | list | remove <name> [--force]
# One git worktree per work segment (A1 S1-S4, ...): wt/<name> on branch seg/<name>, with its own build/ seeded from the
# main tree's (tools/newbuild), so a segment's half-finished edit to a shared tool or source file can no longer break
# another segment's build, and nobody's exports re-run because someone else saved a file.
#   add      new worktree at the main tree's current state: HEAD plus its uncommitted changes and untracked files
#            (--clean: HEAD only). Git-ignored helpers the build needs (third-party clones, venvs, config.local.mk)
#            are linked / copied from the main tree. Shared with the main tree: the job queue's RAM budget and
#            notes/jobq.log (tools/q), the simplify cache (tools/simplify_pool.py), memwatch's job detection.
#   land     the worktree's changes since add (or the last land) as a patch, wt/<name>.patch; --apply applies it to
#            the main tree's working files (nothing is committed)
#   list     worktrees with their branch and last commit
#   remove   delete the worktree (refuses while it has changes not landed, unless --force; the branch stays)
# A worktree's build is under wt/<name>/build: run make / tests from inside wt/<name> as usual. When a segment's tests
# pass there, `land --apply` and rebuild the main tree.
set -e
MAIN=$(git -C "$(dirname "$0")/.." rev-parse --path-format=absolute --git-common-dir); MAIN=${MAIN%/.git}
cmd=$1; name=$2
# git-ignored things the build needs, linked from the main tree (dir-only ignore patterns don't match these symlinks,
# so snapshots leave them out by name)
LINKS="ext tools/ext tools/vgmstream tools/liq tools/lz4hc/build data/ref .venv .venv-tools"
snapshot() {  # commit of a tree's working files (tracked + untracked, not ignored), without touching its index
    local tmp; tmp=$(mktemp)
    GIT_INDEX_FILE=$tmp git -C "$1" read-tree HEAD &&
        GIT_INDEX_FILE=$tmp git -C "$1" add -A -- . $(for l in $LINKS; do [ -L "$1/$l" ] && echo ":(exclude)$l"; done) &&
        git -C "$1" commit-tree "$(GIT_INDEX_FILE=$tmp git -C "$1" write-tree)" -p HEAD -m "$2"
    rm -f "$tmp"
}
case $cmd in
    add)
        [ -n "$name" ] || { sed -n '2p' "$0"; exit 2; }
        WT=$MAIN/wt/$name
        [ ! -e "$WT" ] || { echo "worktree.sh: $WT exists" >&2; exit 1; }
        grep -qx '/wt/' "$MAIN/.gitignore" || echo '/wt/' >> "$MAIN/.gitignore"
        git -C "$MAIN" worktree add -q -b "seg/$name" "$WT" HEAD
        if [ "$3" != --clean ]; then
            # the main tree as it is (HEAD + uncommitted changes + untracked files) as uncommitted changes here;
            # that snapshot (refs/wt-base/<name>) is what `land` diffs against
            S=$(snapshot "$MAIN" "main tree for $name")
            git -C "$WT" checkout -q "$S" -- . && git -C "$WT" reset -q
        else
            S=$(git -C "$MAIN" rev-parse HEAD)
        fi
        git -C "$MAIN" update-ref "refs/wt-base/$name" "$S"
        # link the big ignored helpers, copy the per-machine config
        for p in $LINKS; do
            [ -e "$MAIN/$p" ] && [ ! -e "$WT/$p" ] && { mkdir -p "$(dirname "$WT/$p")"; ln -s "$MAIN/$p" "$WT/$p"; }
        done
        [ -e "$MAIN/config.local.mk" ] && cp -a "$MAIN/config.local.mk" "$WT/"
        for so in "$MAIN"/tools/*/*.so "$MAIN"/tools/*/*/*.so; do  # built native libraries (ignored *.so)
            [ -e "$so" ] || continue
            rel=${so#$MAIN/}; [ -e "$WT/$rel" ] || cp -a "$so" "$WT/$rel" 2>/dev/null || true
        done
        if [ -d "$MAIN/build/gen" ]; then
            bash "$MAIN/tools/newbuild" "$WT/build" --from "$MAIN/build"
        fi
        echo "worktree $WT on seg/$name"
        echo "build there: cd $WT && tools/q -m 12 bash -c 'source ps2/env.sh && make -C ps2/proto'"
        ;;
    land)
        [ -n "$name" ] || { sed -n '2p' "$0"; exit 2; }
        WT=$MAIN/wt/$name
        B=$(git -C "$MAIN" rev-parse -q --verify "refs/wt-base/$name") || { echo "no base for $name" >&2; exit 1; }
        T=$(snapshot "$WT" "$name now")
        P=$MAIN/wt/$name.patch
        git -C "$MAIN" diff --binary "$B" "$T" > "$P"
        echo "$(grep -c '^diff --git' "$P") files changed in $name since it was made: $P"
        git -C "$MAIN" diff --stat "$B" "$T" | tail -1
        if [ "$3" = --apply ]; then
            # onto the main tree's working files (no commit); fails without changing anything if the main tree moved
            # under the same lines since
            git -C "$MAIN" apply --whitespace=nowarn "$P" && echo "applied to $MAIN (uncommitted); rebuild there" &&
                git -C "$MAIN" update-ref "refs/wt-base/$name" "$T"  # the next land carries only what follows
        else
            echo "apply: tools/worktree.sh land $name --apply"
        fi
        ;;
    list)
        git -C "$MAIN" worktree list
        ;;
    remove)
        [ -n "$name" ] || { sed -n '2p' "$0"; exit 2; }
        # its files always differ from its branch (it starts with the main tree's uncommitted state): removable once
        # everything is landed (no change since the last add / land), or with --force
        B=$(git -C "$MAIN" rev-parse -q --verify "refs/wt-base/$name^{tree}" || true)
        T=$(snapshot "$MAIN/wt/$name" "$name now")
        if [ "$3" != --force ] && [ "$(git -C "$MAIN" rev-parse "$T^{tree}")" != "$B" ]; then
            echo "worktree.sh: wt/$name has changes not landed (tools/worktree.sh land $name), or use --force" >&2
            exit 1
        fi
        git -C "$MAIN" worktree remove --force "$MAIN/wt/$name"
        git -C "$MAIN" update-ref -d "refs/wt-base/$name"
        echo "removed wt/$name (branch seg/$name kept: git branch -D seg/$name to drop it)"
        ;;
    *)
        sed -n '2,19p' "$0"; exit 2 ;;
esac
