#include "port/settings_ui.h"
#include "player_settings_files.h"
#include "player_settings_font_fixture.h"
#include "settings_backdrop_check.h"
#include <algorithm>
#include <cassert>
#include <iostream>
using namespace port;
using namespace settings;
static SettingsSession Session(OpenMode mode=OpenMode::ColdStart) {
    SettingsSession s{};s.mode=mode;s.controls=&controls::GetControlApi();s.document.config=DefaultConfig(*s.controls);s.active=s.document.config;
    s.controller=controls::NominalCaps(controls::ProfileId::Handheld);s.features={true,true};s.languages.names={"English","French","German","Spanish","Italian"};return s;
}
static uint64_t clockUs=0;
static void Input(UiState& s,uint32_t menu=0,uint64_t physical=0,bool connected=true) {auto caps=s.session->controller;caps.connected=connected;Step(s,{menu,physical,caps,s.session->detectedProfile},clockUs+=40000);}
static void Tap(UiState& s,uint32_t key) {Input(s,key);Input(s);}
static void Arm(UiState& s) {Input(s);}
int main() {
    auto session=Session();UiState s;Begin(s,session);
    Input(s,Accept,controls::Cross);assert(!s.armed&&s.working.fpsLimit==0);Input(s);Tap(s,Right);assert(s.working.fpsLimit==30&&session.active.fpsLimit==0);
    Tap(s,Right);assert(s.working.fpsLimit==0);Tap(s,Left);assert(s.working.fpsLimit==30);Tap(s,Left);assert(s.working.fpsLimit==0);Tap(s,Right);assert(s.working.fpsLimit==30);
    Tap(s,Down);Tap(s,Right);assert(s.working.autoQuality==AutoQuality::Enabled&&s.working.fpsLimit==30);
    Tap(s,Down);Tap(s,Right);assert(s.working.language=="English");Tap(s,Down);Tap(s,Accept);assert(s.working.reducedMotion);
    Tap(s,Back);assert(s.modal==Modal::Discard&&s.modalChoice==0);Tap(s,Accept);assert(!s.done&&s.modal==Modal::None);Tap(s,Back);Tap(s,Right);Tap(s,Accept);assert(s.done&&!s.result.saved);
    session=Session(OpenMode::Running);Begin(s,session);Arm(s);s.row=5;Input(s,Accept);assert(s.modal==Modal::Restart&&s.modalChoice==0);Input(s,Accept);assert(!s.saveRequested);Input(s);Tap(s,Accept);assert(s.modal==Modal::None&&!s.saveRequested);
    Tap(s,Accept);Tap(s,Right);Tap(s,Accept);assert(s.saveRequested&&s.pendingIntent==ExitIntent::Restart);
    Begin(s,session);Arm(s);s.row=4;Tap(s,Accept);assert(s.saveRequested&&s.pendingIntent==ExitIntent::Return);
    Begin(s,session);Arm(s);Tap(s,NextPage);assert(s.page==Page::Controls);s.row=1;Input(s,Accept);assert(s.modal==Modal::Capture);
    Input(s,Accept,controls::Cross);assert(s.candidate==0);Input(s);Input(s,Back,controls::Circle);assert(s.candidate==controls::Circle&&s.modal==Modal::Capture);
    Input(s);assert(s.modal==Modal::Conflict&&s.modalChoice==0);assert(s.working.profiles[0].bindings[0].buttons[0]=="cross");
    Tap(s,Right);Tap(s,Accept);assert(s.modal==Modal::None&&s.working.profiles[0].bindings[0].buttons[0]=="circle");
    Tap(s,Accept);Input(s);Input(s,0,controls::Start);Input(s,0,controls::Start|controls::Select);assert(s.modal==Modal::None&&s.working.profiles[0].bindings[0].buttons[0]=="circle");
    Input(s);Tap(s,Accept);Input(s);Input(s,0,controls::Cross|controls::Square);assert(s.candidate==0&&!s.captureArmed);Input(s);Input(s,0,controls::Start);Input(s);assert(s.modal==Modal::Conflict);
    Input(s,0,0,false);assert(s.modal==Modal::None&&!s.armed&&!s.controller.connected);Input(s,Accept,controls::Cross,true);assert(!s.armed);Input(s);assert(s.armed);
    s.row=0;Tap(s,Reset);assert(s.modal==Modal::ResetProfile);Tap(s,Right);Tap(s,Accept);assert(s.working.profiles[0].bindings[0].buttons[0]=="cross");s.row=1;
    Tap(s,Accept);Input(s);Input(s,Reset,controls::Square);assert(s.modal==Modal::Capture&&s.candidate==controls::Square);Input(s);assert(s.modal==Modal::Conflict);Tap(s,Back);
    Tap(s,Reset);assert(s.modal==Modal::ButtonPicker);Tap(s,Back);
    UiInput limited{Accept,controls::Cross,session.controller};limited.caps.availableButtons&=~(controls::Start|controls::Select);
    Step(s,limited,clockUs+=40000);assert(!s.armed&&s.modal==Modal::None);
    limited.menuHeld=0;limited.physicalHeld=0;Step(s,limited,clockUs+=40000);limited.menuHeld=Accept;Step(s,limited,clockUs+=40000);assert(s.modal==Modal::ButtonPicker);
    limited.menuHeld=0;Step(s,limited,clockUs+=40000);limited.menuHeld=Back;Step(s,limited,clockUs+=40000);Input(s);Input(s);
    s.row=0;Tap(s,Right);assert(s.working.profile==controls::ProfileId::PlayStation);
    s.row=3;Tap(s,Right);assert(s.bindingSlot==1);Tap(s,Reset);Tap(s,Accept);assert(s.working.profiles[1].bindings[2].buttons[1].empty());
    Tap(s,Reset);Tap(s,Down);Tap(s,Accept);assert(s.modal==Modal::Conflict);Arm(s);Tap(s,Right);Tap(s,Accept);assert(s.modal==Modal::None&&s.message=="Every required action needs a binding"&&s.working.profiles[1].bindings[0].buttons[0]=="cross");
    session=Session();session.features={false,false};session.document.config.fpsLimit=30;Begin(s,session);Arm(s);Tap(s,Right);assert(s.working.fpsLimit==30&&!s.message.empty());Tap(s,NextPage);s.row=1;Tap(s,Accept);assert(s.modal==Modal::None);
    session=Session();session.languages.names.clear();session.document.config.language="French";Begin(s,session);Arm(s);s.row=2;Tap(s,Right);assert(s.working.language=="French"&&!s.message.empty());
    session=Session();session.detectedProfile=controls::ProfileId::PlayStation;session.controller=controls::NominalCaps(session.detectedProfile);session.controller.availableButtons&=~controls::L2;session.document.config.profile=session.detectedProfile;
    Begin(s,session);Arm(s);Tap(s,NextPage);s.row=1;Tap(s,Reset);for(unsigned i=0;i<6;++i)Tap(s,Down);Tap(s,Accept);
    assert(s.working.profiles[1].bindings[0].buttons[0]!="l2");
    std::string capabilityError;assert(session.controls->validate(s.working.profiles[1],s.working.profile,session.controller,capabilityError));
    // Saved inactive mappings remain editable against nominal capabilities.
    session.detectedProfile=controls::ProfileId::Handheld;Begin(s,session);Arm(s);Tap(s,NextPage);s.row=1;Tap(s,Reset);for(unsigned i=0;i<6;++i)Tap(s,Down);Tap(s,Accept);assert(s.working.profiles[1].bindings[0].buttons[0]=="l2");
    session.detectedProfile=controls::ProfileId::PlayStation;session.document.config=s.working;Begin(s,session);Arm(s);s.row=4;Tap(s,Accept);assert(!s.saveRequested&&!s.message.empty());
    session=Session();session.detectedProfile=controls::ProfileId::PlayStation;session.document.config.profile=session.detectedProfile;session.controller=controls::NominalCaps(session.detectedProfile);
    Begin(s,session);Arm(s);Tap(s,NextPage);s.row=3;Tap(s,Right);Tap(s,Reset);for(unsigned i=0;i<16;++i)Tap(s,Down);assert(s.picker==16);
    UiInput shrunk{Accept,0,session.controller,session.detectedProfile};shrunk.caps.availableButtons&=~controls::L2;Step(s,shrunk,clockUs+=40000);assert(!s.saveRequested&&!s.done);
    session.controller.availableButtons=0;Begin(s,session);Arm(s);Tap(s,NextPage);s.row=1;Tap(s,Reset);Tap(s,Accept);assert(s.modal==Modal::ButtonPicker&&!s.done);
    session=Session();std::vector<uint32_t> pixels(544*976+2,0x12345678);Surface surface{pixels.data()+1,960,544,976};
    FontBundle synthetic;auto syntheticBytes=settings_fixture::Archive();const char* fontError=nullptr;assert(ParseGameFontArchive(syntheticBytes.data(),syntheticBytes.size(),synthetic,fontError));
    auto fontSession=session;fontSession.fonts=synthetic.view();const auto measure=MeasureMenuText(fontSession,"A\xc3\xa9\xe4\xb8\x80");
    assert(measure.advance==20+33+39&&measure.minX==-7&&measure.height==27);
    assert(MeasureMenuText(fontSession,"\xc3\xa9").advance!=MeasureMenuText(session,"\xc3\xa9").advance);
    UiState fontState;Begin(fontState,fontSession);DrawAudit fontAudit;DrawChecked(fontState,fontSession,surface,0,fontAudit);assert(fontAudit.minX>=32&&fontAudit.maxX<=928&&fontAudit.maxY<=512);
    auto tallBytes=settings_fixture::Font();tallBytes[44]=3;GameFont fullCell;assert(ParseGameFont(tallBytes.data(),tallBytes.size(),fullCell,fontError));
    auto tallSession=session;tallSession.fonts={&fullCell,&fullCell};UiState tallState;Begin(tallState,tallSession);tallState.message="A";
    Draw(tallState,tallSession,surface,0);assert(surface.pixels[476*surface.stride+448]==0xff9487fbu);
    auto shoulderSession=Session();UiState shoulder;Begin(shoulder,shoulderSession);shoulder.page=Page::Controls;shoulder.row=4;shoulder.working.profile=controls::ProfileId::PlayStation;
    Draw(shoulder,shoulderSession,surface,0);for(unsigned y=202;y<=220;++y)for(unsigned x=612;x<928;++x)assert(surface.pixels[y*surface.stride+x]!=0xffe9dc49u);
    // The mouse-style aiming option is the last row of the controls list, saved with the other choices; the profile row marks the profile as chosen.
    {
        auto aimSession=Session();UiState a;Begin(a,aimSession);Arm(a);Tap(a,NextPage);assert(a.page==Page::Controls&&!a.working.mouseAim&&!a.working.profileChosen);
        const unsigned aimRow=static_cast<unsigned>(aimSession.controls->actionCount)+1;assert(RowCount(a)==aimRow+4);
        Tap(a,Up);Tap(a,Up);Tap(a,Up);Tap(a,Up);assert(a.row==aimRow);                 // Cancel, Apply, Save, then the option (wrapping from row 0)
        Tap(a,Right);assert(a.working.mouseAim&&Dirty(a));Tap(a,Left);assert(!a.working.mouseAim&&!Dirty(a));Tap(a,Accept);assert(a.working.mouseAim&&a.modal==Modal::None);
        Tap(a,Reset);assert(a.modal==Modal::None&&a.working.mouseAim);                 // Square has nothing to reset here
        Tap(a,Down);assert(a.row==aimRow+1);Tap(a,Accept);assert(a.saveRequested&&a.working.mouseAim);
        Begin(a,aimSession);Arm(a);Tap(a,NextPage);a.row=0;Tap(a,Right);assert(a.working.profileChosen&&a.working.profile==controls::ProfileId::PlayStation);
        Tap(a,Left);assert(a.working.profileChosen&&a.working.profile==controls::ProfileId::Handheld&&Dirty(a));
        // It draws, as a row of the list, on both profiles, and the actions above it are still reachable.
        UiState d;for(unsigned profile=0;profile<2;++profile)for(unsigned row:{aimRow-1,aimRow,aimRow+1,aimRow+3}) {
            Begin(d,aimSession);d.page=Page::Controls;d.working.profile=static_cast<controls::ProfileId>(profile);d.row=row;d.working.mouseAim=row&1;
            DrawAudit audit;DrawChecked(d,aimSession,surface,0,audit);assert(audit.minX>=32&&audit.maxX<=928&&audit.maxY<=512);
        }
        // The highlight on the pad picture lands on L2 / R2 (a box of its own above the L1 / R1 box) and stays clear of the status text.
        auto padSession=Session();padSession.detectedProfile=controls::ProfileId::PlayStation;padSession.controller=controls::NominalCaps(controls::ProfileId::PlayStation);
        const auto playstation=controls::ProfileId::PlayStation;const auto caps=controls::NominalCaps(playstation);std::string conflict;
        constexpr uint32_t kCyan=0xffe9dc49u;
        struct Case {const char* action;const char* button;unsigned row;int cx,cy;};
        for(const Case& c:{Case{"star_bits","r2",2,837,233},Case{"crouch","l2",4,686,233},Case{"spin","r1",3,837,247},Case{"crouch","l1",4,686,247}}) {
            UiState h;Begin(h,padSession);h.page=Page::Controls;h.working.profile=playstation;h.row=c.row;
            auto map=h.working.profiles[1];
            if(std::string(c.action)=="spin") map=h.working.profiles[1];             // spin keeps Square + R1: slot 1 is R1
            else assert(padSession.controls->bind(map,c.action,0,c.button,true,caps,conflict)==controls::BindResult::Applied);
            h.working.profiles[1]=map;if(std::string(c.action)=="spin")h.bindingSlot=1;
            Draw(h,padSession,surface,0);
            const unsigned x=static_cast<unsigned>(c.cx)-12,y=static_cast<unsigned>(c.cy)-12;
            assert(surface.pixels[y*surface.stride+x]==kCyan&&surface.pixels[y*surface.stride+x+24]==kCyan&&surface.pixels[(y+24)*surface.stride+x]==kCyan);  // the box's corners
            for(unsigned yy=202;yy<=220;++yy)for(unsigned xx=612;xx<928;++xx)assert(surface.pixels[yy*surface.stride+xx]!=kCyan);
            // and nowhere else on the picture: the other shoulder's box is absent
            const unsigned otherX=static_cast<unsigned>(c.cx==837?686:837)-12;assert(surface.pixels[y*surface.stride+otherX]!=kCyan);
        }
    }
    // Optional backdrop: null / unusable keeps the exact old gradient; a picture is drawn scaled, dimmed and legible.
    {
        auto bdSession=Session();UiState b;Begin(b,bdSession);std::vector<uint32_t> plain(960*544),other(plain.size());
        Draw(b,bdSession,{plain.data(),960,544,960},0);
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)assert(plain[y*960+x]==backdrop_check::RGB(9+y/90,18+y/65,25+y/30));  // the gradient, unchanged
        std::vector<uint32_t> tiny(4,0xffffffffu),huge(961*10,0xffffffffu);
        const struct {const uint32_t* pixels;unsigned w,h;} unusable[]={{nullptr,480,272},{tiny.data(),0,272},{tiny.data(),480,0},{tiny.data(),1,2},{tiny.data(),2,1},{huge.data(),961,10},{huge.data(),10,545}};
        for(const auto& u:unusable) {bdSession.backdrop=u.pixels;bdSession.backdropW=u.w;bdSession.backdropH=u.h;Draw(b,bdSession,{other.data(),960,544,960},0);assert(other==plain);}
        // Pictures: the worst case (all white), a pattern at the stored size, odd sizes, and full size.
        auto picture=[](unsigned w,unsigned h,unsigned kind) {
            std::vector<uint32_t> v(static_cast<size_t>(w)*h);
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
                const unsigned r=kind==0?255:(x*255/w),g=kind==0?255:(y*255/h),c=kind==0?255:((x/3+y/3)&1?255:40);
                v[static_cast<size_t>(y)*w+x]=0xff000000u|(c<<16)|(g<<8)|r;
            }
            return v;
        };
        const struct {unsigned w,h,kind;} sizes[]={{480,272,0},{480,272,1},{480,272,2},{2,2,1},{7,5,2},{960,544,1},{960,544,0},{481,273,2}};
        for(const auto& sz:sizes)for(bool fonts:{false,true})for(auto page:{Page::Display,Page::Controls})for(auto modal:{Modal::None,Modal::Capture,Modal::Overrides})for(unsigned profile=0;profile<2;++profile) {
            const auto pic=picture(sz.w,sz.h,sz.kind);
            bdSession=Session();bdSession.fonts=fonts?synthetic.view():FontSet{};bdSession.backdrop=pic.data();bdSession.backdropW=sz.w;bdSession.backdropH=sz.h;
            bdSession.overrides={{"GXV_FPS_LIMIT","no","Existing picture-rate path"}};
            Begin(b,bdSession);b.page=page;b.modal=modal;b.working.profile=static_cast<controls::ProfileId>(profile);b.row=page==Page::Controls?3:1;b.action="jump";b.message="Settings replacement rename failed.";
            std::vector<uint32_t> frame(976*544+2,0x12345678);Surface framed{frame.data()+1,960,544,976};
            DrawAudit bdAudit;DrawChecked(b,bdSession,framed,0,bdAudit);
            assert(frame.front()==0x12345678&&frame.back()==0x12345678&&bdAudit.minX>=32&&bdAudit.minY>=32&&bdAudit.maxX<=928&&bdAudit.maxY<=512);
            for(unsigned y=0;y<544;++y)for(unsigned x=960;x<976;++x)assert(frame[1+y*976+x]==0x12345678);
            std::vector<uint32_t> tight(960*544);for(unsigned y=0;y<544;++y)std::copy_n(frame.begin()+1+y*976,960,tight.begin()+y*960);
            // The picture shows where the menu draws nothing (the 32 px margin; modals dim the whole screen) at exactly the scaled, dimmed value.
            if(modal==Modal::None)for(unsigned y:{0u,1u,100u,271u,272u,400u,543u})for(unsigned x:{0u,5u,31u})assert(tight[y*960+x]==backdrop_check::Expected(pic.data(),sz.w,sz.h,x,y));
            if(modal==Modal::None&&sz.w>=7) {
                const auto result=backdrop_check::CheckText(tight,pic.data(),sz.w,sz.h,4.5);
                assert(result.textPixels>1000&&!result.failures);
            }
        }
        // Reduced motion freezes the dots over a backdrop too; otherwise they still twinkle.
        const auto pic=picture(480,272,1);bdSession=Session();bdSession.backdrop=pic.data();bdSession.backdropW=480;bdSession.backdropH=272;Begin(b,bdSession);
        b.working.reducedMotion=true;Draw(b,bdSession,{plain.data(),960,544,960},0);Draw(b,bdSession,{other.data(),960,544,960},999999);assert(plain==other);
        b.working.reducedMotion=false;Draw(b,bdSession,{plain.data(),960,544,960},0);Draw(b,bdSession,{other.data(),960,544,960},999999);
        size_t moved=0;for(size_t i=0;i<plain.size();++i)moved+=plain[i]!=other[i];assert(moved>0&&moved<10000);
        // The brightness curve stays inside the dim range and never brightens the picture.
        for(unsigned y=0;y<544;++y)assert(port::settings::BackdropDim(y)>=48&&port::settings::BackdropDim(y)<=96);
    }
    for(bool fonts:{false,true})for(auto page:{Page::Display,Page::Controls})for(auto modal:{Modal::None,Modal::Discard,Modal::Restart,Modal::Capture,Modal::Conflict,Modal::ButtonPicker,Modal::ResetProfile,Modal::Overrides})for(unsigned profile=0;profile<2;++profile) {
        session.fonts=fonts?synthetic.view():FontSet{};
        Begin(s,session);s.page=page;s.modal=modal;s.working.profile=static_cast<controls::ProfileId>(profile);s.row=4;s.action="jump";s.message=std::string(300,'W');
        Draw(s,session,surface,0);assert(pixels.front()==0x12345678&&pixels.back()==0x12345678);
        for(unsigned y=0;y<544;++y)for(unsigned x=960;x<976;++x)assert(pixels[1+y*976+x]==0x12345678);
    }
    session.fonts={};
    std::vector<uint32_t> first(960*544),second(first.size());Begin(s,session);s.working.reducedMotion=true;
    Draw(s,session,{first.data(),960,544,960},0);Draw(s,session,{second.data(),960,544,960},999999);assert(first==second);
    s.working.reducedMotion=false;Draw(s,session,{first.data(),960,544,960},0);Draw(s,session,{second.data(),960,544,960},999999);
    size_t changed=0;for(size_t i=0;i<first.size();++i)changed+=first[i]!=second[i];assert(changed>0&&changed<10000);
    DrawAudit audit;DrawChecked(s,session,{first.data(),960,544,960},0,audit);assert(audit.minX>=32&&audit.minY>=32&&audit.maxX<=928&&audit.maxY<=512&&audit.textRuns>=15);
    // The Reduced motion row's note ("Less camera shake and screen blur.") fits its line with the fallback and the game font, off and on.
    for(bool fonts:{false,true})for(bool on:{false,true}) {
        session.fonts=fonts?synthetic.view():FontSet{};Begin(s,session);s.row=3;s.working.reducedMotion=on;
        DrawAudit noteAudit;DrawChecked(s,session,{first.data(),960,544,960},0,noteAudit);
        assert(noteAudit.minX>=32&&noteAudit.minY>=32&&noteAudit.maxX<=928&&noteAudit.maxY<=512);
        // The test font is wider than the game's and truncates labels elsewhere already; the note adds no truncated run to what row 2 shows.
        UiState other=s;other.row=2;DrawAudit otherAudit;DrawChecked(other,session,{second.data(),960,544,960},0,otherAudit);
        assert(noteAudit.truncatedRuns==otherAudit.truncatedRuns&&(fonts||noteAudit.truncatedRuns==0));
        size_t noteDiff=0;for(size_t y=372;y<410;++y)for(size_t x=60;x<900;++x)noteDiff+=first[y*960+x]!=second[y*960+x];assert(noteDiff>100);  // row 3 has its own note line
    }
    session.fonts={};
    session.overrides={{"GXV_FPS_LIMIT","no","Existing picture-rate path"},{"GXV_REMAP","0","Remapping disabled"}};
    Begin(s,session);Arm(s);Tap(s,Reset);assert(s.modal==Modal::Overrides);Tap(s,Accept);assert(s.modal==Modal::None);
    // Arbitrarily ordered valid mappings must still show/edit the catalog action.
    session=Session();std::swap(session.document.config.profiles[0].bindings[0],session.document.config.profiles[0].bindings[1]);
    Begin(s,session);Arm(s);Tap(s,NextPage);s.row=1;
    Draw(s,session,{first.data(),960,544,960},0);
    auto ordered=Session();UiState orderedState;Begin(orderedState,ordered);orderedState.page=Page::Controls;orderedState.row=1;
    Draw(orderedState,ordered,{second.data(),960,544,960},0);assert(first==second);
    Tap(s,Accept);assert(s.action=="jump");
    struct Host {int i=0;uint64_t now=0;bool present=true;};Host host;
    session.surface={first.data(),960,544,960};session.host={&host,
        [](void* p,UiInput& input){auto& h=*static_cast<Host*>(p);input={0,0,controls::NominalCaps(controls::ProfileId::Handheld)};if(++h.i>3)return false;return true;},
        [](void* p,const Surface&){return static_cast<Host*>(p)->present;},
        [](void* p){return static_cast<Host*>(p)->now;},[](void* p,uint64_t t){static_cast<Host*>(p)->now=t;}};
    assert(Run(session).intent==ExitIntent::Exit);session.mode=OpenMode::Running;host={};host.present=false;assert(Run(session).intent==ExitIntent::Return);
    struct Script {std::vector<uint32_t> keys;size_t index=0;uint64_t now=0;bool present=true;};
    for(auto mode:{OpenMode::ColdStart,OpenMode::Running})for(unsigned choice=0;choice<3;++choice)for(bool failure:{false,true}) {
        session=Session(mode);session.surface={first.data(),960,544,960};UiFiles files;files.failWrite=failure;auto fileOps=files.ops();session.files=&fileOps;session.settingsPath="player-settings.cfg";
        Script script{{0,Right,0}};
        for(unsigned row=0;row<4+choice;++row){script.keys.push_back(Down);script.keys.push_back(0);}
        script.keys.push_back(Accept);script.keys.push_back(0);
        if(choice==1&&mode==OpenMode::Running)script.keys.insert(script.keys.end(),{Right,0,Accept,0});
        if(choice==2)script.keys.insert(script.keys.end(),{Right,0,Accept,0});
        session.host={&script,
            [](void* p,UiInput& input){auto& h=*static_cast<Script*>(p);if(h.index==h.keys.size())return false;input={h.keys[h.index++],0,controls::NominalCaps(controls::ProfileId::Handheld)};return true;},
            [](void* p,const Surface&){return static_cast<Script*>(p)->present;},
            [](void* p){return static_cast<Script*>(p)->now;},[](void* p,uint64_t t){static_cast<Script*>(p)->now=t;}};
        auto result=Run(session);
        if(choice==2){assert(!result.saved&&files.writes==0);}
        else if(failure){assert(!result.saved&&files.writes==1&&files.data.empty());}
        else {assert(result.saved&&files.writes==1);assert(result.intent==(choice==0?(mode==OpenMode::Running?ExitIntent::Return:ExitIntent::Play):(mode==OpenMode::Running?ExitIntent::Restart:ExitIntent::Exit)));}
        assert(session.active.fpsLimit==0);
    }
    std::cout<<"ui: navigation, entry/disconnect rearm, capture/chord/conflicts, results, raster canaries and animation passed\n";
}
