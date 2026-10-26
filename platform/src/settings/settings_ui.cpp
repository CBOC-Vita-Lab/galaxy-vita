#include "port/settings_ui.h"
#include <algorithm>

namespace port::settings {
namespace {
ExitIntent Safe(const SettingsSession& session) {return session.mode==OpenMode::Running?ExitIntent::Return:ExitIntent::Exit;}
unsigned Profile(const UiState& s) {return static_cast<unsigned>(s.working.profile);}
controls::ControllerCaps EditCaps(const UiState& s) {
    auto caps=controls::NominalCaps(s.working.profile);
    if(s.controller.connected&&s.detectedProfile==s.working.profile) {caps=s.controller;caps.availableButtons&=controls::NominalCaps(s.working.profile).availableButtons;}
    return caps;
}
unsigned Commands(const UiState& s) {return s.page==Page::Display?4:static_cast<unsigned>(s.session->controls->actionCount)+1;}
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
        if(s.row==0) {s.working.profile=s.working.profile==controls::ProfileId::Handheld?controls::ProfileId::PlayStation:controls::ProfileId::Handheld;s.bindingSlot=0;s.armed=false;}
        else if(s.row<=s.session->controls->actionCount) {
            const auto& action=s.session->controls->actions[s.row-1];if(action.maxBindings>1)s.bindingSlot=1-s.bindingSlot;
        }
        return;
    }
    switch(s.row) {
        case 0: {
            // Right moves along the screen (30 FPS, Steady 30, Up to 60) and wraps; fixed 30 needs the driver capability.
            if(!s.session->features.fixed30&&s.working.fpsLimit==30) {s.message="Fixed 30 FPS unavailable in this build";break;}
            std::vector<int> rates{0};if(s.session->features.fixed30)rates.push_back(30);rates.push_back(kSteady30);
            auto at=std::find(rates.begin(),rates.end(),s.working.fpsLimit);const int n=static_cast<int>(rates.size());
            const int index=at==rates.end()?0:static_cast<int>(at-rates.begin());
            s.working.fpsLimit=rates[static_cast<size_t>((index+direction+n)%n)];break;
        }
        case 1: s.working.autoQuality=static_cast<AutoQuality>((static_cast<int>(s.working.autoQuality)+direction+3)%3);break;
        case 2: {
            if(s.session->languages.names.empty()) {s.message="Game language unavailable until game data is found";break;}
            std::vector<std::string> choices{""};choices.insert(choices.end(),s.session->languages.names.begin(),s.session->languages.names.end());
            auto at=std::find(choices.begin(),choices.end(),s.working.language);int index=at==choices.end()?0:static_cast<int>(at-choices.begin());
            index=(index+direction+static_cast<int>(choices.size()))%static_cast<int>(choices.size());s.working.language=choices[static_cast<size_t>(index)];break;
        }
        case 3:s.working.reducedMotion=!s.working.reducedMotion;break;
        default:break;
    }
}
}
unsigned RowCount(const UiState& s) {return Commands(s)+3;}
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
        else if(s.row>0&&s.row<=s.session->controls->actionCount) {s.action=s.session->controls->actions[s.row-1].id;s.picker=0;ModalOpen(s,Modal::ButtonPicker);}
        else ModalOpen(s,Modal::ResetProfile);
        return;
    }
    if(event&Up) {s.row=(s.row+RowCount(s)-1)%RowCount(s);s.bindingSlot=0;s.changedAt=now;}
    if(event&Down) {s.row=(s.row+1)%RowCount(s);s.bindingSlot=0;s.changedAt=now;}
    if(event&(Left|Right))Change(s,event&Left?-1:1);
    if(event&Accept) {
        const auto commands=Commands(s);
        if(s.row==commands)Save(s,s.session->mode==OpenMode::Running?ExitIntent::Return:ExitIntent::Play);
        else if(s.row==commands+1) {if(s.session->mode==OpenMode::Running)ModalOpen(s,Modal::Restart);else Save(s,ExitIntent::Exit);}
        else if(s.row==commands+2)Cancel(s);
        else if(s.page==Page::Display||s.row==0)Change(s,1);
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
}
