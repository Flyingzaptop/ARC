"""Prepare a replay pack; run the child only with an explicit --execute flag."""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from prepare import prepare_pack
from prepare_scatter import prepare_scatter_pack


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--pack', type=Path)
    parser.add_argument('--scatter', action='store_true',
                        help='Prepare a machine-derived word-scatter v3 pack')
    parser.add_argument('--exe', type=Path,
                        default=Path(__file__).with_name('build') / 'replay.exe')
    parser.add_argument('--timeout-seconds', type=float, default=20.0)
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--warm-repeats', type=int, default=0)
    parser.add_argument('--execute', action='store_true',
                        help='Run the isolated native child after explicit review')
    args = parser.parse_args()
    if args.pack is None:
        args.pack = Path(__file__).with_name(
            'scatter-pack.bin' if args.scatter else 'bulk-pack.bin')
    prepared = (prepare_scatter_pack(args.manifest, args.pack)
                if args.scatter else prepare_pack(args.manifest, args.pack))
    if not args.execute:
        print(json.dumps(prepared, indent=2))
        return
    if not args.exe.is_file():
        raise FileNotFoundError(args.exe)
    if not 0 < args.timeout_seconds <= 60:
        raise ValueError('timeout must be within (0,60] seconds')
    if not 1 <= args.repeats <= 10:
        raise ValueError('repeats must be within [1,10]')
    if not 0 <= args.warm_repeats <= 10:
        raise ValueError('warm-repeats must be within [0,10]')
    result = subprocess.run(
        [str(args.exe.resolve()), str(args.pack.resolve()), str(args.repeats),
         str(args.warm_repeats)],
        capture_output=True, text=True, timeout=args.timeout_seconds,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
        check=False,
    )
    print(json.dumps({
        'returncode': result.returncode,
        'stdout': result.stdout,
        'stderr': result.stderr,
        'timeout_seconds': args.timeout_seconds,
        'isolated_child_only': True,
    }, indent=2))
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
