"""Compact ARC IR dump with provenance and retained-history limits."""
from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def semantic_counts(data: dict) -> dict:
    work = data.get('work', [])
    certainty = Counter()
    intervals = 0
    for item in work:
        for access in item.get('access', []):
            certainty[access[2]] += 1
            if len(access) >= 9 and access[6]:
                intervals += 1
    submissions = {row[0]: row for row in data.get('submissions', [])}
    pending = [row[5] for row in data.get('presents', []) if row[5]]
    visited, linked, missing = set(), set(), set()
    while pending:
        sid = pending.pop()
        if sid in visited:
            continue
        visited.add(sid)
        submission = submissions.get(sid)
        if submission is None:
            missing.add(sid)
            continue
        linked.update(submission[2])
        pending.extend(submission[3])
    retained_ids = {item['id'] for item in work}
    total = sum(certainty.values())
    return {
        'access_edges': total,
        'known_access_edges': certainty[0],
        'symbolic_access_edges': certainty[1],
        'unknown_access_edges': certainty[2],
        'unknown_access_fraction': certainty[2] / total if total else None,
        'symbolic_or_unknown_access_fraction': (certainty[1] + certainty[2]) / total if total else None,
        'descriptor_interval_edges': intervals,
        'present_linked_retained_work': len(linked & retained_ids),
        'present_linked_unretained_work_ids': len(linked - retained_ids),
        'missing_reachable_submission_ids': len(missing),
        'recording_consistent_submissions': sum(bool(row[4]) for row in submissions.values()),
        'dependency_closure_proven': None,
        'scope': 'retained API-intent records; certainty is not executed shader effects; Present links are recorded submission reachability, not full resource-dependency closure',
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dump", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.dump.read_text(encoding="utf-8-sig"))
    work = data.get("work", [])
    retained = {
        "work_kind_counts": dict(Counter(str(item.get("kind")) for item in work)),
        "known_pipeline": sum(bool(item.get("state", {}).get("pipeline")) for item in work),
        "eligible": sum(item.get("eligible") is True for item in work),
        "unsupported": sum(item.get("supported") is False for item in work),
        "unknown_coverage": sum(bool(item.get("coverage")) for item in work),
    }
    result = {
        "schema": "arc2-ir-dump-summary-v1",
        "dump_sha256": digest(args.dump),
        "dump_bytes": args.dump.stat().st_size,
        "global_total_work": data.get("total_work"),
        "retained_work": len(work),
        "dropped_count_reported": data.get("dropped"),
        "history_truncated": data.get("history_truncated"),
        "incomplete": data.get("incomplete"),
        "uncertain_submissions": data.get("uncertain_submissions"),
        "objects": len(data.get("objects", [])),
        "resources": len(data.get("resources", [])),
        "descriptors": len(data.get("descriptors", [])),
        "presents": len(data.get("presents", [])),
        "shaders": data.get("shaders"),
        "root_signatures": data.get("root_signatures"),
        "accepted_actions": data.get("accepted_actions"),
        "rejected_actions": data.get("rejected_actions"),
        "interface_coverage": data.get("interface_coverage", []),
        "retained_history_statistics": retained,
        "retained_semantic_statistics": semantic_counts(data),
        "statistics_limit": "work-kind and pipeline counts describe only retained history when history_truncated=true",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{args.output}: work={result['global_total_work']} retained={result['retained_work']}")


if __name__ == "__main__":
    main()
