#include "port/settings_backdrop.h"
#include "port/settings_ui.h"
#include "../tests/settings_backdrop_check.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
using namespace port::settings;
int main(int argc,char** argv) {
    const std::string directory=argc>1?argv[1]:"local/settings-preview";
    SettingsSession session{};session.mode=OpenMode::ColdStart;session.controls=&port::controls::GetControlApi();session.document.config=DefaultConfig(*session.controls);session.active=session.document.config;
    session.controller=port::controls::NominalCaps(port::controls::ProfileId::Handheld);session.features={true,true};session.languages.names={"English","French","German","Spanish","Italian"};
    FontBundle installedFonts;
    if(argc>2) {
        std::ifstream file(argv[2],std::ios::binary|std::ios::ate);if(!file||file.tellg()<0||static_cast<uint64_t>(file.tellg())>kFontArchiveLimit)return 2;
        std::vector<uint8_t> archive(static_cast<size_t>(file.tellg()));file.seekg(0);file.read(reinterpret_cast<char*>(archive.data()),static_cast<std::streamsize>(archive.size()));const char* error=nullptr;
        if(!file||!ParseGameFontArchive(archive.data(),archive.size(),installedFonts,error)){std::cerr<<"Local font archive unavailable: "<<error<<"\n";return 2;}
        session.fonts=installedFonts.view();
    }
    std::vector<uint32_t> pixels(960*544);std::ofstream report(directory+"/bounds.txt");
    // Where frames go: the plain frames in `directory`, backdrop frames in a subdirectory of their own, with their own report.
    std::string outDir=directory;std::ofstream* out=&report;bool legible=true;double worstContrast=1e9;
    unsigned frames=0;
    auto render=[&](const char* name,UiState state,uint64_t time=150000) {
        DrawAudit audit;DrawChecked(state,session,{pixels.data(),960,544,960},time,audit);++frames;
        if(audit.minX<32||audit.minY<32||audit.maxX>928||audit.maxY>512)return false;
        // No frame of the menu itself may cut a text with "..." (modal bodies and an error message are the exceptions).
        if(audit.truncatedRuns&&state.modal==Modal::None&&state.message.empty()){std::cerr<<name<<": "<<audit.truncatedRuns<<" truncated text run(s)\n";return false;}
        // With a backdrop, every opaque glyph pixel must keep contrast >= 4.5 against the picture behind it.
        if(session.backdrop&&state.modal==Modal::None) {
            const auto check=backdrop_check::CheckText(pixels,session.backdrop,session.backdropW,session.backdropH,4.5);
            worstContrast=std::min(worstContrast,check.worst);if(check.failures||check.textPixels<1000)legible=false;
        }
        std::ofstream file(outDir+"/"+name+".ppm",std::ios::binary);file<<"P6\n960 544\n255\n";
        for(auto pixel:pixels) {const char rgb[]={static_cast<char>(pixel&255),static_cast<char>((pixel>>8)&255),static_cast<char>((pixel>>16)&255)};file.write(rgb,3);}
        *out<<name<<": "<<audit.textRuns<<" text runs; bounds "<<audit.minX<<","<<audit.minY<<" to "<<audit.maxX<<","<<audit.maxY<<"; "<<audit.truncatedRuns<<" truncated\n";
        return file.good();
    };
    auto scenes=[&]()->bool {
    session.mode=OpenMode::ColdStart;session.document=Document{};session.document.config=DefaultConfig(*session.controls);session.active=session.document.config;
    session.features={true,true};session.overrides.clear();session.languages.names={"English","French","German","Spanish","Italian"};
    UiState state;Begin(state,session);
    if(!render("display-cold",state))return false;
    state.working.fpsLimit=30;state.working.autoQuality=AutoQuality::Disabled;state.working.language="French";state.row=RowLanguage;
    if(!render("display-edited",state))return false;
    // Every Display row focused, then every command: its help must stay inside the safe text bounds and not be truncated.
    {const auto saved=state.working;state.working=session.document.config;
     for(unsigned row=0;row<kDisplayRows+3;++row) {state.row=row;char name[40];std::snprintf(name,sizeof name,"display-row-%02u",row);if(!render(name,state))return false;}
     // The presets, and a row changed by hand.
     const char* presets[]={"best","balanced","performance"};
     for(unsigned i=0;i<3;++i) {state.working=session.document.config;ApplyPreset(state.working,static_cast<Preset>(i));state.row=RowPreset;char name[40];std::snprintf(name,sizeof name,"display-preset-%s",presets[i]);if(!render(name,state))return false;}
     state.working.sharpness=Sharpness::Strong;state.row=RowSharpness;if(!render("display-custom",state))return false;
     // The HD pack missing, and the scrolled-down list.
     state.working=session.document.config;session.hdtexPack=false;state.row=RowHdTextures;if(!render("display-hd-not-installed",state))return false;session.hdtexPack=true;
     state.working=session.document.config;state.row=RowLanguage;if(!render("display-scrolled",state))return false;
     state.working=saved;}
    // The Reduced motion row focused, off and on.
    state.row=RowMotion;if(!render("display-reduced-motion-row",state))return false;state.working.reducedMotion=true;if(!render("display-reduced-motion-row-on",state))return false;state.working.reducedMotion=false;state.row=RowLanguage;
    session.mode=OpenMode::Running;Begin(state,session);state.modal=Modal::Restart;
    if(!render("restart-cancel",state))return false;state.modalChoice=1;if(!render("restart-confirm",state))return false;
    state.modal=Modal::Discard;if(!render("discard",state))return false;
    state.modal=Modal::None;state.page=Page::Controls;state.row=3;state.bindingSlot=1;
    if(!render("controls-handheld",state))return false;
    state.working.profile=port::controls::ProfileId::PlayStation;if(!render("controls-playstation",state))return false;
    state.bindingSlot=0;state.row=4;if(!render("controls-playstation-shoulder-left",state))return false;
    state.row=6;if(!render("controls-playstation-start",state))return false;
    state.row=7;if(!render("controls-playstation-select",state))return false;
    state.row=11;if(!render("controls-scrolled",state))return false;
    for(unsigned row=0;row<=15;++row) {state.row=row;char name[40];std::snprintf(name,sizeof name,"controls-row-%02u",row);if(!render(name,state))return false;}
    state.row=11;
    {std::string conflict;auto caps=port::controls::NominalCaps(port::controls::ProfileId::PlayStation);
     if(session.controls->bind(state.working.profiles[1],"star_bits",0,"r2",true,caps,conflict)!=port::controls::BindResult::Applied)return false;
     state.row=2;if(!render("controls-playstation-r2",state))return false;
     state.row=12;if(!render("controls-aim-row",state))return false;state.working.mouseAim=true;if(!render("controls-aim-row-on",state))return false;state.working.mouseAim=false;}
    state.modal=Modal::Capture;state.action="jump";if(!render("capture",state))return false;
    state.candidate=port::controls::Circle;if(!render("capture-release",state))return false;
    state.modal=Modal::ButtonPicker;state.picker=6;if(!render("button-picker",state))return false;
    state.modal=Modal::Conflict;state.candidateButton="circle";state.conflictAction="star_bits";state.modalChoice=0;if(!render("conflict-cancel",state))return false;
    state.modalChoice=1;if(!render("conflict-swap",state))return false;
    state.modal=Modal::ResetProfile;if(!render("reset-profile",state))return false;
    state.modal=Modal::None;state.page=Page::Display;state.row=RowEffects;state.message="Settings replacement rename failed. Changes remain editable.";if(!render("save-failure",state))return false;
    session.document.editable=false;Begin(state,session);if(!render("newer-readonly",state))return false;
    session.document.editable=true;session.document.config.language="Unavailable language";Begin(state,session);state.row=RowLanguage;if(!render("language-unavailable",state))return false;
    session.document.config.language="";session.languages.names.clear();Begin(state,session);state.row=RowLanguage;if(!render("game-data-unavailable",state))return false;
    session.languages.names={"English","French","German","Spanish","Italian"};
    session.overrides={{"GXV_FPS_LIMIT","invalid","Existing picture-rate path"},{"GXV_AUTOQ","1","1"},{"GXV_AUTOQ_LEVEL","2","2"},{"GXV_AUTOQ_CYCLE","1","1"},{"PETARI_LANGUAGE","japanese","japanese"},{"GXV_REMAP","0","Remapping disabled"}};
    state.modal=Modal::Overrides;if(!render("developer-overrides",state))return false;
    session.document.config.language="";session.overrides.clear();session.features={false,false};session.document.config.fpsLimit=30;Begin(state,session);if(!render("capability-disabled",state))return false;
    session.features={true,true};session.document.config.fpsLimit=0;Begin(state,session);state.changedAt=1000000;
    if(!render("animation-0",state,1000000)||!render("animation-75",state,1075000)||!render("animation-150",state,1150000))return false;
    state.working.reducedMotion=true;if(!render("reduced-motion",state,10000000))return false;
    return true;
    };
    if(!scenes())return 1;
    std::cout<<"preview: "<<frames<<" native-size PPM frames and safe text bounds generated\n";
    // Backdrop frames: a synthetic picture, then a title capture when one is given as the third argument (read only, loaded
    // through the same downsampler as the device; the package carries none, the game makes its own on the console). Each full scene set, with every text pixel's contrast against the picture checked.
    std::vector<uint32_t> synthetic(kBackdropPixels);
    for(unsigned y=0;y<kBackdropHeight;++y)for(unsigned x=0;x<kBackdropWidth;++x) {
        const unsigned v=((x/8+y/8)&1)?255:(x*255/kBackdropWidth);synthetic[y*kBackdropWidth+x]=0xff000000u|(v<<16)|((255-v)<<8)|(y*255/kBackdropHeight);
    }
    std::vector<uint32_t> real;
    const char* captureEnv=std::getenv("SETTINGS_BACKDROP_CAPTURE");  // or the third argument
    const std::string realPath=argc>3?argv[3]:(captureEnv?captureEnv:"");
    {
        std::ifstream file(realPath,std::ios::binary);std::vector<uint8_t> bytes(kBackdropSourceBytes+1);
        if(file){file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            if(static_cast<size_t>(file.gcount())==kBackdropSourceBytes) {
                real.assign(kBackdropPixels,0);
                for(unsigned row=0;row<kBackdropSourceHeight;row+=kBackdropChunkRows)DownsampleBackdropRows(bytes.data()+size_t(row)*kBackdropSourceWidth*4,row,kBackdropChunkRows,real.data());
                CleanBackdropPrompt(real.data());
            }}
    }
    const struct {const char* name;std::vector<uint32_t>* pixels;} sets[]={{"backdrop-synthetic",&synthetic},{"backdrop-real",&real}};
    for(const auto& set:sets) {
        if(set.pixels->empty()){std::cout<<"preview: "<<set.name<<" skipped (no picture at "<<realPath<<")\n";continue;}
        outDir=directory+"/"+set.name;std::filesystem::create_directories(outDir);
        std::ofstream backdropReport(outDir+"/bounds.txt");out=&backdropReport;
        session.backdrop=set.pixels->data();session.backdropW=kBackdropWidth;session.backdropH=kBackdropHeight;legible=true;worstContrast=1e9;
        if(!scenes()||!legible){std::cerr<<"preview: "<<set.name<<" frames failed bounds or text contrast\n";return 1;}
        char line[160];std::snprintf(line,sizeof line,"preview: %s frames generated; worst text contrast %.2f:1 (minimum 4.5)\n",set.name,worstContrast);std::cout<<line;
    }
    session.backdrop=nullptr;
}
