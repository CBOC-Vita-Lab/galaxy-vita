#!/usr/bin/env bash
set -euo pipefail
python3 -B - <<'PY'
import hashlib,json,pathlib
root=pathlib.Path('/source')
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
inputs={}
game=json.loads((root/'build-vita-source/build-report.json').read_text())
archive=root/'build-vita-source/libgalaxy_vita_sources.a'
assert game['counts']=={'completed':1834} and game['source_changes_during_build']==[]
assert game['inventory']['matched'] and game['archive']['member_count']==1834
assert digest(archive)==game['archive']['sha256']
inputs[str(archive)]=digest(archive)
for name,expected in json.loads((root/'build-vita-source/source-state-after.json').read_text()).items():
    path=root/name
    assert digest(path)==expected,name
    inputs[str(path)]=expected
report=json.loads((root/'build-vita-runtime/runtime-report.json').read_text())
archive=root/'build-vita-runtime/libgalaxy_vita_runtime_partial.a'
assert report['source_changes_during_batch']==[] and report['symbol_audit_input_changes']==[]
assert digest(archive)==report['archive']['sha256']
inputs[str(archive)]=digest(archive)
for unit in report['units']:
    if unit['status']=='completed':
        assert digest(root/unit['source'])==unit['source_sha256_after'],unit['source']
        for name,expected in unit['dependencies'].items():
            if name.startswith(('/source/','/opt/vitasdk/')):
                assert digest(pathlib.Path(name))==expected,name
                inputs[name]=expected
for path in (root/'vita/game').iterdir():
    if path.is_file(): inputs[str(path)]=digest(path)
pathlib.Path('/build/verified-inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
print('Verified current full game and partial runtime archives; strict CRT game link follows')
PY
cmake -G Ninja -S /source/vita/game -B /build \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vitasdk/share/vita.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DRUNTIME_ARCHIVE=/source/build-vita-runtime/libgalaxy_vita_runtime_partial.a \
    -DGAME_ARCHIVE=/source/build-vita-source/libgalaxy_vita_sources.a
cmake --build /build --parallel 2
/opt/vitasdk/bin/arm-vita-eabi-nm --undefined-only /build/galaxy_native > /build/undefined-symbols.txt
python3 -B - <<'PY'
import hashlib,json,pathlib,zipfile
rows=pathlib.Path('/build/undefined-symbols.txt').read_text().splitlines()
assert all(line.split()[0] in ('w','v') for line in rows),rows
required_missing=[line.split()[-1] for line in rows if line.split()[-1].startswith(('_ZN','pthread_'))]
assert not required_missing,('Required original game or worker-thread implementation absent',required_missing)
with zipfile.ZipFile('/build/galaxy-native-development.vpk') as package:
    assert package.testzip() is None
    assert set(package.namelist())=={'eboot.bin','sce_sys/param.sfo'}
inputs=json.loads(pathlib.Path('/build/verified-inputs.json').read_text())
assert all(hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()==expected for name,expected in inputs.items())
print('Verified',len(inputs),'unchanged source/SDK/archive inputs and game-code-only VPK; not playable/FPS evidence')
PY
sha256sum /build/galaxy_native /build/galaxy_native.velf /build/eboot.bin /build/galaxy-native-development.vpk > /build/artifacts.sha256
