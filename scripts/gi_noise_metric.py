"""GI buffer noise metric for spatial-hash GI optimization A/B.

Loads an HDR dump (e.g. dumps/diffuse_gi/..._gi_diffuse_hash_cache.hdr) and
reports noise proxies. Emphasis on LOW-FREQUENCY noise (blotchiness), which is
the failure mode optimizations can introduce.

Usage: py scripts/gi_noise_metric.py <a.hdr> [b.hdr]
  - one arg : print metrics for that buffer
  - two args: print metrics for both + deltas (B - A)
"""
import sys
import numpy as np
import imageio.v2 as imageio
from scipy.ndimage import gaussian_filter


def luminance(img):
    img = np.nan_to_num(np.asarray(img, dtype=np.float64))
    if img.ndim == 2:
        return img
    return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722


def metrics(path):
    lum = luminance(imageio.imread(path))
    mean = float(lum.mean())
    # Band-pass energy at the ~4-16px scale = low-frequency "blotch" noise,
    # excluding fine grain (<2px) and true broad gradients (>16px).
    g4 = gaussian_filter(lum, 4.0)
    g16 = gaussian_filter(lum, 16.0)
    lowfreq = float(np.mean(np.abs(g4 - g16)))
    # Fine grain (high-freq).
    g2 = gaussian_filter(lum, 2.0)
    highfreq = float(np.mean(np.abs(lum - g2)))
    denom = max(mean, 1e-4)
    return {
        "mean_luma": mean,
        "lowfreq_noise": lowfreq,
        "lowfreq_rel_%": 100.0 * lowfreq / denom,
        "highfreq_noise": highfreq,
        "highfreq_rel_%": 100.0 * highfreq / denom,
    }


def fmt(m):
    return (f"mean={m['mean_luma']:.4f}  lowfreq={m['lowfreq_noise']:.5f} "
            f"({m['lowfreq_rel_%']:.2f}%)  highfreq={m['highfreq_noise']:.5f} "
            f"({m['highfreq_rel_%']:.2f}%)")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    a = metrics(args[0])
    print(f"A {args[0]}\n  {fmt(a)}")
    if len(args) >= 2:
        b = metrics(args[1])
        print(f"B {args[1]}\n  {fmt(b)}")
        print("delta (B-A):")
        for k in a:
            print(f"  {k:16s} {b[k]-a[k]:+.5f}")


if __name__ == "__main__":
    main()
