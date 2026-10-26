#include "port/settings_ui.h"
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
    auto render=[&](const char* name,UiState state,uint64_t time=150000) {
        DrawAudit audit;DrawChecked(state,session,{pixels.data(),960,544,960},time,audit);
        if(audit.minX<32||audit.minY<32||audit.maxX>928||audit.maxY>512)return false;
        std::ofstream file(directory+"/"+name+".ppm",std::ios::binary);file<<"P6\n960 544\n255\n";
        for(auto pixel:pixels) {const char rgb[]={static_cast<char>(pixel&255),static_cast<char>((pixel>>8)&255),static_cast<char>((pixel>>16)&255)};file.write(rgb,3);}
        report<<name<<": "<<audit.textRuns<<" text runs; bounds "<<audit.minX<<","<<audit.minY<<" to "<<audit.maxX<<","<<audit.maxY<<"; "<<audit.truncatedRuns<<" truncated\n";
        return file.good();
    };
    UiState state;Begin(state,session);
    if(!render("display-cold",state))return 1;
    state.working.fpsLimit=30;state.working.autoQuality=AutoQuality::Enabled;state.working.language="French";state.row=2;
    if(!render("display-edited",state))return 1;
    session.mode=OpenMode::Running;Begin(state,session);state.modal=Modal::Restart;
    if(!render("restart-cancel",state))return 1;state.modalChoice=1;if(!render("restart-confirm",state))return 1;
    state.modal=Modal::Discard;if(!render("discard",state))return 1;
    state.modal=Modal::None;state.page=Page::Controls;state.row=3;state.bindingSlot=1;
    if(!render("controls-handheld",state))return 1;
    state.working.profile=port::controls::ProfileId::PlayStation;if(!render("controls-playstation",state))return 1;
    state.bindingSlot=0;state.row=4;if(!render("controls-playstation-shoulder-left",state))return 1;
    state.row=6;if(!render("controls-playstation-start",state))return 1;
    state.row=7;if(!render("controls-playstation-select",state))return 1;
    state.row=11;if(!render("controls-scrolled",state))return 1;
    state.modal=Modal::Capture;state.action="jump";if(!render("capture",state))return 1;
    state.candidate=port::controls::Circle;if(!render("capture-release",state))return 1;
    state.modal=Modal::ButtonPicker;state.picker=6;if(!render("button-picker",state))return 1;
    state.modal=Modal::Conflict;state.candidateButton="circle";state.conflictAction="star_bits";state.modalChoice=0;if(!render("conflict-cancel",state))return 1;
    state.modalChoice=1;if(!render("conflict-swap",state))return 1;
    state.modal=Modal::ResetProfile;if(!render("reset-profile",state))return 1;
    state.modal=Modal::None;state.page=Page::Display;state.row=4;state.message="Settings replacement rename failed. Changes remain editable.";if(!render("save-failure",state))return 1;
    session.document.editable=false;Begin(state,session);if(!render("newer-readonly",state))return 1;
    session.document.editable=true;session.document.config.language="Unavailable language";Begin(state,session);state.row=2;if(!render("language-unavailable",state))return 1;
    session.document.config.language="";session.languages.names.clear();Begin(state,session);state.row=2;if(!render("game-data-unavailable",state))return 1;
    session.languages.names={"English","French","German","Spanish","Italian"};
    session.overrides={{"GXV_FPS_LIMIT","invalid","Existing picture-rate path"},{"GXV_AUTOQ","1","1"},{"GXV_AUTOQ_LEVEL","2","2"},{"GXV_AUTOQ_CYCLE","1","1"},{"PETARI_LANGUAGE","japanese","japanese"},{"GXV_REMAP","0","Remapping disabled"}};
    state.modal=Modal::Overrides;if(!render("developer-overrides",state))return 1;
    session.document.config.language="";session.overrides.clear();session.features={false,false};session.document.config.fpsLimit=30;Begin(state,session);if(!render("capability-disabled",state))return 1;
    session.features={true,true};session.document.config.fpsLimit=0;Begin(state,session);state.changedAt=1000000;
    if(!render("animation-0",state,1000000)||!render("animation-75",state,1075000)||!render("animation-150",state,1150000))return 1;
    state.working.reducedMotion=true;if(!render("reduced-motion",state,10000000))return 1;
    std::cout<<"preview: 27 native-size PPM frames and safe text bounds generated\n";
}
