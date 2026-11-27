"""Count actual Beach Bowl swimming-penguin placements by scenario (local data only)."""
import sys
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(root / "tools/hdtex"), str(root / "tests/mp")]
from gxarc import rarc_files
from savecheck_scenarios import game_hash, bcsv_reader


def table(data):
    n, count, off, stride = struct.unpack_from("<iiiI", data)
    cols = {key: (ofs, kind) for key, _, ofs, _, kind in (
        struct.unpack_from("<IIHBB", data, 16 + i * 12) for i in range(count))}
    _, numeric = bcsv_reader(data)

    def get(i, col):
        ofs, kind = cols[game_hash(col)]
        p = off + i * stride + ofs
        if kind == 1 or kind == 6:
            if kind == 6:
                p = off + n * stride + struct.unpack_from("<I", data, p)[0]
            return data[p:data.index(b"\0", p)].decode("shift_jis")
        if kind == 2:
            return struct.unpack_from("<f", data, p)[0]
        return numeric(i, col)
    return range(n), get


def archive(file):
    return dict(rarc_files(file.read_bytes()))


if __name__ == "__main__":
    stage = Path(sys.argv[1])
    zone = archive(next(stage.rglob("HeavenlyBeachZone.arc")))
    placed = {}
    for path, data in zone.items():
        if "/placement/" not in path or not path.endswith("/objinfo"):
            continue
        layer = path.split("/")[-2]
        indices, get = table(data)
        rows = [(get(i, "CommonPath_ID"), get(i, "Obj_arg0"))
                for i in indices if get(i, "name") == "PenguinStudent"]
        if rows:
            placed[layer] = rows
    print("Student placements (path, behavior) by layer:", placed)
    scen = archive(next(stage.rglob("HeavenlyBeachGalaxyScenario.arc")))
    rows, get = table(next(data for path, data in scen.items() if path.lower().endswith("/scenariodata.bcsv")))
    paths = set()
    for i in rows:
        mask = get(i, "HeavenlyBeachZone")
        active = [p for layer, group in placed.items() if layer == "common" or mask & (1 << (ord(layer[-1]) - ord("a"))) for p in group]
        swimming = [rail for rail, behavior in active if behavior not in (-1, 0xffffffff)]
        paths.update(swimming)
        print(f"scenario {get(i, 'ScenarioNo')}: {len(swimming)} follower placements, rail IDs {swimming}")
    indices, get = table(zone["stage/jmp/path/commonpathinfo"])
    for i in indices:
        if get(i, "l_id") in paths:
            print("Follower rail", get(i, "l_id"), get(i, "closed"), "points:", get(i, "num_pnt"))
            if len(sys.argv) > 2:
                if len(paths) != 1:
                    raise ValueError("expected a single shared Beach Bowl follower path")
                rows, point = table(zone[f"stage/jmp/path/commonpathpointinfo.{get(i, 'no')}"])
                if len(rows) != get(i, "num_pnt"):
                    raise ValueError("path record count disagrees")
                with Path(sys.argv[2]).open("x") as out:
                    for j in rows:
                        print(*(point(j, f"pnt{k}_{a}") for k in range(3) for a in "xyz"), file=out)
