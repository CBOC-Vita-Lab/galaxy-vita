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
    Input(s,Accept,controls::Cross);assert(!s.armed&&s.working.fpsLimit==0);Input(s);
    // Preset row: Right goes Best picture -> Balanced -> Performance -> Best picture, Left back; each sets only its covered rows.
    assert(s.row==RowPreset&&PresetOf(s.working)==Preset::BestPicture);
    Tap(s,Right);assert(PresetOf(s.working)==Preset::Balanced&&!s.working.aboveNative&&s.working.antiAliasing==AntiAliasing::Auto&&s.working.sharpness==Sharpness::Auto&&s.working.depthOfField&&session.active.aboveNative);
    Tap(s,Right);assert(PresetOf(s.working)==Preset::Performance&&!s.working.aboveNative&&s.working.antiAliasing==AntiAliasing::Off&&s.working.sharpness==Sharpness::Off&&!s.working.depthOfField);
    Tap(s,Right);assert(PresetOf(s.working)==Preset::BestPicture&&s.working.aboveNative&&s.working.depthOfField&&!Dirty(s));
    Tap(s,Left);assert(PresetOf(s.working)==Preset::Performance);Tap(s,Left);assert(PresetOf(s.working)==Preset::Balanced);Tap(s,Left);assert(PresetOf(s.working)==Preset::BestPicture);
    Tap(s,Accept);assert(PresetOf(s.working)==Preset::Balanced);Tap(s,Left);   // Cross steps forward like Right
    // The rows a preset leaves alone survive it: picture rate, HD textures, core plan, reduced motion, language.
    s.working.hdTextures=false;s.working.coreHelp=false;s.working.reducedMotion=true;s.working.language="French";Tap(s,Right);Tap(s,Right);
    assert(!s.working.hdTextures&&!s.working.coreHelp&&s.working.reducedMotion&&s.working.language=="French");s.working.hdTextures=true;s.working.coreHelp=true;s.working.reducedMotion=false;s.working.language="";
    Tap(s,Right);assert(PresetOf(s.working)==Preset::BestPicture);
    Tap(s,Down);assert(s.row==RowRate);Tap(s,Right);assert(s.working.fpsLimit==30&&session.active.fpsLimit==0&&PresetOf(s.working)==Preset::BestPicture); // the rate is not in a preset
    Tap(s,Right);assert(s.working.fpsLimit==0);Tap(s,Left);assert(s.working.fpsLimit==30);Tap(s,Left);assert(s.working.fpsLimit==0);Tap(s,Right);assert(s.working.fpsLimit==30);
    // Editing any covered row reads Custom; putting it back reads the preset again.
    struct Covered {unsigned row;const char* what;};
    for(const Covered c:{Covered{RowDynamic,"dynamic"},Covered{RowSharper,"sharper"},Covered{RowMario,"mario"},Covered{RowAntiAliasing,"aa"},Covered{RowSharpness,"sharpness"},Covered{RowEffects,"effects"},Covered{RowDepthOfField,"dof"}}) {
        s.row=c.row;Tap(s,Right);assert(PresetOf(s.working)==(c.row==RowSharper?Preset::Balanced:Preset::Custom));   // Sharper off alone is exactly Balanced
        if(c.row==RowSharpness){assert(s.working.sharpness==Sharpness::Off);Tap(s,Right);assert(s.working.sharpness==Sharpness::Strong&&PresetOf(s.working)==Preset::Custom);}
        Tap(s,c.row==RowSharpness?Right:Left);assert(PresetOf(s.working)==Preset::BestPicture);
    }
    s.row=RowSharpness;Tap(s,Left);assert(s.working.sharpness==Sharpness::Strong);Tap(s,Right);assert(s.working.sharpness==Sharpness::Auto);
    // From Custom, Right starts the list at Best picture and Left at Performance.
    s.row=RowMario;Tap(s,Right);assert(PresetOf(s.working)==Preset::Custom);s.row=RowPreset;Tap(s,Right);assert(PresetOf(s.working)==Preset::BestPicture&&s.working.prioritizeMario);
    s.row=RowMario;Tap(s,Right);s.row=RowPreset;Tap(s,Left);assert(PresetOf(s.working)==Preset::Performance);Tap(s,Left);Tap(s,Left);assert(PresetOf(s.working)==Preset::BestPicture);
    // Dynamic resolution Off is Custom and its On is the default choice, not a forced one.
    s.row=RowDynamic;Tap(s,Right);assert(s.working.autoQuality==AutoQuality::Disabled&&PresetOf(s.working)==Preset::Custom);Tap(s,Right);assert(s.working.autoQuality==AutoQuality::Default&&PresetOf(s.working)==Preset::BestPicture);
    {auto on=s.working;on.autoQuality=AutoQuality::Enabled;assert(PresetOf(on)==Preset::BestPicture);}   // a saved "on" reads as the preset too
    // The remaining rows toggle.
    s.row=RowHdTextures;Tap(s,Right);assert(!s.working.hdTextures);Tap(s,Left);assert(s.working.hdTextures);
    s.row=RowCorePlan;Tap(s,Accept);assert(!s.working.coreHelp);Tap(s,Accept);assert(s.working.coreHelp);
    s.row=RowLanguage;Tap(s,Right);assert(s.working.language=="English");Tap(s,Left);assert(s.working.language.empty());Tap(s,Right);
    s.row=RowMotion;Tap(s,Accept);assert(s.working.reducedMotion);
    // Up and Down walk every row (settings, then the three commands) and wrap; the commands also move with Left and Right.
    s.row=0;Tap(s,Up);assert(s.row==kDisplayRows+2);Tap(s,Down);assert(s.row==0);for(unsigned i=0;i<kDisplayRows;++i)Tap(s,Down);assert(s.row==kDisplayRows);
    Tap(s,Right);assert(s.row==kDisplayRows+1);Tap(s,Right);assert(s.row==kDisplayRows+2);Tap(s,Right);assert(s.row==kDisplayRows);Tap(s,Left);assert(s.row==kDisplayRows+2);Tap(s,Left);Tap(s,Left);assert(s.row==kDisplayRows);
    Tap(s,Down);assert(s.row==kDisplayRows+1);Tap(s,Up);Tap(s,Up);assert(s.row==RowMotion+1);Tap(s,Up);assert(s.row==RowMotion);
    Tap(s,Back);assert(s.modal==Modal::Discard&&s.modalChoice==0);Tap(s,Accept);assert(!s.done&&s.modal==Modal::None);Tap(s,Back);Tap(s,Right);Tap(s,Accept);assert(s.done&&!s.result.saved);
    session=Session(OpenMode::Running);Begin(s,session);Arm(s);s.row=kDisplayRows+1;Input(s,Accept);assert(s.modal==Modal::Restart&&s.modalChoice==0);Input(s,Accept);assert(!s.saveRequested);Input(s);Tap(s,Accept);assert(s.modal==Modal::None&&!s.saveRequested);
    Tap(s,Accept);Tap(s,Right);Tap(s,Accept);assert(s.saveRequested&&s.pendingIntent==ExitIntent::Restart);
    Begin(s,session);Arm(s);s.row=kDisplayRows;Tap(s,Accept);assert(s.saveRequested&&s.pendingIntent==ExitIntent::Return);
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
    session=Session();session.features={false,false};session.document.config.fpsLimit=30;Begin(s,session);Arm(s);s.row=RowRate;Tap(s,Right);assert(s.working.fpsLimit==30&&!s.message.empty());Tap(s,NextPage);s.row=1;Tap(s,Accept);assert(s.modal==Modal::None);
    session=Session();session.languages.names.clear();session.document.config.language="French";Begin(s,session);Arm(s);s.row=RowLanguage;Tap(s,Right);assert(s.working.language=="French"&&!s.message.empty());
    session=Session();session.detectedProfile=controls::ProfileId::PlayStation;session.controller=controls::NominalCaps(session.detectedProfile);session.controller.availableButtons&=~controls::L2;session.document.config.profile=session.detectedProfile;
    Begin(s,session);Arm(s);Tap(s,NextPage);s.row=1;Tap(s,Reset);for(unsigned i=0;i<6;++i)Tap(s,Down);Tap(s,Accept);
    assert(s.working.profiles[1].bindings[0].buttons[0]!="l2");
    std::string capabilityError;assert(session.controls->validate(s.working.profiles[1],s.working.profile,session.controller,capabilityError));
    // Saved inactive mappings remain editable against nominal capabilities.
    session.detectedProfile=controls::ProfileId::Handheld;Begin(s,session);Arm(s);Tap(s,NextPage);s.row=1;Tap(s,Reset);for(unsigned i=0;i<6;++i)Tap(s,Down);Tap(s,Accept);assert(s.working.profiles[1].bindings[0].buttons[0]=="l2");
    session.detectedProfile=controls::ProfileId::PlayStation;session.document.config=s.working;Begin(s,session);Arm(s);s.row=kDisplayRows;Tap(s,Accept);assert(!s.saveRequested&&!s.message.empty());
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
    Draw(tallState,tallSession,surface,0);
    {bool red=false;for(unsigned y=390;y<420;++y)for(unsigned x=60;x<400;++x)red|=surface.pixels[y*surface.stride+x]==0xff9487fbu;assert(red);}  // the message, in red, on the help line
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
        // The reference table marks the buttons of the selected action: one band for a single binding, two for Spin (Square and R).
        {
            auto padSession=Session();UiState h;constexpr uint32_t kBand=0xff78301cu;   // the table's marker colour
            auto bands=[&](unsigned row,controls::ProfileId profile) {
                Begin(h,padSession);h.page=Page::Controls;h.working.profile=profile;h.row=row;Draw(h,padSession,surface,0);
                unsigned n=0;for(unsigned y=100;y<340;++y)for(unsigned x=590;x<928;++x)n+=surface.pixels[y*surface.stride+x]==kBand;return n;
            };
            for(unsigned p=0;p<2;++p) {
                const auto profile=static_cast<controls::ProfileId>(p);
                assert(bands(0,profile)==0&&bands(12,profile)==0);                      // the profile row and the aiming option mark no button
                const unsigned jump=bands(1,profile),spin=bands(3,profile);
                assert(jump>1000&&spin>jump*3/2&&spin<jump*5/2);
            }
        }
    }
    // Optional backdrop: null / unusable keeps the exact old gradient; a picture is drawn scaled, dimmed and legible.
    {
        auto bdSession=Session();UiState b;Begin(b,bdSession);std::vector<uint32_t> plain(960*544),other(plain.size());
        Draw(b,bdSession,{plain.data(),960,544,960},0);
        for(unsigned y=90;y<460;++y)for(unsigned x=0;x<32;++x)assert(plain[y*960+x]==backdrop_check::RGB(9+y/90,14+y/70,36+y/20));  // the gradient
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
            if(modal==Modal::None)for(unsigned y:{90u,100u,271u,272u,400u,460u})for(unsigned x:{0u,5u,31u})assert(tight[y*960+x]==backdrop_check::Expected(pic.data(),sz.w,sz.h,x,y));
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
        session.fonts=fonts?synthetic.view():FontSet{};Begin(s,session);s.row=RowMotion;s.working.reducedMotion=on;
        DrawAudit noteAudit;DrawChecked(s,session,{first.data(),960,544,960},0,noteAudit);
        assert(noteAudit.minX>=32&&noteAudit.minY>=32&&noteAudit.maxX<=928&&noteAudit.maxY<=512);
        // The test font is wider than the game's and truncates labels elsewhere already; the note adds no truncated run to what row 2 shows.
        UiState other=s;other.row=RowLanguage;DrawAudit otherAudit;DrawChecked(other,session,{second.data(),960,544,960},0,otherAudit);
        assert(fonts||(noteAudit.truncatedRuns==0&&otherAudit.truncatedRuns==0));
        size_t noteDiff=0;for(size_t y=392;y<436;++y)for(size_t x=60;x<900;++x)noteDiff+=first[y*960+x]!=second[y*960+x];assert(noteDiff>100);  // row 3 has its own note line
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
        for(unsigned row=0;row<kDisplayRows+choice;++row){script.keys.push_back(Down);script.keys.push_back(0);}
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
    // Row texts: every row of both pages has a label, a value and help; the HD row says when the pack is missing; no help line is cut with the built-in
    // font (the game's own fonts are measured by the preview tool).
    {
        auto vs=Session();UiState v;Begin(v,vs);
        for(unsigned row=0;row<kDisplayRows;++row) {RowView rv;DisplayRowView(v,row,rv);assert(rv.label[0]&&rv.value[0]&&rv.help1[0]);}
        RowView hd;DisplayRowView(v,RowHdTextures,hd);assert(std::string(hd.value)=="On");vs.hdtexPack=false;DisplayRowView(v,RowHdTextures,hd);assert(std::string(hd.value).find("not installed")!=std::string::npos&&std::string(hd.help2).find("not in")!=std::string::npos);vs.hdtexPack=true;
        RowView preset;DisplayRowView(v,RowPreset,preset);assert(std::string(preset.value)=="Best picture");v.working.sharpness=Sharpness::Strong;DisplayRowView(v,RowPreset,preset);assert(std::string(preset.value)=="Custom");
        for(unsigned row=0;row<=vs.controls->actionCount+1;++row) {RowView rv;ControlsRowView(v,row,rv);assert(rv.label[0]&&rv.help1[0]&&rv.help2[0]&&(rv.value[0]||(row>0&&row<=vs.controls->actionCount)));}   // (an action's buttons are drawn from its binding)
        std::vector<uint32_t> frame(960*544);
        for(auto page:{Page::Display,Page::Controls})for(unsigned profile=0;profile<2;++profile)for(unsigned row=0;row<RowCount(v);++row) {
            Begin(v,vs);v.page=page;v.row=row;v.working.profile=static_cast<controls::ProfileId>(profile);
            DrawAudit rowAudit;DrawChecked(v,vs,{frame.data(),960,544,960},0,rowAudit);
            assert(rowAudit.truncatedRuns==0&&rowAudit.minX>=32&&rowAudit.minY>=32&&rowAudit.maxX<=928&&rowAudit.maxY<=512);
        }
    }
    // The unchanging picture kept in a scratch layer (backdrop, header band, heading): every frame is the same with it as without it,
    // across page changes, fonts and a backdrop, and the audit sees the heading on a frame that only copies the layer.
    {
        std::vector<uint32_t> layer(960*544),plain(960*544),layered(960*544),pic(480*272);
        for(size_t i=0;i<pic.size();++i)pic[i]=0xff000000u|static_cast<uint32_t>((i*2654435761u)>>8);
        for(bool backdrop:{false,true})for(bool fonts:{false,true}) {
            auto ls=Session();ls.fonts=fonts?synthetic.view():FontSet{};if(backdrop) {ls.backdrop=pic.data();ls.backdropW=480;ls.backdropH=272;}
            ls.layer=layer.data();UiState st;Begin(st,ls);
            const struct {Page page;unsigned row;uint64_t now;} steps[]={{Page::Display,0,0},{Page::Display,0,777777},{Page::Display,7,5},{Page::Display,kDisplayRows+1,9},{Page::Controls,3,10},{Page::Controls,12,11},{Page::Display,12,12}};
            for(const auto& step:steps) {
                st.page=step.page;st.row=step.row;DrawAudit a,b;
                ls.layer=nullptr;DrawChecked(st,ls,{plain.data(),960,544,960},step.now,a);
                ls.layer=layer.data();DrawChecked(st,ls,{layered.data(),960,544,960},step.now,b);
                assert(plain==layered&&a.textRuns==b.textRuns&&a.minY==b.minY&&a.minX==b.minX&&a.maxX==b.maxX&&a.maxY==b.maxY);
                assert(ls.layerPage==static_cast<int>(step.page));
            }
        }
    }
    std::cout<<"ui: navigation, entry/disconnect rearm, capture/chord/conflicts, results, raster canaries and animation passed\n";
}
