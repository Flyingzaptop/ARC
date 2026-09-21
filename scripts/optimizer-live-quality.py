"""Confidence-qualified live A/B/A image assessment, outside the render process.

Flow is estimated only between the two ORIGINAL images. Candidate pixels never
influence motion estimation. This is a live proxy, not a same-state replay proof.
Unreliable alignment rejects the trial instead of relaxing quality thresholds.
"""
import argparse
import mmap
import os
# OpenCV's thread limit does not control NumPy/OpenBLAS. Configure the owned
# process before importing either library; never change the game's affinity.
for _thread_setting in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_thread_setting] = "1"
import traceback
import json
import runpy
import sys
if hasattr(sys.stdin,"reconfigure"):sys.stdin.reconfigure(encoding="utf-8")
if hasattr(sys.stdout,"reconfigure"):sys.stdout.reconfigure(encoding="utf-8")
from pathlib import Path
import numpy as np

repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / "build/quality-worker"))
import cv2
from optimizer_quality_metrics import compare_striped, accepts, PROFILES
cv2.setNumThreads(1)
cv2.ocl.setUseOpenCL(False)
_flow = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
def optical_flow(a,b): return _flow.calc(a,b,None)
metrics = runpy.run_path(str(repo / "scripts/optimizer-quality.py"))
gaussian_coordinates = np.arange(-5, 6)
gaussian_weights = np.exp(-(gaussian_coordinates**2)/(2*1.5**2))
gaussian_weights /= gaussian_weights.sum()


def fast_gaussian(pixels):
    # Same float64 11x11 separable Gaussian and reflect-without-repeating-edge
    # border as the independent NumPy implementation; no resolution reduction.
    return cv2.sepFilter2D(pixels, cv2.CV_64F, gaussian_weights, gaussian_weights,
                           borderType=cv2.BORDER_REFLECT_101)


def state_fraction(samples):
    """Estimate simulation position from original game constants, not B pixels."""
    if len(samples) != 3 or not samples[0].get("pipeline"):
        return None
    if any(s.get("pipeline") != samples[0]["pipeline"] or s.get("keys") != samples[0].get("keys") for s in samples):
        return None
    if not samples[0]["submission"] < samples[1]["submission"] < samples[2]["submission"]:
        return None
    rows = len(samples[0].get("keys", []))
    if not 1 <= rows <= 64 or any(len(s.get("words", [])) != rows or len(s.get("valid", [])) != rows for s in samples):
        return None
    values = [np.asarray(s["words"], dtype="<u4").view("<f4").astype(np.float64) for s in samples]
    if any(v.shape != (rows, 4) for v in values):
        return None
    a, b, c = values
    valid = np.ones((rows, 4), dtype=bool)
    for s, value in zip(samples, values):
        valid &= np.isfinite(value) & (np.abs(value) <= 1e6)
        valid &= (np.asarray(s["valid"], dtype=np.uint32)[:, None] & (1 << np.arange(4))) != 0
    span = c-a
    valid &= np.abs(span) > np.maximum(1, np.maximum(np.abs(a), np.abs(c)))*1e-6
    ratios = np.divide(b-a, span, out=np.zeros_like(span), where=valid)
    valid &= (ratios > 0) & (ratios < 1)
    if valid.sum() < 4 or np.count_nonzero(valid.any(axis=1)) < 2:
        return None
    observed = ratios[valid]
    fraction = float(np.median(observed))
    spread = float(np.quantile(observed, .75)-np.quantile(observed, .25))
    if not .15 <= fraction <= .85 or spread > .03:
        return None
    return {"fraction": fraction, "interquartile_spread": spread, "components": int(valid.sum()),
            "source": "original_game_float_constants"}


_linear_tables={}
def linear_table(divisor,subpixels=1):
    key=(divisor,subpixels)
    if key not in _linear_tables:
        _linear_tables[key]=cv2.pow(np.arange(divisor*subpixels+1,dtype=np.float64)/(divisor*subpixels),2.2).reshape(-1)
    return _linear_tables[key]

class PackedImage:
    """Integer display pixels (or exact unscaled bilinear floats), decoded by stripe."""
    ndim=3
    def __init__(self, raw, divisor=255, bilinear=False):
        self.raw=np.ascontiguousarray(raw)
        self.divisor=divisor
        self.bilinear=bilinear
        self.shape=self.raw.shape
    def __getitem__(self, region):
        return self.raw[region].astype(np.float64)/self.divisor
    def linear_region(self, region):
        raw=self.raw[region]
        if np.issubdtype(raw.dtype,np.integer):return linear_table(self.divisor)[raw]
        if self.bilinear:return linear_table(self.divisor,1024)[(raw*1024).astype(np.int32)]
        return cv2.pow(self[region],2.2)

class BlendedImage:
    ndim=3
    def __init__(self, first, second, fraction):
        self.first,self.second,self.fraction=first,second,fraction
        self.shape=first.shape
    def __getitem__(self, region):
        first=self.first[region];first*=1-self.fraction
        second=self.second[region];second*=self.fraction
        first+=second
        return first

def normalized_valid(pixels):
    if isinstance(pixels,PackedImage):
        return np.isfinite(pixels.raw).all() and pixels.raw.min()>=0 and pixels.raw.max()<=pixels.divisor
    return np.isfinite(pixels).all() and pixels.min()>=0 and pixels.max()<=1

def image_luma(pixels, byte=False):
    h,w=pixels.shape[:2]
    result=np.empty((h,w),dtype=np.uint8 if byte else np.float32)
    weights=np.array([.2126,.7152,.0722])
    for row in range(0,h,128):
        values=pixels[row:row+128]@weights
        result[row:row+128]=np.clip(values*255,0,255).astype(np.uint8) if byte else values.astype(np.float32)
    return result

def remap_image(pixels,x,y,*,packed=False):
    if isinstance(pixels,PackedImage):
        if not packed:return cv2.remap(pixels[:],x,y,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
        # INTER_LINEAR uses 5-bit fractional coordinates. Integer 8/10-bit
        # samples and their 1/1024 weighted sums are exactly representable in
        # float32. Normalize AFTER interpolation to avoid precision loss.
        raw=cv2.remap(pixels.raw.astype(np.float32),x,y,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
        return PackedImage(raw,pixels.divisor,bilinear=True)
    return cv2.remap(pixels,x,y,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)

def image(path, shared=None):
    if path.suffix.lower() != ".json":
        return PackedImage(np.asarray(metrics["Image"].open(path).convert("RGB"),dtype=np.uint8)), {}
    metadata = json.loads(path.read_text(encoding="utf-8"))
    if (not metadata.get("readback_complete") or not metadata.get("color_space_known") or
            metadata.get("gpu_features_only") or metadata.get("present_hresult") != 0):
        raise ValueError("Readback/color-space contract unavailable")
    if metadata.get("color_space") != 0:
        raise ValueError("Live critic currently admits SDR G22/P709 only")
    width, height = metadata["width"], metadata["height"]
    if not 11 <= width <= 7680 or not 11 <= height <= 4320:
        raise ValueError("Image extent")
    payload = path.parent / metadata["pixel_file"]
    if payload.parent.resolve() != path.parent.resolve():
        raise ValueError("Pixel payload must be local to the capture")
    raw = np.frombuffer(shared if shared is not None else payload.read_bytes(), np.uint8)
    if raw.size != width * height * 4:
        raise ValueError("Image payload size")
    fmt = metadata["dxgi_format"]
    if fmt in (28, 87):
        rgb = raw.reshape(height, width, 4)[:, :, :3]
        if fmt == 87:
            rgb = rgb[:, :, ::-1]
        return PackedImage(rgb.copy()), metadata
    if fmt == 24:
        packed = raw.view("<u4").reshape(height, width)
        return PackedImage(np.stack([packed & 1023, (packed >> 10) & 1023, (packed >> 20) & 1023], axis=2).astype(np.uint16),1023), metadata
    raise ValueError("Unsupported display format")


def assess(before, candidate, after, fraction=.5, profile="balanced", return_images=False):
    if before.shape != candidate.shape or before.shape != after.shape or not 0 < fraction < 1:
        raise ValueError("Matched images and an interior reference time required")
    if any(not normalized_valid(pixels) for pixels in (before,candidate,after)):
        raise ValueError("Finite normalized pixels required")
    height, width = before.shape[:2]
    # Preserve small edges at the native 1080p acceptance resolution. DIS gives
    # materially better reference alignment than low-resolution Farneback on
    # the moving fixture; confidence limits remain unchanged.
    scale = min(1, 1920/width)
    size = (max(16, round(width*scale)), max(16, round(height*scale)))
    def gray(pixels):
        value = image_luma(pixels,byte=True)
        return cv2.resize(value, size, interpolation=cv2.INTER_AREA)
    a, b = gray(before), gray(after)
    informative = float(a.std()) >= 2 and float(b.std()) >= 2
    forward = optical_flow(a,b)
    backward = optical_flow(b,a)
    forward = cv2.resize(forward, (width, height), interpolation=cv2.INTER_LINEAR)
    backward = cv2.resize(backward, (width, height), interpolation=cv2.INTER_LINEAR)
    forward[:, :, 0] *= width/size[0]; forward[:, :, 1] *= height/size[1]
    backward[:, :, 0] *= width/size[0]; backward[:, :, 1] *= height/size[1]
    x, y = np.meshgrid(np.arange(width, dtype=np.float32), np.arange(height, dtype=np.float32))
    def inverse(flow, t):
        sx, sy = x.copy(), y.copy()
        for _ in range(4):
            sampled = cv2.remap(flow, sx, sy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            sx, sy = x-t*sampled[:, :, 0], y-t*sampled[:, :, 1]
        return sx, sy
    ax, ay = inverse(forward, fraction)
    bx, by = inverse(backward, 1-fraction)
    fa = cv2.remap(forward, ax, ay, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    back_at_end = cv2.remap(backward, ax+fa[:, :, 0], ay+fa[:, :, 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    consistent = np.linalg.norm(fa+back_at_end, axis=2) <= 1.5
    motion = float(np.quantile(np.linalg.norm(forward, axis=2), .99))
    del fa, back_at_end, forward, x, y
    inside = (ax >= 0) & (ay >= 0) & (ax < width-1) & (ay < height-1) & (bx >= 0) & (by >= 0) & (bx < width-1) & (by < height-1)
    flow_coverage = float((inside & consistent).mean())
    warp_a = remap_image(before,ax,ay)
    warp_b = remap_image(after,bx,by)
    del bx,by
    # Motion is not identifiable on flat surfaces (the aperture problem), but
    # their predicted colour can still be identifiable. Accept that evidence
    # only when BOTH original references are locally flat and agree closely.
    # Candidate pixels never contribute to this confidence mask.
    la = image_luma(warp_a)
    lb = image_luma(warp_b)
    kernel = np.ones((3, 3), np.uint8)
    flat = ((cv2.dilate(la, kernel)-cv2.erode(la, kernel) <= 2/255) &
            (cv2.dilate(lb, kernel)-cv2.erode(lb, kernel) <= 2/255))
    del la, lb
    agreement=np.empty((height,width),dtype=bool)
    for row in range(0,height,128):
        region=slice(row,row+128)
        linear_a=warp_a.linear_region(region) if isinstance(warp_a,PackedImage) else cv2.pow(warp_a[region],2.2)
        linear_b=warp_b.linear_region(region) if isinstance(warp_b,PackedImage) else cv2.pow(warp_b[region],2.2)
        agreement[region]=np.max(cv2.absdiff(linear_a,linear_b),axis=2)<=.001
    del linear_a,linear_b
    coverage = float((inside & (consistent | (flat & agreement))).mean())
    flat_fraction=float((inside & flat & agreement).mean())
    del inside, flat, agreement, consistent
    if not return_images: del ax, ay, backward
    reference_check = compare_striped(warp_a, warp_b, filter_fn=fast_gaussian)
    if isinstance(warp_a,PackedImage):
        predicted=BlendedImage(warp_a,warp_b,fraction)
    else:
        warp_a*=1-fraction;warp_b*=fraction;warp_a+=warp_b
        predicted=warp_a
    del warp_a, warp_b
    if isinstance(candidate,PackedImage):candidate=candidate[:]
    quality = compare_striped(predicted, candidate, filter_fn=fast_gaussian)
    confident = (informative and coverage >= .95 and motion <= 64 and reference_check["ssim_gaussian_luma"] >= .99 and
                 reference_check["mean_linear_rgb_error"] <= .002)
    result = {"schema": 2, "quality_profile": profile, "quality_limits": PROFILES[profile], "reference_kind": "motion_interpolated_originals", "same_state_replay": False,
            "flow_method": "opencv_dis_medium", "flow_width": size[0], "opencv_version": cv2.__version__,
            "matched_reference": confident, "alignment_coverage": coverage, "motion_p99_pixels": motion,
            "flow_consistency_coverage": flow_coverage,
            "flat_reference_agreement_fraction": flat_fraction,
            "informative_reference": informative,
            "reference_check": reference_check, "quality": quality,
            "accepted_quality": confident and accepts(quality, profile)}
    if return_images:
        residual=np.empty(candidate.shape,dtype=np.float32)
        for row in range(0,height,128):
            region=slice(row,row+128)
            linear=candidate.linear_region(region) if isinstance(candidate,PackedImage) else cv2.pow(candidate[region],2.2)
            residual[region]=cv2.subtract(linear,cv2.pow(predicted[region],2.2))
        return result, residual, {"backward":backward,"origin_x":ax,"origin_y":ay}
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path); parser.add_argument("candidate", type=Path); parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--profile", choices=list(PROFILES), default="balanced")
    args = parser.parse_args()
    a, am = image(args.before); b, bm = image(args.candidate); c, cm = image(args.after)
    fraction = .5
    if am and bm and cm:
        times = [m["capture_qpc"] for m in (am, bm, cm)]
        if not times[0] < times[1] < times[2]:
            raise ValueError("Capture ordering")
        fraction = (times[1]-times[0])/(times[2]-times[0])
    timing = {"source": "present_qpc" if am and bm and cm else "midpoint", "fraction": fraction}
    if args.state:
        estimated = state_fraction(json.loads(args.state.read_text(encoding="utf-8")))
        if estimated:
            timing = estimated
            fraction = estimated["fraction"]
    result = assess(a, b, c, fraction, args.profile)
    result["reference_timing"] = timing
    with args.output.open("x") as output:
        json.dump(result, output, indent=2)
    print(json.dumps({k: result[k] for k in ("matched_reference", "accepted_quality", "alignment_coverage")}))


def temporal(first, second, error_first, error_second):
    h,w=first.shape[:2]
    def gray(a): return np.clip((a @ np.array([.2126,.7152,.0722]))*255,0,255).astype(np.uint8)
    a,b=gray(first),gray(second)
    flow=optical_flow(b,a)
    reverse=optical_flow(a,b)
    x,y=np.meshgrid(np.arange(w,dtype=np.float32),np.arange(h,dtype=np.float32))
    sx,sy=x+flow[:,:,0],y+flow[:,:,1]
    back=cv2.remap(reverse,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    inside=(sx>=0)&(sy>=0)&(sx<w-1)&(sy<h-1)
    consistent=np.linalg.norm(flow+back,axis=2)<=1.5
    warped_reference=cv2.remap(first,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    kernel=np.ones((3,3),np.uint8)
    warped_gray=cv2.remap(a,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    flat=(cv2.dilate(warped_gray,kernel)-cv2.erode(warped_gray,kernel)<=2)&(cv2.dilate(b,kernel)-cv2.erode(b,kernel)<=2)
    agreement=np.max(np.abs(warped_reference**2.2-second**2.2),axis=2)<=.001
    valid=inside&(consistent|(flat&agreement))
    del warped_reference,warped_gray,back,reverse
    coverage=float(valid.mean())
    warped=cv2.remap(error_first,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    error=np.abs(error_second-warped).mean(axis=2)
    rows,cols=np.arange(0,h,8),np.arange(0,w,8)
    sums=np.add.reduceat(np.add.reduceat(error,rows,axis=0),cols,axis=1)
    counts=np.minimum(8,h-rows)[:,None]*np.minimum(8,w-cols)[None,:]
    return {"p99_tile_error":float(np.quantile(sums/counts,.99)),"mean_error":float(error.mean()),
            "alignment_coverage":coverage,"matched_reference":coverage>=.95 and float(np.quantile(np.linalg.norm(flow,axis=2),.99))<=64,
            "flow_source":"original_interpolated_references_only"}


def assess_request(request):
    if request.get("schema")!=1 or request.get("profile") not in PROFILES:
        raise ValueError("Worker protocol/profile")
    paths=[Path(p) for p in request["paths"]]
    if len(paths) not in (3,5): raise ValueError("Three or five images required")
    maps=[]
    try:
        shared=request.get("shared",[])
        if shared and len(shared)!=len(paths): raise ValueError("Shared image count")
        for entry in shared:
            size=int(entry["bytes"])
            if not 0<size<=256*1024*1024 or not entry["name"].startswith("Local\\ARC-quality-"):
                raise ValueError("Shared image contract")
            maps.append(mmap.mmap(-1,size,tagname=entry["name"],access=mmap.ACCESS_READ))
        def load(i): return image(paths[i],maps[i] if maps else None)
        if len(paths)!=5: raise ValueError("Temporal approval requires five frames")
        states=request.get("frame_states",[]);results=[];previous_error=previous_backward=None;first_fraction=0.;temporal_result=None
        for offset in (0,2):
            a,am=load(offset);b,bm=load(offset+1);c,cm=load(offset+2)
            times=[m["capture_qpc"] for m in (am,bm,cm)]
            if not times[0]<times[1]<times[2]:raise ValueError("Capture ordering")
            estimate=state_fraction(states[offset:offset+3]);fraction=estimate["fraction"] if estimate else (times[1]-times[0])/(times[2]-times[0])
            result,residual,coordinates=assess(a,b,c,fraction,request["profile"],True)
            del a,b,c
            result["reference_timing"]=estimate or dict(source="present_qpc",fraction=fraction)
            results.append(result)
            if not result["matched_reference"] or not result["accepted_quality"]:
                # A rejected first image cannot be rescued by temporal averaging.
                return dict(result,samples=results,temporal=None,complete_sequence=False,reason="reference_unmatched" if not result["matched_reference"] else "quality_threshold_failed")
            if previous_error is not None:
                ax,ay=coordinates["origin_x"],coordinates["origin_y"]
                flow=cv2.remap(previous_backward,ax,ay,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
                sx,sy=ax+(1-first_fraction)*flow[:,:,0],ay+(1-first_fraction)*flow[:,:,1]
                warped=cv2.remap(previous_error,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
                error=np.abs(residual-warped).mean(axis=2);h,w=error.shape;rows,cols=np.arange(0,h,8),np.arange(0,w,8)
                sums=np.add.reduceat(np.add.reduceat(error,rows,axis=0),cols,axis=1);counts=np.minimum(8,h-rows)[:,None]*np.minimum(8,w-cols)[None,:]
                inside=float(((sx>=0)&(sy>=0)&(sx<w-1)&(sy<h-1)).mean())
                temporal_result=dict(p99_tile_error=float(np.quantile(sums/counts,.99)),mean_error=float(error.mean()),alignment_coverage=min(inside,result["alignment_coverage"],results[0]["alignment_coverage"]),matched_reference=inside>=.95,flow_source="original_triplet_flows_composed_through_shared_middle_original")
            else:
                first_fraction=fraction;previous_backward=coordinates["backward"]
            previous_error=residual
            del coordinates
        combined=dict(schema=2,quality_profile=request["profile"],quality_limits=PROFILES[request["profile"]],samples=results,temporal=temporal_result,complete_sequence=True,reference_kind="five_frame_original_triplets",same_state_replay=False)
        combined["quality"]={key:(min if key=="ssim_gaussian_luma" else max)(r["quality"][key] for r in results)
            for key in ("ssim_gaussian_luma","mean_linear_rgb_error","p99_tile_linear_rgb_error","worst_tile_linear_rgb_error","peak_linear_rgb_error")}
        combined["matched_reference"]=all(r["matched_reference"] for r in results) and temporal_result["matched_reference"]
        combined["accepted_quality"]=combined["matched_reference"] and temporal_result["p99_tile_error"]<=PROFILES[request["profile"]]["temporal"]
        combined["reason"]="accepted_quality" if combined["accepted_quality"] else "reference_unmatched" if not combined["matched_reference"] else "quality_threshold_failed"
        return combined
    finally:
        for mapping in maps: mapping.close()


def serve():
    for line in sys.stdin:
        if len(line)>65536: raise ValueError("Worker request capacity")
        request=json.loads(line)
        try:
            result=assess_request(request)
        except Exception as error:
            result={"accepted_quality":False,"matched_reference":False,"quality":{},"reason":"quality_worker_error","detail":str(error)[:512],"error_location":traceback.format_exc(limit=2)[-2048:]}
        result.update(request_id=request.get("request_id"),context=request.get("context"),worker_pid=os.getpid())
        print(json.dumps(result,separators=(",",":")),flush=True)


if __name__ == "__main__":
    if sys.argv[1:]==["--serve"]: serve()
    else: main()
