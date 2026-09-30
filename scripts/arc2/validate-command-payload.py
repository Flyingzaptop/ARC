"""Check retained ARC2 command arguments after a new frontend build.

This validates API-intent capture, not GPU execution or replay completeness.
"""
from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path


KIND = {0: "draw", 1: "draw_indexed", 2: "dispatch", 3: "indirect"}


def inspect(data: dict, required: set[str], expected_dispatch: tuple[int, int, int] | None) -> dict:
    failures: list[str] = []
    warnings: list[str] = []
    counts: Counter[str] = Counter()
    indirect_unresolved = 0
    dispatch_values: Counter[tuple[int, int, int]] = Counter()
    if data.get("schema") != 1 or data.get("command_payload_version") != 1:
        failures.append("expected schema=1 and command_payload_version=1")
    work = data.get("work")
    if not isinstance(work, list):
        failures.append("work array is absent")
        work = []
    for item in work:
        if not isinstance(item, dict):
            failures.append("work entry is not an object")
            continue
        kind = KIND.get(item.get("kind"))
        args = item.get("arguments")
        state = item.get("state")
        if not isinstance(state, dict):
            failures.append(f"work {item.get('id')} has no state snapshot")
            continue
        for field in ("vb", "ib", "scissors", "scissors_known"):
            if field not in state:
                failures.append(f"work {item.get('id')} lacks IA/RS field {field}")
        if isinstance(state.get("vb"), list):
            for binding in state["vb"]:
                if not isinstance(binding, list) or len(binding) != 5:
                    failures.append(f"work {item.get('id')} lacks vertex stride slot")
        if "ib" in state and (not isinstance(state["ib"], list) or len(state["ib"]) != 5):
            failures.append(f"work {item.get('id')} lacks index format/namespace slots")
        if isinstance(state.get("scissors"), list):
            if any(not isinstance(rect, list) or len(rect) != 4 for rect in state["scissors"]):
                failures.append(f"work {item.get('id')} has malformed literal scissor")
        if kind is None:
            continue
        counts[kind] += 1
        if not isinstance(args, dict):
            failures.append(f"work {item.get('id')} has no arguments object")
            continue
        if kind in ("draw", "draw_indexed"):
            fields = ("count_per_instance", "instance_count", "start_vertex_or_index",
                      "base_vertex", "start_instance")
            if args.get("type") != "draw" or args.get("known") is not True or any(
                not isinstance(args.get(field), int) for field in fields
            ):
                failures.append(f"work {item.get('id')} lacks exact draw payload")
            elif kind == "draw" and args["base_vertex"] != 0:
                failures.append(f"non-indexed work {item.get('id')} has nonzero base_vertex")
        elif kind == "dispatch":
            if args.get("type") != "dispatch" or args.get("known") is not True or any(
                not isinstance(args.get(field), int) for field in ("x", "y", "z")
            ):
                failures.append(f"work {item.get('id')} lacks exact dispatch payload")
            else:
                dispatch_values[(args["x"], args["y"], args["z"])] += 1
        else:
            fields = ("signature", "max_count", "argument_buffer", "argument_offset",
                      "count_buffer", "count_offset")
            if args.get("type") != "indirect" or any(
                not isinstance(args.get(field), int) for field in fields
            ) or not isinstance(args.get("count_buffer_present"), bool):
                failures.append(f"work {item.get('id')} lacks typed indirect payload")
            elif args.get("known") is True and (not args["signature"] or
                    not args["argument_buffer"] or
                    (args["count_buffer_present"] and not args["count_buffer"])):
                failures.append(f"work {item.get('id')} claims known unresolved indirect IDs")
            elif args.get("known") is not True:
                indirect_unresolved += 1
    for kind in sorted(required):
        if counts[kind] == 0:
            failures.append(f"required {kind} work absent from retained history")
    if expected_dispatch and dispatch_values[expected_dispatch] == 0:
        failures.append(f"expected dispatch payload {expected_dispatch} absent")
    if data.get("history_truncated") or data.get("dropped"):
        warnings.append("history is truncated; absent command kinds cannot be inferred globally")
    if data.get("incomplete"):
        warnings.append("IR is incomplete; typed arguments do not prove dependency closure")
    return {
        "schema": "arc2-command-payload-check-v1",
        "passed": not failures,
        "retained_work": len(work),
        "global_total_work": data.get("total_work"),
        "retained_command_counts": dict(counts),
        "unresolved_indirect_work": indirect_unresolved,
        "history_truncated": bool(data.get("history_truncated")),
        "failures": failures,
        "warnings": warnings,
        "scope": "retained API-intent records only; no GPU execution, shader effects, or replay proof",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("ir", type=Path)
    parser.add_argument("--require-kind", choices=tuple(KIND.values()), action="append", default=[])
    parser.add_argument("--expect-dispatch", metavar="X,Y,Z")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    expected = None
    if args.expect_dispatch:
        numbers = [int(value) for value in args.expect_dispatch.split(",")]
        if len(numbers) != 3:
            parser.error("--expect-dispatch needs X,Y,Z")
        expected = tuple(numbers)
    result = inspect(json.loads(args.ir.read_text(encoding="utf-8-sig")),
                     set(args.require_kind), expected)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
