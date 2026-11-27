"""Inventory only counts, SHA-256 and bytes of the host cache experiment (no copied private data)."""
import argparse
import collections
import hashlib
import json
from pathlib import Path


def inventory(root):
    result={}
    for f in root.rglob("*"):
        if f.is_symlink(): raise ValueError("symlink in cache")
        if f.is_file():
            data=f.read_bytes()
            result[f.relative_to(root).as_posix()]=(len(data), hashlib.sha256(data).hexdigest())
    return result


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("baseline",type=Path)
    ap.add_argument("donor",type=Path)
    a=ap.parse_args()
    old,new=inventory(a.baseline),inventory(a.donor)
    counts=collections.Counter(); bytes_new=collections.Counter(); records=[]
    for key in sorted(old.keys()|new.keys()):
        group="spv" if key.endswith(".spv") else Path(key).name
        what="added" if key not in old else "removed" if key not in new else "unchanged" if old[key]==new[key] else "changed"
        counts[(group,what)]+=1
        if key in new: bytes_new[(group,what)]+=new[key][0]
        if group!="spv": records.append(dict(file=group,old=old.get(key),new=new.get(key)))
    print(json.dumps({"counts_and_donor_bytes":[[g,c,n,bytes_new[(g,c)]] for (g,c),n in sorted(counts.items())],
                      "other_files":records},indent=2))
