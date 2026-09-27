"""Generic GPU prefix/scatter emission and independent byte oracle."""
import hashlib
import json
import struct
from pathlib import Path
from cpu_composite_shared import Unsupported

def generate_compaction(output, *, max_count=65536):
    """Emit a generic three-dispatch 256-lane GPU prefix/scatter primitive.

    It consumes scratch flags and records; it cannot make the upstream captured
    input binding or alternate filter paths valid by itself.
    """
    if not 1 <= max_count <= 65536:
        raise Unsupported('prefix group count budget')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    shaders = {
        'prefix-local.hlsl': r'''// Exclusive prefix within each 256-lane group.
ByteAddressBuffer Scratch : register(t0);
RWByteAddressBuffer LocalPrefix : register(u0);
RWByteAddressBuffer GroupSums : register(u1);
RWByteAddressBuffer GroupInvalids : register(u2);
cbuffer Parameters : register(b0) { uint Count; uint ScratchWords; uint RecordWords; };
groupshared uint scan[256];
groupshared uint invalidScan[256];
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 group : SV_GroupID) {
  uint valid=0u, deltaLo=0u, deltaHi=0u;
  if (id.x<Count) { uint row=id.x*ScratchWords;
    valid=Scratch.Load(row*4);
    deltaLo=Scratch.Load((row+1u+RecordWords)*4);
    deltaHi=Scratch.Load((row+2u+RecordWords)*4); }
  uint emit=(valid==1u && deltaLo==RecordWords*4u && deltaHi==0u) ? 1u : 0u;
  uint invalid=(id.x<Count && (valid!=1u || deltaHi!=0u ||
    (deltaLo!=0u && deltaLo!=RecordWords*4u))) ? 1u : 0u;
  scan[tid.x]=emit; invalidScan[tid.x]=invalid; GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint offset=1u; offset<256u; offset<<=1u) {
    uint add = tid.x >= offset ? scan[tid.x-offset] : 0u;
    uint bad = tid.x >= offset ? invalidScan[tid.x-offset] : 0u;
    GroupMemoryBarrierWithGroupSync();
    scan[tid.x] += add; invalidScan[tid.x] += bad; GroupMemoryBarrierWithGroupSync();
  }
  if (id.x < Count) LocalPrefix.Store(id.x*4, scan[tid.x]-emit);
  if (tid.x == 255u) { GroupSums.Store(group.x*4, scan[255]);
    GroupInvalids.Store(group.x*4, invalidScan[255]); }
}
''',
        'prefix-groups.hlsl': r'''// At most 256 group sums for at most 65536 elements.
ByteAddressBuffer GroupSums : register(t0);
ByteAddressBuffer GroupInvalids : register(t1);
RWByteAddressBuffer GroupOffsets : register(u0);
RWByteAddressBuffer Metadata : register(u1);
cbuffer Parameters : register(b0) { uint Count; uint RecordStride; uint InitialCursorLo; uint InitialCursorHi; };
groupshared uint scan[256];
groupshared uint invalidScan[256];
[numthreads(256,1,1)]
void main(uint3 tid : SV_GroupThreadID) {
  uint groups=(Count+255u)/256u;
  uint value=tid.x<groups ? GroupSums.Load(tid.x*4) : 0u;
  scan[tid.x]=value; invalidScan[tid.x]=tid.x<groups ? GroupInvalids.Load(tid.x*4) : 0u;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint offset=1u; offset<256u; offset<<=1u) {
    uint add=tid.x>=offset ? scan[tid.x-offset] : 0u;
    uint bad=tid.x>=offset ? invalidScan[tid.x-offset] : 0u;
    GroupMemoryBarrierWithGroupSync();
    scan[tid.x]+=add; invalidScan[tid.x]+=bad; GroupMemoryBarrierWithGroupSync();
  }
  if (tid.x<groups) GroupOffsets.Store(tid.x*4,scan[tid.x]-value);
  if (tid.x==255u) {
    uint emitted=scan[255]; uint delta=emitted*RecordStride;
    uint low=InitialCursorLo+delta;
    Metadata.Store(0,emitted);
    Metadata.Store(4,low);
    Metadata.Store(8,InitialCursorHi+uint(low<InitialCursorLo));
    Metadata.Store(12,invalidScan[255]);
  }
}
''',
        'scatter.hlsl': r'''// Each emitted row writes one disjoint slot assigned by GPU prefix.
ByteAddressBuffer Scratch : register(t0);
ByteAddressBuffer LocalPrefix : register(t1);
ByteAddressBuffer GroupOffsets : register(t2);
RWByteAddressBuffer PackedRecords : register(u0);
cbuffer Parameters : register(b0) { uint Count; uint ScratchWords; uint RecordWords; };
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x>=Count) return;
  uint source=id.x*ScratchWords;
  if (Scratch.Load(source*4)!=1u ||
      Scratch.Load((source+1u+RecordWords)*4)!=RecordWords*4u ||
      Scratch.Load((source+2u+RecordWords)*4)!=0u) return;
  uint slot=GroupOffsets.Load((id.x/256u)*4)+LocalPrefix.Load(id.x*4);
  [loop] for (uint j=0u;j<RecordWords;++j)
    PackedRecords.Store((slot*RecordWords+j)*4,Scratch.Load((source+1u+j)*4));
}
''',
    }
    for name, shader in shaders.items():
        (output/name).write_bytes(shader.encode('utf-8'))
    contract = {'schema': 1, 'algorithm': 'two_level_exclusive_prefix_then_parallel_scatter',
                'max_count': max_count, 'group_size': 256,
                'dispatches': ['prefix-local', 'prefix-groups', 'scatter'],
                'group_count': '(Count+255)/256, at most 256',
                'scratch_input': 'per-row validity word, record words, then 64-bit normalized cursor delta',
                'buffers': {'local_prefix_words': 'Count', 'group_sums_words': 'ceil(Count/256)',
                            'group_invalid_words': 'ceil(Count/256)',
                            'group_offsets_words': 'ceil(Count/256)',
                            'packed_record_words': 'Count*RecordWords worst case',
                            'metadata_words': ['emitted_count', 'final_cursor_low', 'final_cursor_high', 'invalid_count']},
                'row_semantics': {'emit': 'validity=1 and delta=RecordWords*4',
                                  'reject': 'validity=1 and delta=0',
                                  'invalid': 'validity!=1 or delta outside {0,RecordWords*4}'},
                'publication_guard': 'invalid_count must equal zero before any result publication',
                'cursor_recipe': 'one InitialCursor plus emitted_count*RecordStride on GPU',
                'requires_preflight': 'preflight_packet_no_growth for maximum Count*RecordStride and ownership before dispatch',
                'output_slot_source': 'GPU exclusive prefix of validity flags',
                'cpu_prefix_required': False, 'live_replacement_allowed': False,
                'limitations': ['requires ordered dispatch barriers and independent GPU byte validation',
                                'invalid_count readback or GPU consumer guard is required before publication',
                                'upstream scratch validity and input binding are not closed for mixed filter paths',
                                'no publication or fallback after partial GPU effects is provided'],
                'shader_sha256': {name: hashlib.sha256((output/name).read_bytes()).hexdigest()
                                  for name in shaders}}
    (output/'contract.json').write_text(json.dumps(contract, indent=2) + '\n', encoding='utf-8')
    return contract


def compaction_reference(scratch, count, scratch_words, record_words, initial_cursor):
    """Independent byte oracle for the three GPU prefix/scatter dispatches."""
    if (not 0 <= count <= 65536 or scratch_words < 3 + record_words or
            record_words < 1 or len(scratch) != count * scratch_words * 4 or
            not 0 <= initial_cursor < 1 << 64):
        raise Unsupported('compaction reference shape')
    packed = bytearray()
    slots = []
    invalid_count = 0
    for i in range(count):
        row = scratch[i * scratch_words * 4:(i + 1) * scratch_words * 4]
        valid = struct.unpack_from('<I', row)[0]
        delta = struct.unpack_from('<Q', row, (1 + record_words) * 4)[0]
        emit = valid == 1 and delta == record_words * 4
        invalid_count += int(valid != 1 or delta not in (0, record_words * 4))
        slots.append(len(packed) // (record_words * 4) if emit else None)
        if emit:
            packed.extend(row[4:4 + record_words * 4])
    emitted = len(packed) // (record_words * 4)
    final_cursor = initial_cursor + emitted * record_words * 4
    if final_cursor >= 1 << 64:
        raise Unsupported('compacted cursor overflow')
    return {'slots': slots, 'packed_records': bytes(packed), 'emitted_count': emitted,
            'invalid_count': invalid_count, 'publication_allowed': invalid_count == 0,
            'final_cursor': final_cursor,
            'metadata': struct.pack('<IQI', emitted, final_cursor, invalid_count)}
