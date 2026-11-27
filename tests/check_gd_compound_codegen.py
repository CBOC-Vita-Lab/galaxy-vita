"""Check cursor publication in the real Cortex-A9/Thumb command wrappers."""
import re
import sys
from pathlib import Path

assembly = Path(sys.argv[1]).read_text()
assert re.search(r"\.cpu\s+cortex-a9\b", assembly), "requires actual Cortex-A9 assembly"
for name in ("gd_raw_bp", "gd_raw_cp", "gd_raw_xf", "gd_raw_xf_header"):
    match = re.search(r"^" + name + r":\n(.*?)^\.Lfunc_end\d+:", assembly, re.M | re.S)
    assert match, "missing compiled command: " + name
    body = match.group(1)
    # GDLObj::ptr is offset8 on the actual 32-bit Vita ABI. Payload stores in
    # these four wrappers use other offsets; none of them needs a stack frame.
    assert not re.search(r"\b(?:push|pop)\b|\bsp\b", body), name + ": unexpected stack frame"
    stores = re.findall(r"^\s*str(?:\.w)?\s+\w+,\s*\[\w+,\s*#8\]", body, re.M)
    assert len(stores) == 1, f"{name}: expected one cursor publication, observed {len(stores)}"
    assert not re.search(r"^\s*bl(?:\.w)?\s", body, re.M), name + ": unexpected helper call"
    instructions = sum(bool(re.match(r"^\s+[a-z][a-z0-9.]*\s", line)) for line in body.splitlines())
    print(f"PASS: {name}: one cursor publication, {instructions} ARM instructions, no call/stack")
