#!/usr/bin/env bash
# menuback32: Back on the file select's heads returns to the entry screen (Single Player / Multiplayer / Settings).
# 1. source wiring of the FileSelector side (the nerve, the wipe, the camera, the prompt, the title's unchanged first step);
# 2. the entry state machine's host tests for the flow (tests/mp/ui_menu/headsback_test.cpp, run focused: --heads).
# The animation itself (wipe, camera, music) is proven in the emulator: see the report's pad script.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
fs="$root/decomp/src/Game/Map/FileSelector.cpp"
fail() { echo "FAIL: $*" >&2; exit 1; }
body() { awk -v n="$1" '$0 ~ "^void FileSelector::"n"\\(\\) \\{" {p=1} p {print} p && /^}/ {exit}' "$fs"; }

sel=$(body exeFileSelect); back=$(body exeBackToEntry); title=$(body exeTitle)
[ -n "$sel" ] && [ -n "$back" ] && [ -n "$title" ] || fail "a FileSelector function was not found"

# the heads: Back is offered only where the entry screen exists; the game's own back check (B + the round button); never on the first step
grep -q 'MpEntry::headsBackAllowed()' <<<"$sel" || fail "exeFileSelect does not ask MpEntry::headsBackAllowed"
grep -q 'checkSelectedBackButton()' <<<"$sel" || fail "exeFileSelect does not use the game's own back check (B / round Back button)"
grep -q 'FileSelectorNrvBackToEntry' <<<"$sel" || fail "exeFileSelect does not go to the BackToEntry nerve"
grep -q '!MR::isFirstStep(this)' <<<"$sel" || fail "the back check can fire on the nerve's first step"
grep -q 'mBackButton->appear()' <<<"$sel" || fail "the heads show no Back prompt"
grep -q 'NEW_NERVE(FileSelectorNrvBackToEntry, FileSelector, BackToEntry)' "$fs" || fail "nerve not declared"
grep -q 'void exeBackToEntry();' "$root/decomp/include/Game/Map/FileSelector.hpp" || fail "exeBackToEntry not in the header"

# the nerve: the game's wipe (not a cut), the camera back, the entry screen next, and nothing opened or saved
grep -q 'MR::closeWipeFade' <<<"$back" || fail "no closing wipe"
grep -q 'MR::isWipeBlank()' <<<"$back" || fail "the switch does not wait for the closed wipe"
grep -q 'MR::openWipeFade' <<<"$back" || fail "no opening wipe"
grep -q 'goToTitle()' <<<"$back" || fail "the camera is not put back"
grep -q 'MpEntry::headsBack()' <<<"$back" || fail "the entry screen is not asked for"
grep -q 'FileSelectorNrvTitle' <<<"$back" || fail "does not return through the title nerve"
if grep -Eq 'GameSequenceFunction|restoreUserFile|startGameDataLoadSequence|FileSelectorNrv(Demo|Create|FileConfirm)|sMpLaunchFile' <<<"$back"; then
  fail "exeBackToEntry opens or writes something"
fi
# the openWipe must come after the closed-wipe test
awk '/isWipeBlank/ {a=NR} /openWipeFade/ {b=NR} END {exit !(a && b && a < b)}' <<<"$back" || fail "openWipeFade before the wipe is blank"

# rev32 F1: nothing is picked after Back (a file pointed before it must not name the next session's player)
grep -q '_B4 = nullptr;' <<<"$back" && grep -q '_B8 = nullptr;' <<<"$back" || fail "exeBackToEntry does not clear the pointed file (_B4 / _B8)"
# rev32 F6: the camera's fovy is the title's again; F2: the pill pointer is dropped on the game thread at the destroy
grep -q 'mFovy = ::cTitleFovy' "$root/decomp/src/Game/Map/FileSelectCameraController.cpp" || fail "goToTitle does not reset the fovy"
grep -q 'portPostGuidanceKeep(nullptr);  // rev32 F2' "$root/decomp/src/Game/System/GameSystemSceneController.cpp" || fail "exeDestroyScene does not drop the pill pointer"

# players who come straight from the title: the title's first step is as before
grep -q 'if (!MpEntry::skipTitle()) mTitle->appear();' <<<"$title" || fail "exeTitle's first step changed"
grep -q 'case MpEntry::STEP_TITLE:' "$fs" || fail "STEP_TITLE (entry screen's B) is gone"

# every other back of the file select is untouched: the checks still stand
n=$(grep -c 'checkSelectedBackButton()' "$fs")
[ "$n" -ge 5 ] || fail "a back check of the original sub-screens is gone ($n)"

echo "PASS: FileSelector wiring"
out=$(mktemp)
trap 'rm -f "$out"' EXIT
ASAN_OPTIONS=detect_leaks=0 bash "$root/tests/mp/ui_menu/build.sh" "$out" --heads 2>&1 | tail -3 | tee /dev/stderr | grep -q ' 0 failures' || fail "ui_menu --heads"
echo "PASS: heads back (flow)"
