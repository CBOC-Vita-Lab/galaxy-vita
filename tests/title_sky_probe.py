"""Change ONLY a disposable cloned test archive to identify a sky texture by its pixels."""
import argparse
import struct
from pathlib import Path


def entries(data):
    start=data.index(b"1XET")
    size=struct.unpack_from("<I",data,start+4)[0]
    count,=struct.unpack_from("<H",data,start+8)
    table,names=struct.unpack_from("<II",data,start+12)
    assert count==12 and start+size<=len(data)
    for i in range(count):
        h=start+table+32*i
        n=start+names+struct.unpack_from("<H",data,start+names+6+4*i)[0]
        name=data[n:data.index(0,n)].decode("ascii")
        w,ht=struct.unpack_from("<HH",data,h+2)
        yield name,h,data[h],w,ht,data[h+24],h+struct.unpack_from("<I",data,h+28)[0]


if __name__=="__main__":
    ap=argparse.ArgumentParser();ap.add_argument("profile",type=Path);ap.add_argument("name",choices=["Cloud01k","EarthKsMM","Skyk","EarthFarK","fine-mip","base-cloud"])
    args=ap.parse_args();d=args.profile.resolve()
    out=(Path(__file__).resolve().parents[2]/"outputs/galaxy-c183").resolve()
    if d.parent!=out or not (d/"isolation.json").is_file() or (d/"launch.json").exists():
        raise ValueError("Only a fresh, unlaunched, disposable C183 profile may be probed")
    [path]=(d/"portable/fs/ux0/data/smg/game/files/ObjectData").rglob("CometNearOrbitSky.arc")
    if path.is_symlink() or not path.resolve().is_relative_to(d/"portable/fs") or path.stat().st_nlink!=1:
        raise ValueError("Refusing linked or out-of-profile archive")
    data=bytearray(path.read_bytes())
    if args.name=="base-cloud":
        _,h,*_=next(r for r in entries(data) if r[0]=="Cloud01k")
        data[h+16]=0
        data[h+20]=data[h+21]=1
        data[h+22]=data[h+23]=0
        data[h+24]=1
        struct.pack_into("<h",data,h+26,0)
        path.write_bytes(data);print("Private Cloud01k base bilinear probe");raise SystemExit
    if args.name=="fine-mip":
        for name,h,*rest in entries(data):
            if name in ("Cloud01k","EarthKsMM"):
                struct.pack_into("<h",data,h+26,-50)
                data[h+19]=2
        path.write_bytes(data);print("Private filter probe: -0.50 LOD, GX_ANISO_4 for Earth and clouds only");raise SystemExit
    name,h,fmt,w,ht,n,p=next(r for r in entries(data) if r[0]==args.name)
    for i in range(n):
        # GX 8x8/32-byte blocks for I4/CMPR and 8x4 for I8.
        tw,th=(8,4) if fmt==1 else (8,8)
        length=((w+tw-1)//tw)*((ht+th-1)//th)*32
        if fmt==14:
            # Opaque magenta; UV-independent sample at every mip.
            data[p:p+length]=bytes.fromhex("f81f000000000000")*(length//8)
        else: data[p:p+length]=bytes([255])*length
        p+=length;w=max(1,w//2);ht=max(1,ht//2)
    path.write_bytes(data)
    print("Private probe",name,"format",fmt,"mips",n)
