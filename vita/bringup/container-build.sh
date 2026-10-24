#!/usr/bin/env bash
set -euo pipefail
python3 -B - <<'PY'
import hashlib,json,pathlib
root=pathlib.Path('/source')
report=json.loads((root/'build-vita-runtime/runtime-report.json').read_text())
archive=root/'build-vita-runtime/libgalaxy_vita_runtime_partial.a'
assert report['source_changes_during_batch']==[] and report['symbol_audit_input_changes']==[]
assert hashlib.sha256(archive.read_bytes()).hexdigest()==report['archive']['sha256']
inputs={}
for unit in report['units']:
    if unit['status']=='completed':
        assert hashlib.sha256((root/unit['source']).read_bytes()).hexdigest()==unit['source_sha256_after'],unit['source']
        for name,expected in unit['dependencies'].items():
            if name.startswith(('/source/','/opt/vitasdk/')):
                assert hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()==expected,name
                inputs[name]=expected
for path in (root/'vita/bringup').iterdir():
    if path.is_file():
        inputs[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
inputs[str(archive)]=report['archive']['sha256']
pathlib.Path('/build/verified-inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
print('Verified current partial runtime archive; not a complete game link')
PY
cmake -G Ninja -S /source/vita/bringup -B /build \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vitasdk/share/vita.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DRUNTIME_ARCHIVE=/source/build-vita-runtime/libgalaxy_vita_runtime_partial.a
cmake --build /build --parallel 2
/opt/vitasdk/bin/arm-vita-eabi-nm --undefined-only /build/galaxy_bringup > /build/undefined-symbols.txt
python3 -B - <<'PY'
import pathlib
rows=pathlib.Path('/build/undefined-symbols.txt').read_text().splitlines()
assert all(line.split()[0] in ('w','v') for line in rows),rows
print('No strong undefined symbols;',len(rows),'optional SDK weak symbols recorded')
PY
sha256sum /build/galaxy_bringup /build/galaxy_bringup.velf /build/eboot.bin /build/galaxy-native-bringup.vpk > /build/artifacts.sha256
python3 -B - <<'PY'
import zipfile
with zipfile.ZipFile('/build/galaxy-native-bringup.vpk') as package:
    assert package.testzip() is None
    assert set(package.namelist())=={'eboot.bin','sce_sys/param.sfo'}
    print('Verified diagnostic VPK payload:',package.namelist())
PY
python3 -B - <<'PY'
import hashlib,json,pathlib
inputs=json.loads(pathlib.Path('/build/verified-inputs.json').read_text())
assert all(hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()==expected for name,expected in inputs.items())
print('Verified',len(inputs),'current source/SDK/archive inputs without build drift')
PY
