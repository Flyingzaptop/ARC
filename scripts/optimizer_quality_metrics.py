"""Bounded-working-set metrics. The standalone optimizer-quality.py is the oracle."""
import numpy as np
import cv2
import ctypes
from pathlib import Path

_native=None
_root=Path(__file__).resolve().parents[1]
for _path in (_root/'arc-quality-metrics.dll', _root/'build/Release/arc-quality-metrics.dll'):
    if _path.is_file():
        _library=ctypes.CDLL(str(_path))
        _native=_library.ArcQualityStripe
        _pointer=np.ctypeslib.ndpointer(dtype=np.float64,flags='C_CONTIGUOUS')
        _native.argtypes=[_pointer]*7+[ctypes.c_uint]*5+[_pointer]*2
        _native.restype=ctypes.c_int
        break

PROFILES = {
    "balanced": dict(ssim=.98, mean=.01, p99=.04, worst=.15, temporal=.02),
    "aggressive": dict(ssim=.94, mean=.03, p99=.12, worst=.25, temporal=.06),
}


def accepts(metrics, profile="balanced"):
    p = PROFILES[profile]
    return (metrics["ssim_gaussian_luma"] >= p["ssim"] and
            metrics["mean_linear_rgb_error"] <= p["mean"] and
            metrics["p99_tile_linear_rgb_error"] <= p["p99"] and
            metrics["worst_tile_linear_rgb_error"] <= p["worst"])


def compare_striped(reference, candidate, *, filter_fn, stripe_rows=128):
    if reference.shape != candidate.shape or reference.ndim != 3 or reference.shape[2] != 3 or min(reference.shape[:2]) < 11:
        raise ValueError("Matched RGB images at least 11x11 required")
    if stripe_rows < 8 or stripe_rows % 8:
        raise ValueError("Stripe height must align with error tiles")
    h, w, _ = reference.shape
    luma = np.array([.2126, .7152, .0722])
    ssim_sum = error_sum = peak = 0.
    tiles = []
    columns = np.arange(0, w, 8)
    for y in range(0, h, stripe_rows):
        end = min(y+stripe_rows, h)
        lo, hi = max(0, y-5), min(h, end+5)
        ra, rb = reference[lo:hi], candidate[lo:hi]
        if not np.isfinite(ra).all() or not np.isfinite(rb).all():
            raise ValueError("Non-finite image")
        a, b = cv2.transform(ra, luma[None,:]), cv2.transform(rb, luma[None,:])
        ma, mb = filter_fn(a), filter_fn(b)
        aa,bb,ab=filter_fn(a*a),filter_fn(b*b),filter_fn(a*b)
        if _native is not None:
            sums=np.empty(((end-y+7)//8,(w+7)//8),dtype=np.float64); totals=np.empty(3,dtype=np.float64)
            linear_a=reference.linear_region(slice(y,end)) if hasattr(reference,"linear_region") else cv2.pow(ra[y-lo:end-lo],2.2)
            linear_b=candidate.linear_region(slice(y,end)) if hasattr(candidate,"linear_region") else cv2.pow(rb[y-lo:end-lo],2.2)
            first,last=max(y,5)-y,max(max(y,5),min(end,h-5))-y
            first=min(first,end-y);last=min(last,end-y)
            if _native(ma,mb,aa,bb,ab,linear_a,linear_b,w,end-y,y-lo,first,last,sums,totals):raise ValueError('Native stripe contract')
            ssim_sum+=totals[0];error_sum+=totals[1];peak=max(peak,float(totals[2]))
            rows=np.arange(0,end-y,8);counts=np.minimum(8,end-y-rows)[:,None]*np.minimum(8,w-columns)[None,:]*3
            tiles.append((sums/counts).ravel())
            continue
        va = np.maximum(0, aa-ma*ma)
        vb = np.maximum(0, bb-mb*mb)
        cov = ab-ma*mb
        ssim = ((2*ma*mb+.01**2)*(2*cov+.03**2) /
                ((ma*ma+mb*mb+.01**2)*(va+vb+.03**2)))
        first, last = max(y, 5)-lo, min(end, h-5)-lo
        if last > first:
            ssim_sum += ssim[first:last, 5:-5].sum(dtype=np.float64)
        linear_a=reference.linear_region(slice(y,end)) if hasattr(reference,"linear_region") else cv2.pow(ra[y-lo:end-lo],2.2)
        linear_b=candidate.linear_region(slice(y,end)) if hasattr(candidate,"linear_region") else cv2.pow(rb[y-lo:end-lo],2.2)
        err = cv2.absdiff(linear_a,linear_b)
        error_sum += err.sum(dtype=np.float64)
        peak = max(peak, float(err.max()))
        rows = np.arange(0, end-y, 8)
        sums = np.add.reduceat(np.add.reduceat(cv2.transform(err,np.ones((1,3))), rows, axis=0), columns, axis=1)
        counts = np.minimum(8, end-y-rows)[:, None]*np.minimum(8, w-columns)[None, :]*3
        tiles.append((sums/counts).ravel())
    tile_errors = np.concatenate(tiles)
    result = dict(ssim_gaussian_luma=float(ssim_sum/((h-10)*(w-10))),
                  mean_linear_rgb_error=float(error_sum/(h*w*3)),
                  p99_tile_linear_rgb_error=float(np.quantile(tile_errors, .99)),
                  worst_tile_linear_rgb_error=float(tile_errors.max()),
                  peak_linear_rgb_error=peak)
    # Legacy field kept for consumers; the profile-specific decision is separate.
    result["moderate_pass"] = (result["ssim_gaussian_luma"] >= .98 and
                               result["mean_linear_rgb_error"] <= .01 and
                               result["p99_tile_linear_rgb_error"] <= .04)
    return result
