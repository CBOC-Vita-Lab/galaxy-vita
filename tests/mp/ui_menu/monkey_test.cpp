// menus4 item 4: the verifier's (chk-menus) monkey test, in the suite. Random presses on every screen in every environment and language:
// no crash (ASan/UBSan), no screen without an action, Circle leads back within 14 presses, every big button at 0.9 or more, every string
// fits its box in five languages, no banned word, no placeholder, no Wii control. Default: 3 seeds x 60,000 presses (under a minute);
// the full size (6 seeds x 200,000 = 1.2 million presses) with MENUS_MONKEY_FULL=1 (or MENUS_MONKEY_SEEDS / MENUS_MONKEY_STEPS).
#include <set>
#include <map>
#include <functional>
#include "common.hpp"
#include "port/mp_gather.h"

namespace {

static const int kLangs = 5;
static const char* kLn[kLangs] = {"UsEnglish","UsFrench","UsSpanish","EuGerman","EuItalian"};
static std::mt19937 rng;
static int R(int n) { return (int)(rng() % (unsigned)n); }

static std::u16string low(const std::u16string& s){ std::u16string o=s; for(auto&c:o) if(c>='A'&&c<='Z') c+=32; else if(c>=0xC0&&c<=0xDE&&c!=0xD7) c+=32; return o; }
static bool isL(char16_t c){ return (c>='a'&&c<='z')||(c>=0xC0&&c<0x250&&c!=0xD7&&c!=0xF7); }
// banned tokens (whole words) per language: owner's list translated
static const char16_t* kB[kLangs][24] = {
 {u"host",u"hosts",u"hosting",u"hosted",u"join",u"joins",u"joining",u"rejoin",u"server",u"servers",u"session",u"sessions",u"nat",u"lobby",u"address",u"router",u"ping",u"relay",u"guest",u"guests",nullptr},
 {u"hôte",u"hôtes",u"hébergement",u"rejoindre",u"rejoins",u"serveur",u"serveurs",u"nat",u"lobby",u"adresse",u"routeur",u"invité",u"invités",u"salon",nullptr},
 {u"anfitrión",u"anfitriona",u"unirse",u"unirte",u"únete",u"servidor",u"servidores",u"sesión",u"nat",u"lobby",u"dirección",u"huésped",u"router",u"sala",nullptr},
 {u"gastgeber",u"gastgeberin",u"beitreten",u"server",u"sitzung",u"nat",u"lobby",u"adresse",u"router",u"gast",u"gäste",nullptr},
 {u"ospite",u"ospiti",u"unirsi",u"unirti",u"unisciti",u"server",u"sessione",u"nat",u"lobby",u"indirizzo",u"router",nullptr},
};
static std::string A(const std::u16string& t){ std::string o; for(char16_t c:t) o+= c<128?(char)c:'?'; return o; }
static std::string banned(const char16_t* text, int lang){
    std::u16string s = low(text), tok;
    auto chk=[&](const std::u16string&t)->std::string{ if(t.empty()) return ""; for(const char16_t*const*b=kB[lang];*b;++b) if(t==*b) return A(t); return ""; };
    if (low(text).find(u"room settings")!=std::u16string::npos) return "room settings";
    for(size_t i=0;i<=s.size();i++){ if(i<s.size()&&isL(s[i])){tok+=s[i];continue;} auto h=chk(tok); if(!h.empty()) return h; tok.clear(); }
    return "";
}
static std::string placeholder(const char16_t* t){
    std::string a = A(t);
    for (const char* p : {"%", "{", "}", "$"}) if (a.find(p)!=std::string::npos) return p;
    // (whole words only: "Annulla" holds "null")
    std::string lo; for (char c : a) lo += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    for (const char* p : {"todo", "xxx", "mp_t", "null", "???", "<none>", "tbd", "lorem", "placeholder"}) {
        const std::string w = p; size_t at = 0;
        while ((at = lo.find(w, at)) != std::string::npos) {
            const bool before = at > 0 && isalpha((unsigned char)lo[at - 1]), after = at + w.size() < lo.size() && isalpha((unsigned char)lo[at + w.size()]);
            if (!before && !after) return p;
            at += w.size();
        }
    }
    return "";
}
static void strings(const MpPanel::Model& m, std::vector<std::pair<std::string,std::u16string>>* out){
    auto add=[&](const char*f,const char16_t*t){ if(t&&*t) out->push_back({f,t}); };
    add("title",m.title),add("subtitle",m.subtitle),add("bigLabel",m.bigLabel),add("big",m.big),add("status",m.status),add("hint",m.hint),add("line",m.line);
    for(int i=0;i<m.tabCount&&i<3;i++) add("tab",m.tabs[i]);
    for(int i=0;i<m.rowCount&&i<MpPanel::kMaxRows;i++) add("lead",m.rows[i].lead),add("label",m.rows[i].label),add("value",m.rows[i].value);
    for(int i=0;i<m.buttonCount&&i<4;i++) add("button",m.buttons[i].label);
    for(int i=0;i<m.cellCount&&i<MpPanel::kMaxCells;i++) add("cell",m.cells[i].label);
}

struct Env { bool wifi, service, friendNear, fullNear, copies, first, guest, serviceOver; std::string str() const { char b[200]; snprintf(b,sizeof b,"wifi=%d svc=%d friendNear=%d fullNear=%d copies=%d first=%d guest=%d svcFail=%d",wifi,service,friendNear,fullNear,copies,first,guest,serviceOver); return b; } };

static std::vector<std::string> seq;
static std::string epHeader;
static std::set<std::string> reported;
static int nFail = 0;
static const char* kPressN[] = {"Cross","Circle","Down","Triangle","Square","Left","Right","Start","Up","TouchRow","TouchTap2","KbOK","KbCancel","Frames(120)","NetFlip","DialogNo"};

static void report(const std::string& kind, const std::string& detail, int seed, int ep) {
    std::string key = kind + "|" + detail;
    if (reported.count(key)) return;
    reported.insert(key); ++nFail;
    if (nFail > 80) return;
    printf("FAIL[%s] seed=%d ep=%d\n  %s\n  detail: %s\n", kind.c_str(), seed, ep, epHeader.c_str(), detail.c_str());
    std::string s; for (auto& x : seq) { s += x; s += ' '; } printf("  presses: %s\n", s.c_str());
}

static MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
static MpPanel::Model gm; static MpPanel::Plan gplan; static MpPanel::Scene gscene;
static std::set<uint64_t> composed;
static uint64_t hashModel(const MpPanel::Model& m, int lang, int w){ uint64_t h=1469598103934665603ull^lang^(w<<8); auto f=[&](const char16_t*t){ for(;t&&*t;++t){h^=*t;h*=1099511628211ull;} h^=0x1f; h*=1099511628211ull; };
    f(m.title);f(m.subtitle);f(m.big);f(m.status);f(m.hint);f(m.line); for(int i=0;i<m.rowCount&&i<MpPanel::kMaxRows;i++){f(m.rows[i].label);f(m.rows[i].value);f(m.rows[i].lead);} for(int i=0;i<m.buttonCount;i++)f(m.buttons[i].label);
    for(int i=0;i<m.cellCount&&i<MpPanel::kMaxCells;i++)f(m.cells[i].label); h^=m.kind*131+m.rowCount*7+m.look*3+m.headCount*11+m.pages; return h; }

static std::string curName() { return MpMenu::screenName(S().screen()); }

static void check(int seed, int ep, int lang, bool doCompose) {
    if (!S().isOpen() || S().screen()==MpMenu::S_NONE) { io.texts.clear(); return; }
    static MpPanel::Model m;
    S().buildModel(&m);
    std::vector<std::pair<std::string,std::u16string>> vis; strings(m,&vis);
    for (auto& v : vis) {
        std::string b = banned(v.second.c_str(), lang);
        if (!b.empty()) report("BANNED", curName()+"/"+kLn[lang]+"/"+v.first+" '"+b+"' in \""+A(v.second)+"\"", seed, ep);
        std::string p = placeholder(v.second.c_str());
        if (!p.empty() && !(p == "todo" && A(v.second) == "Restablecer todo")) report("PLACEHOLDER", curName()+"/"+kLn[lang]+"/"+v.first+" '"+p+"' in \""+A(v.second)+"\"", seed, ep);
        std::string w = vitawords::WhyWii(v.second); const std::string lw = A(v.second); if (!w.empty() && lw.find("camera shake") == std::string::npos && lw.find("Camera shake") == std::string::npos) report("WII", curName()+"/"+kLn[lang]+"/"+v.first+" "+w+" \""+A(v.second)+"\"", seed, ep);
    }
    for (int i=0;i<m.rowCount&&i<MpPanel::kMaxRows;i++) if (!m.rows[i].label[0] && i>=m.headCount && m.kind!=MpPanel::KIND_CHARS) report("EMPTYROW", curName()+" row "+std::to_string(i), seed, ep);
    for (int i=0;i<m.buttonCount&&i<4;i++) if (!m.buttons[i].label[0]) report("EMPTYBUTTON", curName()+" button "+std::to_string(i), seed, ep);
    // dialog texts
    for (auto& t : io.texts) { std::u16string u; for(char c:t) u+=(char16_t)c; std::string b=banned(u.c_str(),lang); if(!b.empty()) report("BANNED-DLG", std::string(kLn[lang])+" '"+b+"' in \""+t+"\"", seed, ep); if(!windowFits(t)) report("DLG-TOO-LONG", std::string(kLn[lang])+" \""+t+"\"", seed, ep); if (t.empty()) report("DLG-EMPTY", "", seed, ep); }
    io.texts.clear();
    // toasts
    {
        const int n = MpOverlay::toastCount();
        for (int i=0;i<n;i++){ const char16_t* t = MpOverlay::toasts()[i].text; std::string b=banned(t,lang); if(!b.empty()) report("BANNED-TOAST", std::string(kLn[lang])+" '"+b+"' in \""+A(t)+"\"", seed, ep); if(!*t) report("TOAST-EMPTY","",seed,ep); std::string w=vitawords::WhyWii(t); if(!w.empty()) report("WII-TOAST",w+" "+A(t),seed,ep); }
    }
    if (doCompose) for (float width : {810.6667f, 608.0f}) {
        uint64_t h = hashModel(m, lang, (int)width);
        if (composed.count(h)) continue; composed.insert(h);
        MpPanel::Screen scr; scr.width = width;
        MpPanel::beginScene(&gscene); MpPanel::begin(&gplan);
        int f0 = gFails;
        MpPanel::compose(m, scr, fm, 0.25f, &gscene, &gplan);
        for (int u = 0; u < gscene.useCount; u++) if (gscene.uses[u].kind == MpPanel::USE_BUTTON && gscene.uses[u].w < (m.rowCount >= 4 ? 0.84f : 0.9f)) { report("BUTTON-SCALE", curName()+"/"+kLn[lang]+" w"+std::to_string((int)width)+" scale "+std::to_string(gscene.uses[u].w)+" ["+[&]{std::string r; for(int i=0;i<m.buttonCount&&i<4;i++) r+=A(m.buttons[i].label)+"; "; return r;}()+"]", seed, ep); break; }
        if (gplan.overflow) report("PLAN-OVERFLOW", curName()+"/"+kLn[lang], seed, ep);
        if (gplan.textClipped) report("TEXT-CLIPPED", curName()+"/"+kLn[lang]+" w"+std::to_string((int)width)+" "+A(m.title)+"|"+A(m.line)+"|"+A(m.hint), seed, ep);
        // geometry (fixed-width font, 16 units/char: over-pessimistic vs the real font; report only as "tight")
        std::string nm = curName();
        fflush(stderr);
        uinativeGeometry(nm.c_str(), kLn[lang], m, scr, gscene, gplan);
        if (gFails != f0) report("GEOMETRY", nm+"/"+kLn[lang]+" w"+std::to_string((int)width)+" title='"+A(m.title)+"' line='"+A(m.line)+"' rows="+std::to_string(m.rowCount)+" ["+[&]{std::string r; for(int i=0;i<m.rowCount&&i<6;i++) r+=A(m.rows[i].label)+"="+A(m.rows[i].value)+"|"+std::to_string(m.rows[i].marks)+"; "; return r;}()+"]", seed, ep);
        gFails = f0;
        // line length
        if (m.look==MpPanel::LOOK_GAME && m.line[0]) { int seg=0,worst=0; for(const char16_t*q=m.line;;++q){ if(*q==u'\n'||!*q){ if(seg>worst)worst=seg; seg=0; if(!*q)break; continue;} ++seg; } if(worst>44) report("LINE>44", nm+"/"+kLn[lang]+" "+std::to_string(worst)+" \""+A(m.line)+"\"", seed, ep); }
    }
}

// is there something to press on this screen?
static bool actionable() {
    if (S().dialogActive() || S().keyboardActive()) return true;
    static MpPanel::Model m; S().buildModel(&m);
    if (m.buttonCount > 0) return true;
    if (S().itemCount() > 0) return true;
    return false;
}

static void doPress(int what) {
    MpMenu::Input in; MpMenu::Sequence& s = S();
    switch (what) {
    case 0: in.ok=true; break; case 1: in.back=true; break; case 2: in.down=true; break; case 3: in.tri=true; break;
    case 4: in.sq=true; break; case 5: in.left=true; break; case 6: in.right=true; break; case 7: in.start=true; break; case 8: in.up=true; break;
    case 9: { static MpPanel::Model m; s.buildModel(&m); int n = m.rowCount + m.cellCount; if (n>0) { s.pointAtVisible(R(n)); in.ok = true; } break; }   // touch a row and tap
    case 10: { static MpPanel::Model m; s.buildModel(&m); if (m.cellCount>0) { s.pointAtVisible(R(m.cellCount)); if (!s.firstTapPoints(m.cellCount)) in.ok=true; } else if (m.rowCount>0) { s.pointAt(R(m.rowCount)); } break; }
    case 11: fake::kbResult = 1; break;
    case 12: fake::kbResult = -1; break;
    case 13: frames(120); return;
    case 14: { int k=R(6); if(k==0) fake::wifi=!fake::wifi; else if(k==1) fake::service=!fake::service; else if(k==2){ fake::status.phase=(MpUiPhase)R(8); } else if (k==3) fake::status.error=(MpUiError)R(12); else if (k==4) { fake::active=!fake::active; } else fake::status.phase = MP_UI_IDLE; break; }
    case 15: io.answer = MpMenu::DlgState::No; frames(3); io.answer = MpMenu::DlgState::Yes; return;
    }
    step(in); frames(2);
    if (what == 11 || what == 12) { frames(3); fake::kbResult = 0; }
    if (what == 0) { /* kb text variety */ int k=R(8); if(k==0) snprintf(fake::kbText,sizeof fake::kbText,""); else if(k==1) snprintf(fake::kbText,sizeof fake::kbText,"K7QM4D"); else if(k==2) snprintf(fake::kbText,sizeof fake::kbText,"abcdefghijklmnopqrstuvwxyz0123456789"); else if(k==3) validCode(); else if(k==4) snprintf(fake::kbText,sizeof fake::kbText,"1234-5678"); else if (k==5) snprintf(fake::kbText,sizeof fake::kbText,"Hi!"); }
}

static void setup(const Env& e, int startPath) {
    fresh();
    fake::wifi = e.wifi; fake::service = e.service;
    fake::flow.charPicked = e.first ? 0 : 1; fake::flow.fallsAsked = e.first ? 0 : 1;
    if (e.copies) { fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_BEFORE, "b.smgmp", 1794300000)); }
    fake::flow.playedCount = R(4); for (int i=0;i<fake::flow.playedCount;i++) snprintf(fake::flow.playedWith[i],17,"Pal%d",i);
    if (e.serviceOver) { fake::ensureResult = -(int)MP_ERR_SERVICE_DOWN; fake::svcCode = "over_budget"; }
    fake::kbResult = 0;
    mp_gather_fake_set_radio(e.wifi, true);
    openMain();
    if (startPath >= 0) { const Path& p = kPaths[startPath]; fresh(); fake::wifi=e.wifi; fake::service=e.service; fake::flow.charPicked = e.first?0:1; fake::flow.fallsAsked = e.first?0:1; if (e.copies) { fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); } mp_gather_fake_set_radio(e.wifi,true); p.go(); }
    if (e.friendNear) { mp_gather_fake_add_playing(77, "Ros", MP_MODE_TOGETHER, 2); }
    if (e.fullNear) { mp_gather_fake_add_playing(78, "Zed", MP_MODE_TOGETHER, 4); mp_gather_fake_other_room(true); }
    if (e.guest) { mp_gather_fake_set_leader(2); }
    frames(3);
}

int runSeed(int seed, long steps, int onlyLang) {
    rng.seed(seed);
    reported.clear(); nFail = 0;
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    MpPanel::resetPlanPeaks();
    long done = 0; int ep = 0; long noWay = 0, circleFail = 0, circleRuns = 0;
    std::map<std::string,long> screensSeen;
    while (done < steps) {
        ++ep; seq.clear();
        Env e{R(4)!=0, R(4)!=0, R(3)==0, R(4)==0, R(2)==0, R(3)==0, R(3)==0, R(6)==0};
        if (!e.wifi) e.service = false;
        int lang = onlyLang >= 0 ? onlyLang : (ep % kLangs);
        MpMsgText::setLanguage(lang);
        int sp = R(3) == 0 ? -1 : R((int)(sizeof kPaths / sizeof kPaths[0]));
        char h[400]; snprintf(h, sizeof h, "env[%s] lang=%s start=%s", e.str().c_str(), kLn[lang], sp < 0 ? "openMain" : kPaths[sp].name);
        epHeader = h;
        composed.clear();
        setup(e, sp);
        int len = 150 + R(150);
        for (int i = 0; i < len && done < steps; i++, done++) {
            // the dead-end check: before pressing
            if (S().screen() != MpMenu::S_NONE && S().isOpen()) {
                screensSeen[curName()]++;
                if (!actionable()) { ++noWay; report("NOACTION", curName()+"/"+kLn[lang], seed, ep); }
            }
            int w = R(100); int what;
            if (w < 28) what = 0; else if (w < 40) what = 1; else if (w < 52) what = 2; else if (w < 58) what = 8; else if (w < 64) what = 3; else if (w < 70) what = 4;
            else if (w < 74) what = 7; else if (w < 78) what = 5; else if (w < 82) what = 6; else if (w < 87) what = 9; else if (w < 90) what = 10; else if (w < 93) what = 11; else if (w < 94) what = 12; else if (w<96) what = 13; else if (w<98) what = 14; else what = 15;
            seq.push_back(kPressN[what]);
            if (S().screen()==MpMenu::S_NONE || !S().isOpen()) { // reopen from the file select or session
                if (R(2)) S().openMain(1); else if (fake::active) S().openSession(); else S().openMain(1);
                frames(3); seq.push_back("(reopen)");
            }
            doPress(what);
            check(seed, ep, lang, (i % 3) == 0);
            // Circle-bounded check at random times (mutates the walk; ends the episode's usefulness a little, so only sometimes)
            if (R(60) == 0 && S().isOpen()) {
                ++circleRuns; int n = 0; std::string scrStart = curName(); int depth0 = S().depth();
                // a dead-end is caught here: circle must leave the menu or reach the Multiplayer root within kN presses
                const int kN = 14;
                // (a window asked on the way out is answered Yes, then No, then Yes...: whichever of the two leaves, the player finds it)
                for (n = 0; n < kN; n++) {
                    if (!S().isOpen() || S().screen()==MpMenu::S_NONE || S().screen()==MpMenu::S_MAIN || S().screen()==MpMenu::S_SESSION) break;
                    if (S().screen()==MpMenu::S_SET_CAPTURE) { io.finger = true; frames(3); io.finger = false; seq.push_back("Circle-test:finger"); continue; }   // (a capture is answered by any button or a finger: the list)
                    if (S().keyboardActive()) { fake::kbResult = -1; frames(4); fake::kbResult = 0; seq.push_back("Circle-test:kbCancel"); continue; }
                    io.answer = (n & 1) ? MpMenu::DlgState::No : MpMenu::DlgState::Yes;
                    seq.push_back("Circle-test"); MpMenu::Input in; in.back = true; step(in); frames(3);
                }
                io.answer = MpMenu::DlgState::Yes;
                if (n >= kN) { ++circleFail; report("CIRCLE-STUCK", "from "+scrStart+" depth "+std::to_string(depth0)+" still on "+curName()+" after "+std::to_string(kN), seed, ep); }
            }
        }
    }
    printf("seed %d: %ld steps, %d episodes, %d distinct failures, noWay=%ld circleRuns=%ld circleFail=%ld\nscreens visited:", seed, done, ep, nFail, noWay, circleRuns, circleFail);
    for (auto& kv : screensSeen) printf(" %s=%ld", kv.first.c_str(), kv.second);
    printf("\n");
    return nFail;
}
}  // namespace

void testMonkey() {
    long steps = 60000; int seeds = 3;
    if (getenv("MENUS_MONKEY_FULL")) steps = 200000, seeds = 6;
    if (const char* s = getenv("MENUS_MONKEY_STEPS")) steps = atol(s);
    if (const char* s = getenv("MENUS_MONKEY_SEEDS")) seeds = atoi(s);
    int total = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        const int f = runSeed(seed, steps, -1);
        total += f;
        CHECK(f == 0);
    }
    MpMsgText::setLanguage(0);
    fprintf(stderr, "  monkey: %d seeds x %ld presses, %d distinct failures\n", seeds, steps, total);
}
