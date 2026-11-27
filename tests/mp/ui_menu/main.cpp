// The menus' host tests: the runner (flow split). Build: see tests/mp/ui_menu/build.sh.
#include "common.hpp"

void testFlow();
void testEntry();
void testHeadsBack();   // menuback32
void testConnTest();
void testSettingsWords();   // native Settings: banned words (conntest_test.cpp)
void testConnTestWords();   // m-conntest: banned words, every screen, five languages
void testJoinLeaveSounds();
void testRulesCard();
void testPickToast();
void testChatMenu();
void testByAddress();
void testCodeJoinFixes();
void testInviteCode();
void testHonestEndings();
void testAnyoneDoesNotStrand();
void testGatherStartReasons();   // mpwifi44
void testGatherStarCache();
void testMessagesTapTwice();
void testMeMark();
void testWaitLook();
void testRaceNeverAsksFile();
void testNoDroppedHints();
void testReconnect();
void testJoinErrorsShown();
void testServiceErrors();
void testRoomIdentity();
void testRootRetry();
void testQuickMatchSave();
void testStartReopen();
void testMessages();
void testResultsCodec();
void testReachAndBack();
void testRequests();
void testFriendsSelfRefresh();
void testFriendsSignIn();
void testFriendsDuplicateNames();
void testFriendsCodeEntry();
void testDeleteAccount();
void testFriendsPage();
void testPlayedWith();
void testFriendsLimits();
void testFriendsBack();
void testFriendsCompose();
void testErrors();
void testSession();
void testCompose();
void testTextStrip();
void testStarCard();
void testStripCompose();
void testFalls();
void testSessionMenu();
void testSessionWarp();
void testSessionPlayers();
void testSessionFriends();
void testSessionMessages();
void testSessionWords();
void testSessionLook();
void testFilesUnreadable();
void testFilesSinglePlayerWrite();
void testFilesSourceGate();
void testFilesAfterSession();
void testFilesBackPaths();
void testKeepScreens();
void testKeepEntry();
void testPlanCapacity();   // memfix (plans_test.cpp)
void testRaceLeave();
void testLobbyIsGathering();
void testGatherThreeButtons();
void testHeardAndHints();
void testSafeCopiesReach();   // menus4 (reach_test.cpp)
void testSafeCopiesWords();
void testWordsGameVoice();
void testSettingsDescriptions();   // native Settings: every pill, value and move has a description (words_test.cpp)
void testSettingsVoice();    // native Settings (words_test.cpp)
void testSettingsBoxes();    // native Settings (boxfit_test.cpp)
void testWordsRealBoxes();   // menus5 (boxfit_test.cpp)
void testSettingsScreens();   // settingsnative (settings_test.cpp)
void testSettingsValues();
void testSettingsAvailability();
void testSettingsLeave();
void testSettingsSession();
void testSettingsButtons();
void testSettingsRare();
void testEntrySettings();
void testSettingsAssist();   // setnat31: the 60 Assist row (settings_test.cpp)
void testMonkey();
void testScriptedChecks();
void testRematchAskByHand();
void testReport();   // beta diagnostics (report_test.cpp)
void testFirstTimeFlowPresses();   // menus4 (words_test.cpp)
void testHarvestTour();   // harvprep31 (harvest_test.cpp)

int main(int argc, char** argv) {
    setenv("GXV_REMAP", "1", 1);   // settingsnative: the button list exists (RemapEnabled reads this once)
    MpPanel::resetPlanPeaks();   // memfix
    if (argc > 1 && !strcmp(argv[1], "--settings")) {   // a focused run: the native Settings' own tests and the geometry / words checks over its screens (settings_test.cpp, the kPaths)
        testSettingsScreens();
        testSettingsValues();
        testSettingsAvailability();
        testSettingsLeave();
        testSettingsSession();
        testSettingsButtons();
        testSettingsRare();
        testEntrySettings();
        testSettingsAssist();
        testHarvestTour();
        testSettingsWords();
        testSettingsVoice();
        testSettingsDescriptions();
        testSettingsBoxes();
        fprintf(stderr, "%d checks, %d failures (settings only)\n", gChecks, gFails);
        return gFails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--heads")) {   // menuback32: a focused run: the entry screen's tests, the heads' Back among them
        testEntry();
        testHeadsBack();
        testEntrySettings();
        fprintf(stderr, "%d checks, %d failures (entry only)\n", gChecks, gFails);
        return gFails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--words")) {   // (a focused run: only the words, the boxes and the banned words; the full suite with its monkeys runs in the final pass)
        testConnTestWords();
        testSettingsWords();
        testWordsGameVoice();
        testWordsRealBoxes();
        testSettingsVoice();
        testSettingsDescriptions();
        testSettingsBoxes();
        fprintf(stderr, "%d checks, %d failures (words only)\n", gChecks, gFails);
        return gFails ? 1 : 0;
    }
    testFlow();
    testEntry();
    testHeadsBack();
    testConnTest();
    testConnTestWords();
    testSettingsWords();
    testJoinLeaveSounds();
    testRulesCard();
    testPickToast();
    testChatMenu();
    testByAddress();
    testCodeJoinFixes();
    testInviteCode();
    testHonestEndings();
    testAnyoneDoesNotStrand();
    testGatherStartReasons();
    testGatherStarCache();
    testMessagesTapTwice();
    testMeMark();
    testWaitLook();
    testRaceNeverAsksFile();
    testNoDroppedHints();
    testReconnect();
    testJoinErrorsShown();
    testServiceErrors();
    testRoomIdentity();
    testRootRetry();
    testQuickMatchSave();
    testStartReopen();
    testMessages();
    testResultsCodec();
    testReachAndBack();
    testRequests();
    testFriendsSelfRefresh();
    testFriendsSignIn();
    testFriendsDuplicateNames();
    testFriendsCodeEntry();
    testDeleteAccount();
    testFriendsPage();
    testPlayedWith();
    testFriendsLimits();
    testFriendsBack();
    testFriendsCompose();
    testErrors();
    testSession();
    testCompose();
    testTextStrip();
    testStarCard();
    testStripCompose();
    testFalls();
    testSessionMenu();
    testSessionWarp();
    testSessionPlayers();
    testSessionFriends();
    testSessionMessages();
    testSessionWords();
    testSessionLook();
    testFilesUnreadable();
    testFilesSinglePlayerWrite();
    testFilesSourceGate();
    testFilesAfterSession();
    testFilesBackPaths();
    testKeepScreens();
    testKeepEntry();
    testSettingsScreens();   // settingsnative
    testSettingsValues();
    testSettingsAvailability();
    testSettingsLeave();
    testSettingsSession();
    testSettingsButtons();
    testSettingsRare();
    testEntrySettings();
    testSettingsAssist();
    testPlanCapacity();   // memfix: after every screen composed above (reads the plans' peaks)
    testRaceLeave();
    testLobbyIsGathering();
    testGatherThreeButtons();
    testHeardAndHints();
    testSafeCopiesReach();
    testSafeCopiesWords();
    testWordsGameVoice();
    testWordsRealBoxes();
    testSettingsVoice();
    testSettingsDescriptions();
    testSettingsBoxes();
    testScriptedChecks();
    testRematchAskByHand();
    testFirstTimeFlowPresses();
    testReport();
    testHarvestTour();
    testMonkey();
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}

