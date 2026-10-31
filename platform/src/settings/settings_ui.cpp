#include "port/settings_ui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace port::settings {
namespace {
ExitIntent Safe(const SettingsSession& session) {return session.mode==OpenMode::Running?ExitIntent::Return:ExitIntent::Exit;}
unsigned Profile(const UiState& s) {return static_cast<unsigned>(s.working.profile);}
controls::ControllerCaps EditCaps(const UiState& s) {
    auto caps=controls::NominalCaps(s.working.profile);
    if(s.controller.connected&&s.detectedProfile==s.working.profile) {caps=s.controller;caps.availableButtons&=controls::NominalCaps(s.working.profile).availableButtons;}
    return caps;
}
// Controls page: the profile row, one row per action, then the mouse-style aiming option; Display page: kDisplayRows settings.
unsigned Commands(const UiState& s) {return s.page==Page::Display?kDisplayRows:static_cast<unsigned>(s.session->controls->actionCount)+2;}
bool AimRow(const UiState& s) {return s.page==Page::Controls&&s.row==s.session->controls->actionCount+1;}
void ModalOpen(UiState& s,Modal modal) {s.modal=modal;s.modalChoice=0;s.captureArmed=false;s.candidate=0;s.previousMenu=0;s.armed=false;s.repeatKey=0;}
void Finish(UiState& s) {s.result={Safe(*s.session),false};s.done=true;}
void Cancel(UiState& s) {if(Dirty(s))ModalOpen(s,Modal::Discard);else Finish(s);}
void Save(UiState& s,ExitIntent intent) {
    if(!s.session->document.editable) {s.message="Newer settings file is read-only";return;}
    std::string error;
    for(unsigned i=0;i<2;++i)if(!s.session->controls->validate(s.working.profiles[i],static_cast<controls::ProfileId>(i),controls::NominalCaps(static_cast<controls::ProfileId>(i)),error)) {s.message=error;return;}
    if(s.controller.connected&&s.detectedProfile==s.working.profile&&!s.session->controls->validate(s.working.profiles[Profile(s)],s.working.profile,EditCaps(s),error)) {s.message=error;return;}
    s.pendingIntent=intent;s.saveRequested=true;
}
std::vector<const controls::ButtonInfo*> Buttons(const UiState& s) {
    std::vector<const controls::ButtonInfo*> buttons;
    const auto caps=EditCaps(s);
    for(size_t i=0;i<s.session->controls->buttonCount;++i)if(caps.availableButtons&s.session->controls->buttons[i].mask)buttons.push_back(&s.session->controls->buttons[i]);
    return buttons;
}
void Bind(UiState& s,const std::string& button,bool swap) {
    auto proposed=s.working.profiles[Profile(s)];
    auto result=s.session->controls->bind(proposed,s.action.c_str(),s.bindingSlot,button.c_str(),swap,EditCaps(s),s.conflictAction);
    if(result==controls::BindResult::Applied) {s.working.profiles[Profile(s)]=std::move(proposed);s.modal=Modal::None;s.message="Control saved in working profile";s.armed=false;}
    else if(result==controls::BindResult::Conflict) {s.candidateButton=button;ModalOpen(s,Modal::Conflict);}
    else {s.message=result==controls::BindResult::RequiredActionLost?"Every required action needs a binding":"Button unavailable in this profile";s.modal=Modal::None;s.armed=false;}
}
void Capture(UiState& s,const UiInput& input) {
    const uint64_t chord=controls::Start|controls::Select;
    if((input.caps.availableButtons&chord)==chord&&(input.physicalHeld&chord)==chord) {
        s.modal=Modal::None;s.candidate=0;s.captureArmed=false;s.armed=false;s.message="Binding unchanged";return;
    }
    if(!s.captureArmed) {if(!input.physicalHeld&&!input.menuHeld)s.captureArmed=true;return;}
    const auto held=input.physicalHeld;
    if(held&&(held&(held-1))) {s.candidate=0;s.captureArmed=false;s.message="One button at a time";return;}
    if(held) {
        if(s.candidate&&held!=s.candidate) {s.candidate=0;s.captureArmed=false;s.message="One button at a time";return;}
        s.candidate=held;return;
    }
    if(s.candidate) {
        for(const auto* b:Buttons(s))if(b->mask==s.candidate) {
            s.candidate=0;Bind(s,b->id,false);
            if(s.modal==Modal::Conflict&&!input.menuHeld)s.armed=true;
            return;
        }
        s.message="Button unavailable in this profile";s.candidate=0;s.captureArmed=false;
    }
}
void Change(UiState& s,int direction) {
    if(s.page==Page::Controls) {
        if(s.row==0) {s.working.profile=s.working.profile==controls::ProfileId::Handheld?controls::ProfileId::PlayStation:controls::ProfileId::Handheld;s.working.profileChosen=true;s.bindingSlot=0;s.armed=false;}
        else if(AimRow(s))s.working.mouseAim=!s.working.mouseAim;
        else if(s.row<=s.session->controls->actionCount) {
            const auto& action=s.session->controls->actions[s.row-1];if(action.maxBindings>1)s.bindingSlot=1-s.bindingSlot;
        }
        return;
    }
    switch(s.row) {
        case RowPreset: {
            // Best picture, Balanced, Performance; Custom is only ever read from the rows. From Custom, Right starts at the first.
            const auto now=PresetOf(s.working);int index=now==Preset::Custom?(direction>0?-1:3):static_cast<int>(now);
            ApplyPreset(s.working,static_cast<Preset>((index+direction+3)%3));break;
        }
        case RowRate: {
            // Right moves along the screen (Locked 30, Up to 60) and wraps; the locked 30 needs the driver capability.
            if(!s.session->features.fixed30&&(s.working.fpsLimit==30||s.working.fpsLimit==kSteady30)) {s.message="Fixed 30 FPS unavailable in this build";break;}
            if(s.working.fpsLimit==kSteady30)s.working.fpsLimit=30;
            std::vector<int> rates{0};if(s.session->features.fixed30)rates.push_back(30);
            auto at=std::find(rates.begin(),rates.end(),s.working.fpsLimit);const int n=static_cast<int>(rates.size());
            const int index=at==rates.end()?0:static_cast<int>(at-rates.begin());
            s.working.fpsLimit=rates[static_cast<size_t>((index+direction+n)%n)];break;
        }
        // On and the default are one choice on the console; turning it back on returns to the default (no switch is forced).
        case RowDynamic: s.working.autoQuality=s.working.autoQuality==AutoQuality::Disabled?AutoQuality::Default:AutoQuality::Disabled;break;
        case RowSharper: s.working.aboveNative=!s.working.aboveNative;break;
        case RowMario: s.working.prioritizeMario=!s.working.prioritizeMario;break;
        case RowAntiAliasing: s.working.antiAliasing=s.working.antiAliasing==AntiAliasing::Auto?AntiAliasing::Off:AntiAliasing::Auto;break;
        case RowSharpness: s.working.sharpness=static_cast<Sharpness>((static_cast<int>(s.working.sharpness)+direction+3)%3);break;
        case RowHdTextures: s.working.hdTextures=!s.working.hdTextures;break;
        case RowEffects: s.working.effects=s.working.effects==EffectsQuality::Adaptive?EffectsQuality::Full:EffectsQuality::Adaptive;break;
        case RowDepthOfField: s.working.depthOfField=!s.working.depthOfField;break;
        case RowCorePlan: s.working.coreHelp=!s.working.coreHelp;break;
        case RowMotion: s.working.reducedMotion=!s.working.reducedMotion;break;
        case RowLanguage: {
            if(s.session->languages.names.empty()) {s.message="Game language unavailable until game data is found";break;}
            std::vector<std::string> choices{""};choices.insert(choices.end(),s.session->languages.names.begin(),s.session->languages.names.end());
            auto at=std::find(choices.begin(),choices.end(),s.working.language);int index=at==choices.end()?0:static_cast<int>(at-choices.begin());
            index=(index+direction+static_cast<int>(choices.size()))%static_cast<int>(choices.size());s.working.language=choices[static_cast<size_t>(index)];break;
        }
        default:break;
    }
}
}
unsigned RowCount(const UiState& s) {return Commands(s)+3;}
unsigned SettingRows(const UiState& s) {return Commands(s);}
bool Dirty(const UiState& s) {return !Equal(s.working,s.session->document.config);}
void Begin(UiState& s,const SettingsSession& session) {
    s=UiState{};s.session=&session;s.working=session.document.config;s.connected=session.controller.connected;
    s.controller=session.controller;
    s.detectedProfile=session.detectedProfile;
    s.result={Safe(session),false};s.pendingIntent=Safe(session);
    s.message=session.statusMessage;
    if(!session.document.editable)s.message="Newer settings file is read-only";
}
void Step(UiState& s,const UiInput& input,uint64_t now) {
    if(s.done||s.saveRequested||!s.session)return;
    const bool capabilityChanged=s.controller.availableButtons!=input.caps.availableButtons||s.detectedProfile!=input.detectedProfile;
    s.controller=input.caps;
    s.detectedProfile=input.detectedProfile;
    if(input.caps.connected!=s.connected||capabilityChanged) {
        s.connected=input.caps.connected;s.armed=false;s.captureArmed=false;s.candidate=0;s.repeatKey=0;s.previousMenu=0;
        if(s.modal==Modal::Capture||s.modal==Modal::Conflict||s.modal==Modal::ButtonPicker)s.modal=Modal::None;
        s.message=!s.connected?"Controller disconnected":capabilityChanged?"Controller capabilities changed":"Controller connected";
    }
    if(!s.connected)return;
    if(s.modal==Modal::Capture) {Capture(s,input);return;}
    if(!s.armed) {
        if(!input.menuHeld&&!input.physicalHeld) {s.armed=true;s.previousMenu=0;s.repeatKey=0;}
        return;
    }
    const uint32_t held=input.menuHeld, pressed=held&~s.previousMenu;s.previousMenu=held;
    uint32_t event=pressed;
    const uint32_t direction=held&(Up|Down|Left|Right);
    if(direction&&!(direction&(direction-1))) {
        if(direction!=s.repeatKey) {s.repeatKey=direction;s.repeatAt=now+350000;}
        else if(now>=s.repeatAt) {event|=direction;s.repeatAt=now+90000;}
    } else s.repeatKey=0;
    if(!event)return;
    if(s.modal!=Modal::None) {
        if(event&Back) {s.modal=Modal::None;s.armed=false;return;}
        if(s.modal==Modal::Overrides) {
            if(event&Accept){s.modal=Modal::None;s.armed=false;}
            return;
        }
        if(s.modal==Modal::ButtonPicker) {
            const auto buttons=Buttons(s);if(buttons.empty())return;
            const unsigned optional=s.bindingSlot>0?1:0,count=static_cast<unsigned>(buttons.size())+optional;
            if(event&(Up|Left))s.picker=(s.picker+count-1)%count;
            if(event&(Down|Right))s.picker=(s.picker+1)%count;
            const unsigned index=s.picker%count;
            if(event&Accept)Bind(s,optional&&index==0?"":buttons[index-optional]->id,false);
            return;
        }
        if(event&(Up|Down|Left|Right))s.modalChoice=1-s.modalChoice;
        if(event&Accept) {
            const auto modal=s.modal;
            if(!s.modalChoice) {s.modal=Modal::None;s.armed=false;return;}
            s.modal=Modal::None;s.armed=false;
            if(modal==Modal::Discard)Finish(s);
            if(modal==Modal::Restart)Save(s,ExitIntent::Restart);
            if(modal==Modal::Conflict)Bind(s,s.candidateButton,true);
            if(modal==Modal::ResetProfile) {s.working.profiles[Profile(s)]=s.session->controls->defaults(s.working.profile);s.message="Current profile reset";}
        }
        return;
    }
    if(event&Back) {Cancel(s);return;}
    if((event&Reset)&&s.page==Page::Display&&!s.session->overrides.empty()) {ModalOpen(s,Modal::Overrides);return;}
    if(event&(PreviousPage|NextPage)) {s.page=s.page==Page::Display?Page::Controls:Page::Display;s.row=0;s.bindingSlot=0;s.changedAt=now;return;}
    if(event&Reset&&s.page==Page::Controls) {
        if(!s.session->features.remap)s.message="Remapping unavailable in this build";
        else if(AimRow(s)) return;
        else if(s.row>0&&s.row<=s.session->controls->actionCount) {s.action=s.session->controls->actions[s.row-1].id;s.picker=0;ModalOpen(s,Modal::ButtonPicker);}
        else ModalOpen(s,Modal::ResetProfile);
        return;
    }
    if(event&Up) {s.row=(s.row+RowCount(s)-1)%RowCount(s);s.bindingSlot=0;s.changedAt=now;}
    if(event&Down) {s.row=(s.row+1)%RowCount(s);s.bindingSlot=0;s.changedAt=now;}
    if(event&(Left|Right)) {
        // The three commands sit side by side: Left and Right move between them (Up and Down still walk every row).
        if(s.row>=Commands(s)) {const unsigned at=s.row-Commands(s);s.row=Commands(s)+(at+(event&Left?2:1))%3;s.changedAt=now;}
        else Change(s,event&Left?-1:1);
    }
    if(event&Accept) {
        const auto commands=Commands(s);
        if(s.row==commands)Save(s,s.session->mode==OpenMode::Running?ExitIntent::Return:ExitIntent::Play);
        else if(s.row==commands+1) {if(s.session->mode==OpenMode::Running)ModalOpen(s,Modal::Restart);else Save(s,ExitIntent::Exit);}
        else if(s.row==commands+2)Cancel(s);
        else if(s.page==Page::Display||s.row==0||AimRow(s))Change(s,1);
        else if(s.session->features.remap) {
            s.action=s.session->controls->actions[s.row-1].id;s.message.clear();
            const auto chord=controls::Start|controls::Select;
            ModalOpen(s,(input.caps.availableButtons&chord)==chord?Modal::Capture:Modal::ButtonPicker);s.picker=0;
        }
        else s.message="Remapping unavailable in this build";
    }
}
RunResult Run(SettingsSession& session) {
    const RunResult safe{Safe(session),false};
    if(!session.controls||!session.surface.pixels||session.surface.width!=960||session.surface.height!=544||session.surface.stride<960||!session.host.poll||!session.host.present||!session.host.nowUs||!session.host.waitUntilUs)return safe;
    UiState state;Begin(state,session);
    while(!state.done) {
        const uint64_t now=session.host.nowUs(session.host.context);UiInput input{};
        if(!session.host.poll(session.host.context,input))return safe;
        Step(state,input,now);Draw(state,session,session.surface,now);
        if(!session.host.present(session.host.context,session.surface))return safe;
        if(state.saveRequested) {
            state.saveRequested=false;Document saved;
            if(!session.files||!session.settingsPath)state.message="Settings storage unavailable";
            else {
                auto result=SaveDocument(session.settingsPath,session.document,state.working,*session.controls,*session.files,saved);
                if(result.status==StoreStatus::Ok) {session.document=std::move(saved);state.result={state.pendingIntent,true};state.done=true;}
                else state.message=result.message;
            }
        }
        if(!state.done)session.host.waitUntilUs(session.host.context,now+33333);
    }
    return state.result;
}

namespace {
void Put(char (&to)[72],const char* text) {std::snprintf(to,sizeof to,"%s",text);}
const char* OnOff(bool on) {return on?"On":"Off";}
struct ActionHelp {const char* id; const char* text;};
constexpr ActionHelp kActionHelp[]={
    {"jump","Jump (hold for a higher jump); also confirms in menus and dialogue."},
    {"star_bits","Shoots Star Bits at the pointer, and grabs what the pointer is on."},
    {"spin","Spin attack (the Wii Remote shake). A second button can spin too."},
    {"crouch","Crouch; in mid-air a ground pound; with Jump, backflips and long jumps."},
    {"camera_centre","Puts the camera behind Mario; hold it for the first-person view."},
    {"pause","Pauses the game, and confirms on the title screen."},
    {"minus","The Wii Remote's Minus: recalibrates the Star Pointer."},
    {"camera_up","Moves the camera up, and moves up in menus."},
    {"camera_down","Moves the camera down, and moves down in menus."},
    {"camera_left","Turns the camera left, and moves left in menus."},
    {"camera_right","Turns the camera right, and moves right in menus."}};
}
void DisplayRowView(const UiState& s,unsigned row,RowView& v) {
    const auto& c=s.working;v=RowView{};
    switch(row) {
        case RowPreset: {
            v.label="Preset";const auto p=PresetOf(c);
            Put(v.value,p==Preset::BestPicture?"Best picture":p==Preset::Balanced?"Balanced":p==Preset::Performance?"Performance":"Custom");
            v.help1="Sets dynamic resolution, Sharper, Mario, anti-aliasing, sharpness, effects and depth of field together.";
            v.help2="Change any of them and this reads Custom. Picture rate, HD textures, core plan and language are not in a preset.";break;
        }
        case RowRate:
            v.label="Picture rate";Put(v.value,(c.fpsLimit==30||c.fpsLimit==kSteady30)?(s.session->features.fixed30?"Locked 30":"Locked 30 (unavailable here)"):"Up to 60");
            if(!s.session->features.fixed30&&(c.fpsLimit==30||c.fpsLimit==kSteady30)) {v.help1="Locked 30 is saved, but this build cannot hold it. The current rate is unchanged.";v.help2="";}
            else if(c.fpsLimit!=0) {v.help1="Locked 30: an even 30 FPS, every frame, at full effects.";v.help2="Steadier than a rate that wanders, and kinder to the battery.";}
            else {v.help1="Up to 60: as fast as each scene allows, with 30 as the floor.";v.help2="Scenes that cannot reach 60 run at 30 (or between) rather than dropping in size first.";}
            break;
        case RowDynamic:
            v.label="Dynamic resolution";Put(v.value,OnOff(c.autoQuality!=AutoQuality::Disabled));
            v.help1="Lets the picture size follow the GPU: sharper when there is room, smaller only to hold 30 FPS.";
            v.help2="Off keeps the game's own 640x456 picture all the time; Sharper, Mario and Anti-aliasing then do nothing.";break;
        case RowSharper:
            v.label="Sharper when possible";Put(v.value,OnOff(c.aboveNative));
            v.help1="On: when the GPU has room the picture grows up to the screen's own 960x544, then smooths edges.";
            v.help2="Off: never above the game's 640x456 picture. Balanced and Performance turn this off.";break;
        case RowMario:
            v.label="Prioritize Mario";Put(v.value,OnOff(c.prioritizeMario));
            v.help1="On: under load, effects, background textures and picture size give way before Mario's quality,";
            v.help2="and Mario and Luigi get their own edge smoothing when the picture shrinks. Off: no Mario-first ladder.";break;
        case RowAntiAliasing:
            v.label="Anti-aliasing";Put(v.value,c.antiAliasing==AntiAliasing::Auto?"Auto":"Off");
            v.help1="Auto: smooths jagged edges with 2x or 4x multisampling when the GPU has room (needs Sharper when possible).";
            v.help2="Off: no multisampling; the picture then tops out at 960x544 without it.";break;
        case RowSharpness:
            v.label="Sharpness";Put(v.value,c.sharpness==Sharpness::Auto?"Auto":c.sharpness==Sharpness::Off?"Off":"Strong");
            v.help1="Sharpens the picture where it is stretched to the screen. Auto follows the stretch (none at 960x544).";
            v.help2="Strong always sharpens more, at a small extra GPU cost. Off is a plain scale.";break;
        case RowHdTextures:
            v.label="HD textures";std::snprintf(v.value,sizeof v.value,"%s%s",OnOff(c.hdTextures),s.session->hdtexPack?"":"  (not installed)");
            v.help1="Sharper textures for Mario, the main characters and key items, from the HD pack (hdtex.pack).";
            v.help2=s.session->hdtexPack?"On only matters when the pack is installed; Off uses the game's own textures.":"The pack is not in ux0:data/smg, so there is nothing to turn on yet.";break;
        case RowEffects:
            v.label="Effects quality";Put(v.value,c.effects==EffectsQuality::Adaptive?"Adaptive":"Full");
            v.help1="Adaptive: effects and blur are drawn at half size only while a scene would drop under 30 FPS.";
            v.help2="Full: they are never reduced, so under load the picture size drops instead.";break;
        case RowDepthOfField:
            v.label="Depth of field";Put(v.value,c.depthOfField?"On (as game)":"Off");
            v.help1="On: the game's own depth-of-field blur. Off: skips that full-screen blur pass, which is cheaper,";
            v.help2="but backgrounds stay sharp in the scenes where the game would blur them.";break;
        case RowCorePlan:
            v.label="Core plan";Put(v.value,c.coreHelp?"Auto":"Off");
            v.help1="Auto: disc read-ahead and a movie decoder may use the system core when the console allows it.";
            v.help2="Off: they stay on cores 1 and 2 as before. Try Off only if one console stutters while loading.";break;
        case RowMotion:
            v.label="Reduced motion";Put(v.value,OnOff(c.reducedMotion));
            v.help1="Less camera shake and screen blur; short bright flashes are softened.";
            v.help2="Physics, timing and the camera's pose are not changed.";break;
        case RowLanguage:
            v.label="Game language";
            if(c.language.empty())Put(v.value,s.session->languages.names.empty()?"Game data unavailable":"Disc default");
            else {std::snprintf(v.value,sizeof v.value,"%.40s%s",c.language.c_str(),std::find(s.session->languages.names.begin(),s.session->languages.names.end(),c.language)==s.session->languages.names.end()?" (unavailable)":"");}
            v.help1="The language of the game's text. Disc default follows the language the game picks itself.";
            v.help2="A language the disc does not have cannot be chosen; a saved one that is missing is ignored.";break;
        default:break;
    }
}
void ControlsRowView(const UiState& s,unsigned row,RowView& v) {
    v=RowView{};const auto& api=*s.session->controls;
    if(row==0) {
        v.label="Profile";Put(v.value,s.working.profile==controls::ProfileId::Handheld?"Handheld":"PlayStation");
        v.help1="Handheld uses the Vita's own buttons; PlayStation suits a DS3 or DS4 on a PS TV.";
        v.help2="Each profile keeps its own list. Left or Right switches the profile.";return;
    }
    if(row==api.actionCount+1) {
        v.label="Mouse-style aiming";Put(v.value,OnOff(s.working.mouseAim));
        v.help1="Right stick moves the Star Pointer by speed, like a mouse, with acceleration.";
        v.help2="Off: the 1.0 feel, where the stick position sets the pointer's speed in a straight line.";return;
    }
    if(row>api.actionCount)return;
    const auto& action=api.actions[row-1];v.label=action.label;v.help1=action.label;
    for(const auto& h:kActionHelp)if(!std::strcmp(h.id,action.id))v.help1=h.text;
    v.help2=action.maxBindings>1?"Two slots: Left or Right picks the slot, Cross changes it, Triangle opens the button list.":"Cross waits for a button press; Triangle opens a list of the buttons this profile has.";
}
void CommandView(const UiState& s,unsigned index,const char*& label,const char*& help1,const char*& help2) {
    const bool running=s.session->mode==OpenMode::Running;
    if(index==0) {
        label=running?"Save for Next Launch and Return":"Apply and Play";
        help1=running?"Saves these choices and goes back to the game. The current game keeps its active settings until restart.":"Saves these choices and starts the game with them.";
        help2="Nothing is changed in the game itself until the next launch.";
        if(!running)help2="Saved choices apply on a fresh launch.";
    } else if(index==1) {
        label=running?"Apply and Restart":"Apply and Exit";
        help1=running?"Saves these choices and restarts the game now so they apply. Unsaved game progress is lost.":"Saves these choices and leaves without starting the game.";
        help2=running?"You are asked to confirm first.":"Saved choices apply the next time the game starts.";
    } else {
        label="Cancel";help1="Leaves the menu without saving. You are asked first when there are unsaved changes.";help2="";
    }
}
}
