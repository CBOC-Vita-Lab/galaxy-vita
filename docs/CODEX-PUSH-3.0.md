# Codex: publish Galaxy Vita 3.0 (one prompt, one push)

For Codex. We prepared everything; **you push, we never do.** Do the checks first, in order, and stop at the first one that fails. Do not improvise: no rebase, no amend, no force-push, no extra commit, no change of any file in the snapshot.

## What you are given (in the folder `<SNAPSHOT_OUT>`, made by `tools/make_public_snapshot.sh`)

| File | What |
|---|---|
| `galaxy-vita/` | a repository with ONE new commit on branch `main`, made by `CBOC-Vita`, whose single parent is the public `main` as it was when we built the snapshot (`parent_commit` in the info file; the parent is a shallow commit, that is fine). Annotated tag `v3.0.0` on the new commit. No remote is configured. |
| `Galaxy-Vita.vpk` | the release package. A GitHub release asset only; never commit it. |
| `PUBLIC-SNAPSHOT-INFO.txt` | parent, new commit, tree, tag, VPK sha256, diff size from the parent, what was checked. |
| `RELEASE-INFO.txt` (from the build folder) | the build's ref, eboot and VPK sha256, build id `3.0.0`, gates passed. |

## Where it goes

- Remote: **`https://github.com/CBOC-Vita-Lab/galaxy-vita.git`**, branch **`main`** (confirmed read-only on 18 Nov: `main` was at `81bb9a45` = tag `v2.2.1`; tags `v2.0`, `v2.1`, `v2.1.1`, `v2.2`, `v2.2.1`). `parent_commit` in the info file is what `main` was when the snapshot was built; if the remote has moved since, the snapshot is stale: stop.
- The push is a **fast-forward**: `main` gets one new commit on top of the commit it already has. **Never `--force`, never `--force-with-lease`, no rebase, no amend.** If a push is refused, stop and report.
- Tag `v3.0.0`. GitHub release "Super Mario Galaxy for PS Vita 3.0", tag `v3.0.0`, asset `Galaxy-Vita.vpk`. The release notes are the 3.0 section of `CHANGELOG.md` (copy it; say only what it says).
- Identity: `CBOC-Vita <344212538+CBOC-Vita@users.noreply.github.com>` for the commit and the tag (already made; do not make new ones). Push with the public account. If GitHub would show another account, stop and ask.

## Checks before you push (all must pass; paste each result into your report)

1. `sha256sum Galaxy-Vita.vpk` equals `vpk sha256` in `PUBLIC-SNAPSHOT-INFO.txt`, equals `vpk_sha256` in `RELEASE-INFO.txt`, and equals `package.sha256` in `galaxy-vita/cboc-release.json` (which must not say `PENDING-VPK`).
2. The remote `main` is still the parent: `git ls-remote https://github.com/CBOC-Vita-Lab/galaxy-vita.git refs/heads/main` prints `parent_commit` from the info file. **If not, STOP.**
3. In `galaxy-vita/`: `git rev-parse HEAD` equals `snapshot_commit`; `git rev-list --parents -n1 HEAD` prints exactly two hashes (the commit and ONE parent), the parent being `parent_commit`; `git log -1 --format='%an <%ae> | %cn <%ce>'` is `CBOC-Vita <344212538+CBOC-Vita@users.noreply.github.com>` twice; `git log -1 --format=%B` is `Galaxy Vita 3.0` with no co-author line and no link; `git remote` prints nothing; `git status --short` prints nothing; `git tag -l` prints `v3.0.0` only.
4. Fast-forward: `git remote add origin https://github.com/CBOC-Vita-Lab/galaxy-vita.git`, `git fetch --depth=1 origin main`, then `git merge-base --is-ancestor FETCH_HEAD HEAD` exits 0 (the remote main is an ancestor of the new commit). Exit 1: STOP.
5. The diff from the public `main` (tag `v2.2.1`) is what the push adds: `git diff --stat FETCH_HEAD HEAD | tail -1` equals `diff_from_parent` in the info file. Scan the whole new tree (which contains everything the diff adds): in the snapshot's tree run `python3 -I tools/public_scan.py --tree . --repo <an internal clone, if you have one>`; the snapshot script already ran it with the internal history and printed `passed`. Without the internal history, run these in the tree and report the counts, which must all be 0: links to the Claude web app and session ids (`grep -rIlE 'claude\.ai/(code|chat|artifact)|session_01[A-Za-z0-9]{16}' .`), private key blocks (`grep -rIl 'BEGIN .*PRIVATE KEY' .`), files named `*.pem *.p12 *.key .env`, any `*.vpk *.suprx *libshacccg* *.iso *.rvz *.wbfs`, and the owner's name and addresses as the owner gives them to you in the same message as this prompt (not written here). Also `git log --format='%an <%ae>' FETCH_HEAD..HEAD | sort -u` is the public identity only.
6. The five approved pictures are untouched (never regenerate or replace them): in `vita/vgx-game/sce_sys/`, sha256 of
   - `icon0.png` `7fa63a19c822bff68d23aacada5b7e1fc97d698364a1145b4159c72ec82a7b2a`
   - `pic0.png` `59668c9c65aa3b6550bb4ecee07ab3158f74bf8e28070c9a14da07569b681096`
   - `livearea/contents/bg.png` `3d57068135279416adc97b7fe99e2b96fa7d26fdbeb2d493ba63210c0343fa3a`
   - `livearea/contents/startup.png` `d1f6e806f1f11f0613abd3ae65dad066eccbde148a8bf47e3ea6f0c91f1bd2c2`
   - `livearea/contents/settings.png` `3ed8b34c9e5dcee4df2dcd75728641c577b12fa2affe0e6ee3eca1c7451796ed`
7. VPK members: `unzip -l Galaxy-Vita.vpk` lists only `eboot.bin`, `sce_sys/param.sfo`, `sce_sys/icon0.png`, `sce_sys/pic0.png`, `sce_sys/livearea/contents/{bg.png,startup.png,template.xml}`, `shadercache/vgx_shaderlist.bin`, `shadercache/vgx_stagewarm.bin`, `shadercache/<8 hex>/stages.pack`, `shadercache/<8 hex>/v3/seed.c182` (about 13 entries). Anything else: stop.
8. `RELEASE-INFO.txt` says `verdict PASSED`, `build_id 3.0.0`, `gates_skipped none`. Anything else: stop.

## Push (only after every check above passed)

```
cd galaxy-vita
git push origin main            # fast-forward; a rejection means STOP, never force
git push origin v3.0.0
gh release create v3.0.0 ../Galaxy-Vita.vpk --repo CBOC-Vita-Lab/galaxy-vita --title "Super Mario Galaxy for PS Vita 3.0" --notes-file <the 3.0 section of CHANGELOG.md>
```

After it: `git ls-remote https://github.com/CBOC-Vita-Lab/galaxy-vita.git refs/heads/main refs/tags/v3.0.0` shows `snapshot_commit` (and the tag); the earlier tags are unchanged; download the release asset and compare its sha256 with step 1; open the release page and confirm exactly one asset. Report all of it. Do not announce anything and do not touch the online service in this step.

## Not part of this push

The online service package (`smg-online-server-20261118a.tgz`) is a separate Codex step (`docs/RELEASE-3.0-CHECKLIST.md` section 3). Deploy it before the release is made public; do not run it from here.
