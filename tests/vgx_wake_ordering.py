"""Check the production wake's compiled memory-order contract, not source spelling."""
from pathlib import Path
import re
import sys

ir = Path(sys.argv[1]).read_text()
body = re.search(r"define\b[^\n]*@wake_ordering_contract\([^\n]*\{\n(.*?)^}", ir, re.M | re.S)
assert body, "production WakePlayer wrapper missing from compiled IR"
instructions = body.group(1).splitlines()
sleep_load = next((i for i, line in enumerate(instructions)
                   if "load atomic" in line and "sPlSleeping" in line), None)
assert sleep_load is not None, "sleeping flag's atomic load missing"
assert any(re.search(r"\bfence seq_cst\b", line) for line in instructions[:sleep_load]), (
    "WakePlayer needs a full store-to-load fence before reading sleeping: "
    "release publication alone allows both sides to see old values")
print("PASS: compiled production wake orders publication before its sleeping-flag load")
