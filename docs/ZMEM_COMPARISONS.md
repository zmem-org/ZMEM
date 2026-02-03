# ZMEM Comparison Overview

A brief comparison of ZMEM with other binary serialization formats.

## Feature Matrix

| Feature | ZMEM | FlatBuffers | Cap'n Proto | Protocol Buffers |
|---------|------|-------------|-------------|------------------|
| Zero-copy read | Yes | Yes | Yes | No |
| Direct memcpy (fixed structs) | Yes | No | No | No |
| Schema evolution | No | Yes | Yes | Yes |
| Fixed struct overhead | **0 bytes** | 0 bytes (struct) / ~4 bytes (table) | ~8 bytes | 1-2 bytes/field |
| Variable struct overhead | 8 bytes | ~4 bytes (table) | ~8 bytes | 1-2 bytes/field |
| Fixed array overhead | **0 bytes** | 0 bytes (struct fields) | 8 bytes (List) | 1-2 bytes/element (repeated) |
| Dynamic array overhead | 8 bytes | 4 bytes | 8 bytes | 1-2 bytes + length |
| Random access | Yes | Yes | Yes | No |
| Native mutable state | Yes | No | No | Yes |
| Max document size | 2^64 bytes | ~2 GB (32-bit offsets) | 2^64 bytes (multi-segment) | ~2 GB (default) |
| Max array elements | 2^64 | ~2^32 | ~2^29 per List | ~2^32 (varint) |

## Wire Size Comparison

### Fixed Struct: `Point { x: f32, y: f32 }`

| Format | Wire Size | Overhead |
|--------|-----------|----------|
| ZMEM | **8 bytes** | 0% |
| Protocol Buffers | 10 bytes | 25% |
| FlatBuffers | 12 bytes | 50% |
| Cap'n Proto | 24 bytes | 200% |

### Fixed Struct: `Particle` (9 fields, 40 bytes of data)

| Format | Wire Size | Overhead |
|--------|-----------|----------|
| ZMEM | **40 bytes** | 0% |
| Protocol Buffers | 49 bytes | 22.5% |
| FlatBuffers | ~44 bytes | 10% |
| Cap'n Proto | ~56 bytes | 40% |

## Random Access Support

| Format | Random Access | Method |
|--------|---------------|--------|
| ZMEM | Yes | Direct offsets (compile-time for fixed structs) |
| FlatBuffers | Yes | vtables + pointers |
| Cap'n Proto | Yes | Relative pointers |
| Protocol Buffers | No | Must parse entire message first |
| SBE | No | Preorder traversal required |

## Memory Model

| Format | Build Model | Mutable State | Long-lived Modifications |
|--------|-------------|---------------|--------------------------|
| ZMEM (fixed) | Native struct | Yes | No memory leaks |
| ZMEM (variable) | Native types → serialize | Yes | No memory leaks |
| FlatBuffers | Arena (FlatBufferBuilder) | No | Arena bloat |
| Cap'n Proto | Arena (MessageBuilder) | No | Arena bloat |
| Protocol Buffers | Heap allocation | Yes | Works normally |

## When to Use Each Format

| Use Case | Recommended Format |
|----------|-------------------|
| IPC / Shared memory | ZMEM |
| Game networking (fixed messages) | ZMEM |
| High-frequency trading | ZMEM, SBE |
| Memory-mapped files | ZMEM, FlatBuffers, Cap'n Proto |
| Microservices (independent deployment) | Protocol Buffers, Cap'n Proto, BEVE |
| Schema evolution required | Protocol Buffers, FlatBuffers, Cap'n Proto, BEVE |
| Browser/server communication | JSON |

## Detailed Comparisons

For in-depth technical comparisons, see:
- [ZMEM vs Cap'n Proto](ZMEM_VS_CAPNPROTO.md)
- [ZMEM vs FlatBuffers](ZMEM_VS_FLATBUFFERS.md)
