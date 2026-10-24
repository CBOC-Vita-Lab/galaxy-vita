"""Real-ESRGAN inference on the CPU (PyTorch), without basicsr: the two network
architectures of the published weights (RRDBNet: RealESRGAN_x4plus / _anime_6B;
SRVGGNetCompact: realesr-general-x4v3), weights loaded from the .pth files of the
xinntao/Real-ESRGAN releases (BSD-3-Clause). Input/output: float32 RGB arrays in [0, 1].
"""
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


class RDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)

    def forward(self, x):
        lr = lambda t: F.leaky_relu(t, 0.2)
        x1 = lr(self.conv1(x))
        x2 = lr(self.conv2(torch.cat((x, x1), 1)))
        x3 = lr(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = lr(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.rdb1, self.rdb2, self.rdb3 = RDB(nf, gc), RDB(nf, gc), RDB(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, num_block=23, nf=64, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(3, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(num_block)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)

    def forward(self, x):
        lr = lambda t: F.leaky_relu(t, 0.2)
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = lr(self.conv_up1(F.interpolate(feat, scale_factor=2, mode='nearest')))
        feat = lr(self.conv_up2(F.interpolate(feat, scale_factor=2, mode='nearest')))
        return self.conv_last(lr(self.conv_hr(feat)))


class SRVGGNetCompact(nn.Module):
    def __init__(self, nf=64, num_conv=32, upscale=4):
        super().__init__()
        self.upscale = upscale
        body = [nn.Conv2d(3, nf, 3, 1, 1), nn.PReLU(num_parameters=nf)]
        for _ in range(num_conv):
            body += [nn.Conv2d(nf, nf, 3, 1, 1), nn.PReLU(num_parameters=nf)]
        body.append(nn.Conv2d(nf, 3 * upscale * upscale, 3, 1, 1))
        self.body = nn.ModuleList(body)
        self.upsampler = nn.PixelShuffle(upscale)

    def forward(self, x):
        out = x
        for layer in self.body:
            out = layer(out)
        return self.upsampler(out) + F.interpolate(x, scale_factor=self.upscale, mode='nearest')


def load(path):
    sd = torch.load(path, map_location='cpu', weights_only=True)
    sd = sd.get('params_ema', sd.get('params', sd))
    if 'conv_first.weight' in sd:
        nblk = 1 + max(int(k.split('.')[1]) for k in sd if k.startswith('body.'))
        net = RRDBNet(num_block=nblk)
    else:
        nconv = (1 + max(int(k.split('.')[1]) for k in sd if k.startswith('body.')) - 3) // 2
        net = SRVGGNetCompact(num_conv=nconv)
    net.load_state_dict(sd, strict=True)
    net.eval()
    return net


def upscale4(net, rgb, pad=8, mode=('reflect', 'reflect'), tile=192):
    """rgb: (h, w, 3) float32 in [0,1] -> (4h, 4w, 3). `mode`: numpy padding of the borders,
    (vertical, horizontal): 'wrap' for repeating textures, 'symmetric' for mirrored, 'edge' for
    clamped, so that the network sees what the sampler will show across an edge."""
    h, w, _ = rgb.shape
    p = np.pad(rgb, ((pad, pad), (0, 0), (0, 0)), mode=mode[0])
    p = np.pad(p, ((0, 0), (pad, pad), (0, 0)), mode=mode[1])
    out = np.zeros((4 * h, 4 * w, 3), np.float32)
    with torch.inference_mode():
        for ty in range(0, h, tile):
            for tx in range(0, w, tile):
                th, tw = min(tile, h - ty), min(tile, w - tx)
                src = p[ty:ty + th + 2 * pad, tx:tx + tw + 2 * pad]
                t = torch.from_numpy(np.ascontiguousarray(src.transpose(2, 0, 1)))[None]
                r = net(t)[0].numpy().transpose(1, 2, 0)
                out[4 * ty:4 * (ty + th), 4 * tx:4 * (tx + tw)] = r[4 * pad:4 * (pad + th), 4 * pad:4 * (pad + tw)]
    return np.clip(out, 0, 1)
