#!/usr/bin/env python3
"""build_route24.py: the scripted input of the pack24 refill tour (ux0:data/smg/input.txt), made with mkroute.py (route24's builder, copied here).

    python3 build_route24.py [--out DIR]     writes route-p24.txt (and route-p24-pu.txt, the same plus nothing but a longer idle)

t = 0 is the first turn of play in each scene (env: PETARI_SCRIPT_ANCHOR=play PETARI_SCRIPT_LOG=1). The priority actions come first, so a
stage hold of about 1,500 turns (25 s) sees all of them: first run, jump, spin, run+jump, long jump, backflip (legs halved: safe where the
spawn area is small), then a pointer sweep with star-bit shots (B), one pause (PLUS, 100 turns, PLUS), crouch, jump + ground pound, camera steps.
Every file is verified turn by turn (emit, re-parse with the port's grammar, replay) before it is written.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkroute import Route, TPS, verify_route, warnings, STOCK_ENTRIES, STOCK_CHARS  # noqa: E402

def L(n): return max(4, int(round(n * 0.5)))

def firstuse(r):
    r.wait(6)
    r.run("F", L(40)); r.wait(20)
    r.jump(14, 40)
    r.spin(6, 40)
    with r.par() as p:
        p.run("B", L(40)); p.at(L(40) // 2 + 2).hold("A", 14)
    r.wait(40)
    r.long_jump("F", L(30), 25, 50)
    r.run("B", L(70)); r.wait(20)
    r.backflip(50)
    r.run("F", L(25)); r.wait(30)

def route_p24():
    r = Route("route-p24")
    firstuse(r)                                   # ~7.9 s
    for x, y in ((-0.5, 0.2), (0.5, -0.2)):       # pointer + star-bit shots on the spot (3 s each)
        with r.par() as p:
            p.pointer(x, y, 90); p.at(30).shoot(); p.at(60).shoot()
    r.wait(30)
    r.pause(100, 60)                              # the pause menu (PLUS twice: see FORMAT.md)
    r.crouch(40); r.wait(20)
    r.jump(14, 10); r.ground_pound(60)
    r.cam_right(); r.cam_left(); r.cam_reset()
    r.wait(max(1500 - r.t, 60))
    return r

def route_pu(total=4500):
    """a form cycle every 300 turns (PETARI_POWERUP_EVERY=300): in each, walk, a held jump (flying/floating), spin (fire, ice, spring), shots, ground pound"""
    r = Route("route-pu-p24")
    r.wait(60)
    while r.t + 300 <= total:
        t0 = r.t
        r.run("F", L(40)); r.jump(40, 20); r.spin(6, 30)
        with r.par() as p:
            p.pointer(0.4, 0.1, 60); p.at(20).shoot(); p.at(40).shoot()
        r.run("B", L(40)); r.jump(14, 20); r.ground_pound(45)
        r.wait(max(300 - (r.t - t0), 1))
    r.wait(max(total - r.t, 1))
    return r

def main(argv):
    out = "."
    if "--out" in argv: out = argv[argv.index("--out") + 1]
    os.makedirs(out, exist_ok=True)
    r = route_p24()
    info = verify_route(r, False, STOCK_ENTRIES, STOCK_CHARS); verify_route(r, True, STOCK_ENTRIES, STOCK_CHARS)
    head = ["route-p24.txt: the pack24 refill tour: first-use block (run, jump, spin, run+jump, long jump, backflip), pointer + star-bit shots, pause, crouch, ground pound, camera",
            "t = 0 is the first turn of play in the scene (PETARI_SCRIPT_ANCHOR=play); %d turns (%.1f s); written by tools/pack24/build_route24.py" % (r.turns, r.turns / TPS),
            "env: PETARI_SCRIPT_ANCHOR=play PETARI_SCRIPT_LOG=1"]
    with open(os.path.join(out, "route-p24.txt"), "w") as f: f.write(r.text(header=head))
    r2 = route_pu()
    info2 = verify_route(r2, False, STOCK_ENTRIES, STOCK_CHARS); verify_route(r2, True, STOCK_ENTRIES, STOCK_CHARS)
    h2 = ["route-pu-p24.txt: player-form cycle: walk, held jump, spin, pointer + shots, run back, jump, ground pound, every 300 turns for 4500 turns",
          "t = 0 is the first turn of play in the scene (PETARI_SCRIPT_ANCHOR=play); env: PETARI_SCRIPT_ANCHOR=play PETARI_SCRIPT_LOG=1"]
    with open(os.path.join(out, "route-pu-p24.txt"), "w") as f: f.write(r2.text(header=h2))
    print("route-pu-p24.txt %d turns %d entries %d chars" % (r2.turns, info2["entries"], info2["chars"]))
    print("route-p24.txt  %d turns %.1f s  %d entries %d chars" % (r.turns, r.turns / TPS, info["entries"], info["chars"]))
    for w in warnings(r): print("   warning:", w)
    return 0

if __name__ == "__main__": sys.exit(main(sys.argv))
