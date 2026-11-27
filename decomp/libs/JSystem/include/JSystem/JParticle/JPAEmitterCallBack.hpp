#pragma once

class JPABaseEmitter;

class JPAEmitterCallBack {
public:
    virtual ~JPAEmitterCallBack() = 0;
    virtual void execute(JPABaseEmitter*) {
    }
    virtual void executeAfter(JPABaseEmitter*) {
    }
    virtual void draw(JPABaseEmitter*) {
    }
    virtual void drawAfter(JPABaseEmitter*) {
    }
    virtual void init(JPABaseEmitter*) {
    }
#ifdef TARGET_PC
    // GXV_PAR_PTCL (parwork, 7 Nov; JPAEmitterManager.cpp calcNormal): what the manager may do with an emitter that has this callback.
    //  0 unknown: the whole turn's particle calc stays serial.
    //  1 pure: execute / executeAfter write only the emitter they are given and read only data that nothing writes during the particle
    //    calc (a host matrix, the callback's own fields), so the emitter may be calculated on another thread.
    //  2 audited serial: touches state of its own besides the emitter (MarioEffect's follow table); runs on the game thread, in order,
    //    but creates and deletes no emitter or particle.
    virtual int portCalcKind() const {
        return 0;
    }
#endif
};
