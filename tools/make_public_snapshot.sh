#!/bin/bash
# make_public_snapshot.sh <frozen-ref> <out-dir> [--vpk <Galaxy-Vita.vpk>] [--date YYYY-MM-DD]     (release31)
#
# The public snapshot of a Galaxy Vita release: the tree of ONE frozen ref as ONE new commit whose single parent is the current public `main` (so Codex pushes a
# fast-forward, never a force-push). The internal history carries the owner's name and is never published. Never pushes; the only network use is the read-only
# `git fetch --depth=1` of the public main (PUBLIC_REMOTE, default https://github.com/CBOC-Vita-Lab/galaxy-vita; PUBLIC_REMOTE may be a local repo for a test).
# Codex pushes what this makes (docs/CODEX-PUSH-3.0.md).
#
#   1. refuses an existing <out-dir>/galaxy-vita; exports the tree with `git archive <ref>` (no .git, no untracked files)
#   2. fetches the public main (depth 1) into the snapshot repo: that commit is the parent
#   3. --vpk: puts the release VPK in the tree root and runs tools/update-checksums.py (the VPK's sha256 and its eboot's into cboc-release.json,
#      docs/RELEASE.json, docs/BUILD-VERIFICATION.json, SOURCE-MANIFEST.json and SHA256SUMS, as every earlier public release); the VPK is then moved
#      OUT of the tree: it is a GitHub release asset, never a committed file (earlier public trees list it in SHA256SUMS and do not contain it).
#      Without --vpk the three metadata files keep PENDING-VPK and the snapshot is NOT a release (the script says so and exits 3).
#   4. tools/public_scan.py on the whole tree (so on everything the diff from the parent adds): no private name / address from the history (counts and paths
#      only, never the values), no claude.ai or session link, no private / secrets folder, no key or .env file, no token pattern, nothing over 20 MB; home paths
#      and LAN addresses are warnings. tools/release30_gates.py art (the five contributor PNGs, approved hashes) and tree (3.0 version strings); no .vpk / .suprx /
#      disc image in the tree
#   5. `git commit-tree <tree> -p <public main>` by the public identity (the one the repo's earlier public commits use: CBOC-Vita, found in `git log --all`;
#      override with PUBLIC_NAME / PUBLIC_EMAIL), message "Galaxy Vita 3.0"; `main` points at it; an annotated tag v3.0.0 by the same identity
#   6. the checks again on what was COMMITTED: exactly one parent = the fetched public main, one new commit, our author alone, no remote configured, the
#      diff stat from the parent
# Outputs in <out-dir>: galaxy-vita/ (the repo: the parent is a shallow commit, which is fine for a fast-forward push), Galaxy-Vita.vpk (if given),
# PUBLIC-SNAPSHOT-INFO.txt (commit, parent, tree, sha256s, results).
# Exit 0 = ready to hand to Codex; 1 = a check failed (the out dir is kept for reading, not for pushing); 2 = usage / no network; 3 = no VPK.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=${REPO:-/home/claude/w/galaxy}
TAG=${TAG:-v3.0.0}
PUBLIC_REMOTE=${PUBLIC_REMOTE:-https://github.com/CBOC-Vita-Lab/galaxy-vita}; PUBLIC_BRANCH=${PUBLIC_BRANCH:-main}
REF=""; OUT=""; VPK=""; DATE=""
while [ $# -gt 0 ]; do case "$1" in
  --vpk) VPK=${2:-}; shift 2;; --date) DATE=${2:-}; shift 2;; -h|--help) sed -n 2,28p "$0"; exit 0;;
  -*) echo "unknown option $1"; exit 2;; *) if [ -z "$REF" ]; then REF=$1; elif [ -z "$OUT" ]; then OUT=$1; else echo "too many arguments"; exit 2; fi; shift;; esac; done
[ -n "$REF" ] && [ -n "$OUT" ] || { sed -n 2,4p "$0"; exit 2; }
SHA=$(git -C "$REPO" rev-parse --verify "$REF^{commit}") || { echo "unknown ref $REF"; exit 2; }
[ -z "$VPK" ] || [ -f "$VPK" ] || { echo "no such VPK: $VPK"; exit 2; }
DATE=${DATE:-$(date -u +%F)}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); SNAP=$OUT/galaxy-vita
[ ! -e "$SNAP" ] || { echo "REFUSED: $SNAP exists (a snapshot is made once; remove it yourself if you mean to redo it)"; exit 2; }
# the public identity: what the earlier public commits use (never invented)
if [ -z "${PUBLIC_NAME:-}" ] || [ -z "${PUBLIC_EMAIL:-}" ]; then
  ID=$(git -C "$REPO" log --all --author=CBOC-Vita --format='%an|%ae' | sort | uniq -c | sort -rn | head -1 | sed 's/^ *[0-9]* //')
  PUBLIC_NAME=${PUBLIC_NAME:-${ID%%|*}}; PUBLIC_EMAIL=${PUBLIC_EMAIL:-${ID#*|}}
fi
case "$PUBLIC_NAME|$PUBLIC_EMAIL" in "|"*|*"|") echo "no public identity found in the history (git log --all --author=CBOC-Vita); set PUBLIC_NAME and PUBLIC_EMAIL"; exit 2;; esac
case "$PUBLIC_EMAIL" in *@users.noreply.github.com) ;; *) echo "REFUSED: the public e-mail is not a GitHub noreply address"; exit 2;; esac
say() { echo "== snapshot: $*"; }
fail() { echo "SNAPSHOT FAILED: $*"; echo "failed $*" > "$OUT/PUBLIC-SNAPSHOT-FAILED.txt"; exit 1; }
say "ref $REF = ${SHA:0:12}; public identity $PUBLIC_NAME (the address is the repo's earlier public one)"

# 1. export
mkdir -p "$SNAP"; git -C "$REPO" archive "$SHA" | tar -x -C "$SNAP" || fail "git archive"
# 2. VPK into the checksums, then out of the tree
git -C "$SNAP" init -q -b main 2>/dev/null || { (cd "$SNAP" && git init -q && git checkout -q -b main); }   # update-checksums lists files through git
git -C "$SNAP" config user.name "$PUBLIC_NAME"; git -C "$SNAP" config user.email "$PUBLIC_EMAIL"
say "fetching the public $PUBLIC_BRANCH (read-only, depth 1) from $PUBLIC_REMOTE"
git -C "$SNAP" fetch -q --depth=1 "$PUBLIC_REMOTE" "$PUBLIC_BRANCH" 2> "$OUT/fetch.log" || { cat "$OUT/fetch.log"; fail "cannot fetch the public $PUBLIC_BRANCH: without it the snapshot has no parent and would need a force-push"; }
PARENT=$(git -C "$SNAP" rev-parse FETCH_HEAD) || fail "no FETCH_HEAD"
say "parent = $PARENT"
RELEASE=1
if [ -n "$VPK" ]; then
  cp "$VPK" "$SNAP/Galaxy-Vita.vpk"
  python3 -I "$SNAP/tools/update-checksums.py" --date "$DATE" > "$OUT/update-checksums.log" 2>&1 || fail "tools/update-checksums.py (see $OUT/update-checksums.log)"
  tail -1 "$OUT/update-checksums.log"
  mv "$SNAP/Galaxy-Vita.vpk" "$OUT/Galaxy-Vita.vpk"
  VPKSHA=$(sha256sum "$OUT/Galaxy-Vita.vpk" | cut -d' ' -f1)
  grep -q "$VPKSHA" "$SNAP/cboc-release.json" || fail "cboc-release.json does not carry the VPK's sha256"
  grep -q "PENDING-VPK" "$SNAP/cboc-release.json" "$SNAP/docs/RELEASE.json" && fail "PENDING-VPK is still in the release metadata"
else
  RELEASE=0; VPKSHA="none"; say "NO VPK given: cboc-release.json keeps PENDING-VPK; this snapshot is not a release"
fi
# 3 + 4. checks on the tree
say "checks on the exported tree"
python3 -I "$HERE/public_scan.py" --tree "$SNAP" --repo "$REPO" > "$OUT/public-scan.log" 2>&1; SCAN=$?
cat "$OUT/public-scan.log"
[ $SCAN = 0 ] || fail "public_scan.py found something that must not be public (see above and $OUT/public-scan.log)"
python3 -I "$HERE/release30_gates.py" art --root "$SNAP" || fail "the five contributor PNGs are not the approved files"
python3 -I "$HERE/release30_gates.py" tree --root "$SNAP" || fail "the tree does not carry the 3.0 version strings"
BADFILES=$(cd "$SNAP" && find . -path ./.git -prune -o -type f \( -iname '*.vpk' -o -iname '*.suprx' -o -iname '*libshacccg*' -o -iname '*.iso' -o -iname '*.rvz' -o -iname '*.wbfs' -o -iname 'hdtex.pack' \) -print)
[ -z "$BADFILES" ] || { echo "$BADFILES"; fail "a VPK, shader compiler, disc image or HD pack is in the tree"; }
# 5. one new commit on top of the public main, one tag
export GIT_AUTHOR_NAME=$PUBLIC_NAME GIT_AUTHOR_EMAIL=$PUBLIC_EMAIL GIT_COMMITTER_NAME=$PUBLIC_NAME GIT_COMMITTER_EMAIL=$PUBLIC_EMAIL
git -C "$SNAP" add -A >/dev/null 2>&1 || fail "git add"
TREE=$(git -C "$SNAP" write-tree) || fail "write-tree"
MSG=$(mktemp); printf 'Galaxy Vita 3.0\n' > "$MSG"
COMMIT=$(git -C "$SNAP" commit-tree "$TREE" -p "$PARENT" -F "$MSG") || { rm -f "$MSG"; fail "commit-tree"; }
git -C "$SNAP" update-ref refs/heads/main "$COMMIT" || fail "update-ref"
git -C "$SNAP" symbolic-ref HEAD refs/heads/main
git -C "$SNAP" reset -q || true
git -C "$SNAP" tag -a "$TAG" -F "$MSG" "$COMMIT" || { rm -f "$MSG"; fail "git tag"; }
rm -f "$MSG"
# 6. the committed result
say "checks on what was committed"
CHK=$(mktemp -d "${TMPDIR:-/tmp}/snapchk.XXXXXX"); trap 'rm -rf "$CHK"' EXIT
git -C "$SNAP" archive HEAD | tar -x -C "$CHK"
python3 -I "$HERE/public_scan.py" --tree "$CHK" --repo "$REPO" | grep -E "^(FAIL|public_scan)"
python3 -I "$HERE/public_scan.py" --tree "$CHK" --repo "$REPO" > /dev/null || fail "the committed tree fails public_scan.py"
python3 -I "$HERE/release30_gates.py" art --root "$CHK" || fail "committed artwork"
[ "$(git -C "$SNAP" rev-parse HEAD)" = "$COMMIT" ] || fail "HEAD is not the new commit"
[ "$(git -C "$SNAP" rev-list --parents -n1 HEAD | wc -w | tr -d ' ')" = 2 ] || fail "the new commit does not have exactly one parent"
[ "$(git -C "$SNAP" rev-parse HEAD^)" = "$PARENT" ] || fail "the parent is not the fetched public $PUBLIC_BRANCH"
git -C "$SNAP" merge-base --is-ancestor "$PARENT" HEAD || fail "the push would not be a fast-forward"
[ "$(git -C "$SNAP" log -1 --format='%an|%ae|%cn|%ce')" = "$PUBLIC_NAME|$PUBLIC_EMAIL|$PUBLIC_NAME|$PUBLIC_EMAIL" ] || fail "author / committer of the new commit is not the public identity alone"
[ -z "$(git -C "$SNAP" remote)" ] || fail "a remote is configured"
[ "$(git -C "$SNAP" log -1 --format=%B | grep -ci 'claude\|session\|co-authored')" = 0 ] || fail "the commit message names a tool, a session or a co-author"
[ "$(git -C "$SNAP" tag -l)" = "$TAG" ] || fail "the snapshot has tags other than $TAG"
PAUTH=$(git -C "$SNAP" log -1 --format='%an|%ae' "$PARENT")
[ "$PAUTH" = "$PUBLIC_NAME|$PUBLIC_EMAIL" ] || say "WARNING: the public main's head commit is not by the public identity (check the public history by hand before pushing)"
git -C "$SNAP" config --unset user.name 2>/dev/null; git -C "$SNAP" config --unset user.email 2>/dev/null
git -C "$SNAP" diff --stat "$PARENT" HEAD | tail -1
DIFFN=$(git -C "$SNAP" diff --name-status "$PARENT" HEAD | cut -c1 | sort | uniq -c | tr -s ' ' | tr '\n' ';')
{ echo "frozen_ref $REF"; echo "frozen_commit $SHA"; echo "public_remote $PUBLIC_REMOTE"; echo "public_branch $PUBLIC_BRANCH"; echo "parent_commit $PARENT"
  echo "snapshot_commit $COMMIT"; echo "snapshot_tree $TREE"; echo "snapshot_tag $TAG ($(git -C "$SNAP" rev-parse "$TAG"))"
  echo "author $PUBLIC_NAME <$PUBLIC_EMAIL> (the public identity of the repo's earlier public commits)"
  echo "diff_from_parent $(git -C "$SNAP" diff --stat "$PARENT" HEAD | tail -1 | sed 's/^ *//') [status counts: $DIFFN]"
  echo "files $(git -C "$SNAP" ls-files | wc -l | tr -d ' ')"
  echo "vpk Galaxy-Vita.vpk sha256 $VPKSHA"; echo "public_scan passed on the whole tree (warnings on home paths and LAN addresses are listed in public-scan.log)"
  echo "release $([ $RELEASE = 1 ] && echo yes || echo NO-VPK-PENDING)"; echo "made $(date -u +%Y-%m-%dT%H:%M:%SZ)"; } > "$OUT/PUBLIC-SNAPSHOT-INFO.txt"
cat "$OUT/PUBLIC-SNAPSHOT-INFO.txt"
[ $RELEASE = 1 ] || exit 3
exit 0
