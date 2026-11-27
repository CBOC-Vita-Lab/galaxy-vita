"""Match saved Vita3K title clips on the unchanged comet and measure globe band steps."""
import argparse
import io
import json
import statistics
import subprocess
from pathlib import Path
from PIL import Image, ImageChops, ImageFilter, ImageStat


def ff(path, *opts):
    return subprocess.check_output(["ffmpeg", "-loglevel", "error", "-i", str(path), *opts])


def samples(path):
    w, h = 120, 31
    raw = ff(path, "-vf", "fps=12,crop=960:248:0:29,scale=120:31", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
    step = 3*w*h
    return [Image.frombytes("RGB", (w,h), raw[i:i+step]) for i in range(0,len(raw)-step+1,step)]


def frame(path,t):
    return Image.open(io.BytesIO(ff(path, "-ss",str(t), "-frames:v","1","-vcodec","png","-f","image2pipe","-"))).convert("RGB")


def steps(im):
    # Native-resolution, text-free side strips across the lower globe. Compare
    # long straight horizontal and vertical steps against the local slopes.
    scores=[]
    for l,r in ((24,232),(728,936)):
        a=im.convert("L").crop((l,510,r,569))
        for d in (a,a.transpose(Image.Transpose.TRANSPOSE)):
            pix=d.load();w,h=d.size
            for y in range(2,h-2):
                for x in range(0,w-7,4):
                    edge=sum(abs(pix[x+i,y]-pix[x+i,y-1]) for i in range(8))
                    neighbors=sum(abs(pix[x+i,y-1]-pix[x+i,y-2])+abs(pix[x+i,y+1]-pix[x+i,y]) for i in range(8))
                    scores.append(max(0, edge/8-neighbors/16))
    return (sum(x>=2 for x in scores)/len(scores), statistics.mean(scores), statistics.quantiles(scores,n=100)[98])


def fine_motion(a,b):
    # Change in high spatial frequencies over 1/6 s. This measures detail in
    # motion and shimmer together, not a substitute for a hardware motion trial.
    mask=Image.new("L",(416,59))
    for idx,(l,r) in enumerate(((24,232),(728,936))):
        roi=(l,510,r,569)
        def high(im):
            g=im.convert("L").crop(roi)
            return ImageChops.subtract(g,g.filter(ImageFilter.BoxBlur(2)),offset=128)
        mask.paste(ImageChops.difference(high(a),high(b)),(idx*208,0))
    return ImageStat.Stat(mask).mean[0]


def main():
    p=argparse.ArgumentParser();p.add_argument("before",type=Path);p.add_argument("after",type=Path);p.add_argument("output",type=Path)
    p.add_argument("--baseline-start",type=float,default=9);p.add_argument("--baseline-end",type=float,default=13.5);a=p.parse_args()
    a.output.mkdir(exist_ok=True,parents=True)
    before,after=samples(a.before),samples(a.after)
    rows=[]
    for i in range(round(a.baseline_start*12),min(len(before),round(a.baseline_end*12)),6):
        def error(j):
            return statistics.mean(ImageStat.Stat(ImageChops.difference(before[i],after[j])).mean)
        j=min(range(len(after)),key=error)
        if error(j)>2.0:continue
        bi,ai=frame(a.before,i/12),frame(a.after,j/12)
        bplus,aplus=frame(a.before,(i+2)/12),frame(a.after,(j+2)/12)
        rows.append(dict(t_before=i/12,t_after=j/12,comet_MAE=error(j),before=steps(bi),after=steps(ai),
                         before_fine_motion=fine_motion(bi,bplus),after_fine_motion=fine_motion(ai,aplus)))
        ix=len(rows)
        if ix in (1,4,7):
            both=Image.new("RGB",(960,2*200))
            both.paste(bi.crop((0,372,960,572)),(0,0));both.paste(ai.crop((0,372,960,572)),(0,200));both.save(a.output/f"matched-{ix}.png")
    result=json.dumps(rows,indent=2)
    (a.output/"metrics.json").write_text(result+"\n")
    print(result)


if __name__ == "__main__":main()
