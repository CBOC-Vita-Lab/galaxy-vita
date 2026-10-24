import sys, os, re; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import read, write, sub, done

# `T x = init;` -> `T x; x = init;` so switch/goto may legally jump past it
# (only used for scalar and pointer types).
SPLITS = [
    ('src/Game/Animation/XanimeResource.cpp', 224),
    ('src/Game/AreaObj/ChangeBgmCube.cpp', 133),
    ('src/Game/Map/KCollision.cpp', 1205),
    ('src/Game/Player/MarioActor.cpp', 1348),
    ('src/Game/Player/MarioActor.cpp', 1416),
    ('src/Game/Player/MarioWarp.cpp', 548),
    ('src/Game/Ride/SurfRay.cpp', 398),
    ('src/Game/Util/JMapInfo.cpp', 101),
    ('src/Game/Util/JMapInfo.cpp', 61),
    ('src/JSystem/JKernel/JKRMemArchive.cpp', 226),
]
DECL = re.compile(r'^(\s*)(.*?[\w\*&>])\s*\b(\w+)\s*=\s*(.+;)\s*$')

by_file = {}
for f, l in SPLITS:
    by_file.setdefault(f, []).append(l)
for f, lines in by_file.items():
    rows = read(f).split('\n')
    for l in sorted(lines, reverse=True):
        row = rows[l - 1]
        m = DECL.match(row)
        if not m:
            print('no match', f, l, row)
            sys.exit(1)
        ind, typ, name, init = m.groups()
        rows[l - 1] = '%s%s %s;\n%s%s = %s' % (ind, typ, name, ind, name, init)
    write(f, '\n'.join(rows))

# Case bodies that declare objects of class type get their own scope.
G = 'src/Game/System/GameEventFlagChecker.cpp'
sub(G, '''    case GameEventFlag::Type_GalaxyOpenStar:
        s32 currentPowerStarNum = mDataHolder->calcCurrentPowerStarNum();
        s32 powerStarOpenNum = GameDataConst::getPowerStarNumToOpenGalaxy(pFlag->mGalaxyName);

        return powerStarOpenNum <= currentPowerStarNum;''', '''    case GameEventFlag::Type_GalaxyOpenStar: {
        s32 currentPowerStarNum = mDataHolder->calcCurrentPowerStarNum();
        s32 powerStarOpenNum = GameDataConst::getPowerStarNumToOpenGalaxy(pFlag->mGalaxyName);

        return powerStarOpenNum <= currentPowerStarNum;
    }''')
sub(G, '''    case GameEventFlag::Type_EventFlag:
        const char* pRequirement1 = pFlag->mRequirement1;
        const char* pRequirement2 = pFlag->mRequirement2;
        bool isOnRequirement1 = pRequirement1 != nullptr ? isOn(pRequirement1) : true;
        bool isOnRequirement2 = pRequirement2 != nullptr ? isOn(pRequirement2) : true;

        return isOnRequirement1 && isOnRequirement2;''', '''    case GameEventFlag::Type_EventFlag: {
        const char* pRequirement1 = pFlag->mRequirement1;
        const char* pRequirement2 = pFlag->mRequirement2;
        bool isOnRequirement1 = pRequirement1 != nullptr ? isOn(pRequirement1) : true;
        bool isOnRequirement2 = pRequirement2 != nullptr ? isOn(pRequirement2) : true;

        return isOnRequirement1 && isOnRequirement2;
    }''')
sub(G, '''    case GameEventFlag::Type_StarPiece:
        GameEventFlagAccessor accessor1 = GameEventFlagAccessor(pFlag);
        s32 needStarPieceNum = accessor1.getNeedStarPieceNum();

        return mDataHolder->getStarPieceNumGivingToTicoSeed(pFlag->mStarPieceIndex + 8) >= needStarPieceNum;
    case GameEventFlag::Type_EventValueIsZero:
        GameEventFlagAccessor accessor2 = GameEventFlagAccessor(pFlag);

        if (isOn(accessor2.getRequirement()) == false) {
            return false;
        }

        return static_cast< u16 >(mDataHolder->getGameEventValue(accessor2.getEventValueName())) == 0;''', '''    case GameEventFlag::Type_StarPiece: {
        GameEventFlagAccessor accessor1 = GameEventFlagAccessor(pFlag);
        s32 needStarPieceNum = accessor1.getNeedStarPieceNum();

        return mDataHolder->getStarPieceNumGivingToTicoSeed(pFlag->mStarPieceIndex + 8) >= needStarPieceNum;
    }
    case GameEventFlag::Type_EventValueIsZero: {
        GameEventFlagAccessor accessor2 = GameEventFlagAccessor(pFlag);

        if (isOn(accessor2.getRequirement()) == false) {
            return false;
        }

        return static_cast< u16 >(mDataHolder->getGameEventValue(accessor2.getEventValueName())) == 0;
    }''')

C = 'src/Game/AreaObj/ChangeBgmCube.cpp'
t = read(C)
a = t.index('        case 0:\n            if (objArg1 < 0) {\n                MR::startCurrentStageBGM();')
b = t.index('        case 1:\n', a)
seg = t[a:b]
if not seg.startswith('        case 0: {'):
    seg = seg.replace('        case 0:\n', '        case 0: {\n', 1)
    seg = seg.rstrip('\n') + '\n        }\n'
    t = t[:a] + seg + t[b:]
    write(C, t)

done()
