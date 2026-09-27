"""Shared types for bounded captured CPU-to-GPU experiments."""

class Unsupported(ValueError):
    pass

WIDTH = {'u8': 1, 'u16': 2, 'u32': 4, 'f32': 4,
         'u64': 8, 'u32x4': 16, 'f32x4': 16, 'bytes3': 3}
LEAVES = {'memory_input', 'entry_register', 'entry_vector'}
