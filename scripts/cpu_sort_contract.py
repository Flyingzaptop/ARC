"""Bounded snapshot preflight, NOT permission to replace live application memory.

The caller must independently establish comparator/algorithm equivalence, exclusive
ownership through commit, and continuation state. A private heap page or TLS root
alone does not establish these properties. No execution certificate is minted here.
"""
from collections import Counter

MAX_BYTES = 4 * 1024 * 1024


def validate_projection(bits, record_bytes=16):
    if not isinstance(bits, (list, tuple)) or len(bits) != 64:
        raise ValueError('Expected a 64-bit projection')
    if any(type(x) is not int or x < -2 or x >= record_bytes * 8 for x in bits):
        raise ValueError('Invalid projected bit')


def key(record, bits):
    value = int.from_bytes(record, 'little')
    return sum((1 if b == -2 else 0 if b == -1 else (value >> b) & 1) << i
               for i, b in enumerate(bits))


def preflight(snapshot, bits, begin=0, end=None):
    """Evaluate data guards on an owned snapshot; no live-pointer dereference.

    Ties are byte-order independent iff every record in a key group is identical.
    This is stricter than observational equivalence, but needs no consumer oracle.
    The proposed bounded insertion path also requires <=32 records and a minimum
    first key. Its machine-code and output-state proof is deliberately still open.
    """
    validate_projection(bits)
    end = begin + len(snapshot) if end is None else end
    if (type(begin) is not int or type(end) is not int or begin < 0 or
            end < begin or end >= 1 << 64 or begin % 16 or
            end - begin != len(snapshot) or len(snapshot) % 16 or
            len(snapshot) > MAX_BYTES):
        return {'input_guards_pass': False, 'reason': 'invalid_or_over_budget_span',
                'replacement_admitted': False}
    records = [snapshot[i:i + 16] for i in range(0, len(snapshot), 16)]
    representatives = {}
    conflicts = set()
    tie_difference_bits = 0
    keys = []
    for r in records:
        k = key(r, bits)
        keys.append(k)
        if k in representatives and representatives[k] != r:
            conflicts.add(k)
            tie_difference_bits |= int.from_bytes(representatives[k], 'little') ^ int.from_bytes(r, 'little')
        representatives.setdefault(k, r)
    ties_safe = not conflicts
    bounded = 2 <= len(records) <= 32
    first_minimum = bool(keys) and keys[0] == min(keys)
    return {'records': len(records), 'distinct_keys': len(representatives),
            'ambiguous_tie_groups': len(conflicts),
            'differing_record_bits_within_ties': [i for i in range(128) if tie_difference_bits & (1 << i)],
            'tie_permutation_byte_invariant': ties_safe,
            'bounded_insertion_size': bounded, 'first_key_minimum': first_minimum,
            'input_guards_pass': ties_safe and bounded and first_minimum,
            'replacement_admitted': False,
            'remaining_obligations': ['all_path_machine_semantics',
                                      'exclusive_ownership_until_commit',
                                      'continuation_state_and_external_effects']}


def compare_snapshot(before, after, bits):
    validate_projection(bits)
    if len(before) != len(after) or len(before) % 16 or len(before) > MAX_BYTES:
        raise ValueError('Invalid snapshot pair')
    a = [before[i:i + 16] for i in range(0, len(before), 16)]
    b = [after[i:i + 16] for i in range(0, len(after), 16)]
    stable = sorted(a, key=lambda r: key(r, bits))
    keys_b = [key(r, bits) for r in b]
    multiset = Counter(a) == Counter(b)
    ordered = all(x <= y for x, y in zip(keys_b, keys_b[1:]))
    mismatch = sum(x != y for x, y in zip(stable, b))
    return {'same_multiset': multiset, 'nondecreasing_unsigned_keys': ordered,
            'stable_sort_position_mismatches': mismatch,
            'differences_only_within_equal_key_groups': multiset and ordered and
                all(key(x, bits) == key(y, bits) for x, y in zip(stable, b)),
            'byte_result_equal': stable == b,
            'observable_result_equal': True if stable == b else None,
            'observable_scope': 'output array only; caller state and external effects separate'}
