// Time of one Settings frame (Draw) on the host at -O2: the busiest Display and Controls pages, with the title backdrop, with and without
// the scratch layer, with the built-in font and (when SETTINGS_FONT_ARCHIVE names the game's Font.arc) the game's own fonts.
// Fails when a frame with the layer takes more than 4 ms on average over the best of five batches.
#include "port/settings_backdrop.h"
#include "port/settings_ui.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
using namespace port::settings;
int main() {
    std::vector<uint32_t> pic(kBackdropPixels),layer(960*544),frame(960*544);
    for(size_t i=0;i<pic.size();++i)pic[i]=0xff000000u|static_cast<uint32_t>((i*2654435761u)>>8);
    FontBundle bundle;bool game=false;
    if(const char* path=std::getenv("SETTINGS_FONT_ARCHIVE")) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(file&&file.tellg()>0&&static_cast<uint64_t>(file.tellg())<=kFontArchiveLimit) {
            std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            const char* error=nullptr;game=ParseGameFontArchive(bytes.data(),bytes.size(),bundle,error);
        }
    }
    bool ok=true;
    for(const bool fonts:{false,true}) {
        if(fonts&&!game)continue;
        for(const bool useLayer:{false,true})for(const Page page:{Page::Display,Page::Controls}) {
            SettingsSession s{};s.mode=OpenMode::Running;s.controls=&port::controls::GetControlApi();s.document.config=DefaultConfig(*s.controls);s.active=s.document.config;
            s.controller=port::controls::NominalCaps(port::controls::ProfileId::Handheld);s.features={true,true};s.languages.names={"English","French","German"};
            s.backdrop=pic.data();s.backdropW=kBackdropWidth;s.backdropH=kBackdropHeight;if(fonts)s.fonts=bundle.view();if(useLayer)s.layer=layer.data();
            UiState st;Begin(st,s);st.page=page;st.row=page==Page::Display?5:4;st.message="";
            Draw(st,s,{frame.data(),960,544,960},0); // builds the layer
            double best=1e9;
            for(int batch=0;batch<5;++batch) {
                const auto t0=std::chrono::steady_clock::now();
                for(int i=0;i<100;++i)Draw(st,s,{frame.data(),960,544,960},static_cast<uint64_t>(i)*33333);
                const auto t1=std::chrono::steady_clock::now();
                best=std::min(best,std::chrono::duration<double,std::milli>(t1-t0).count()/100.0);
            }
            std::printf("render: %s fonts, %s, %s page: %.3f ms a frame\n",fonts?"game":"built-in",useLayer?"with layer":"no layer",page==Page::Display?"Display":"Controls",best);
            if(useLayer&&best>4.0)ok=false;
        }
    }
    if(!ok){std::cerr<<"render: a frame took more than 4 ms\n";return 1;}
    std::cout<<"render: every frame with the layer is within 4 ms\n";
}
