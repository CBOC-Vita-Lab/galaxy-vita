import pathlib
import sys
import subprocess

lines=pathlib.Path(sys.argv[1]).read_text().splitlines()
limits={'coldtrace_arm':4096,'coldtrace_overlap':32768,'coldtrace_item':2048,'coldtrace_worker':2048}
seen={}
largest=0
for line in lines:
    fields=line.split('\t')
    size=int(fields[1])
    assert size<=32768,('emitted callee exceeds core stack bound',fields[0],size)
    assert fields[2]=='static',('unbounded stack entry',line)
    largest=max(largest,size)
    for name,limit in limits.items():
        if fields[0].endswith(':'+name):
            size=int(fields[1])
            assert size<=limit,(name,size,limit)
            seen[name]=size
assert seen.keys()==limits.keys(),seen
symbols=subprocess.check_output(['objdump','-t',sys.argv[2]],text=True).splitlines()
recorder=[s for s in symbols if s.split() and s.split()[-1]=='_ZN3vgx9coldtrace10g_recorderE']
assert len(recorder)==1 and '.bss' in recorder[0],('probe state must be zero-initialized BSS, not a large data image',recorder)
print('PASS: coldtrace bounded stack usage',seen,'largest emitted callee',largest)
