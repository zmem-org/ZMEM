# ZMEM vs FlatBuffers: A Technical Comparison

This document provides a detailed technical comparison between **ZMEM** and **FlatBuffers**, two binary serialization formats designed for high-performance applications. Both prioritize zero-copy deserialization, but make fundamentally different trade-offs.

## Executive Summary

| Aspect | ZMEM | FlatBuffers |
|--------|-------|-------------|
| **Primary Goal** | Maximum performance, zero overhead | Zero-copy with schema evolution |
| **Schema Evolution** | ❌ Not supported | ✅ Fully supported |
| **Fixed Struct Overhead** | **0 bytes** | 4-8 bytes (vtable offset) |
| **Zero-Copy Read** | ✅ Yes | ✅ Yes |
| **Direct memcpy** | ✅ Yes (fixed structs) | ❌ No (except inline structs) |
| **Nested Vectors** | ✅ Yes | ✅ Yes |
| **Variable Strings** | ✅ Yes (`string` type) | ✅ Yes |
| **Best For** | IPC, HFT, game networking, real-time | RPC, file formats, cross-version compatibility |

---

## Design Philosophy

### ZMEM: "Same Schema, Maximum Speed"

ZMEM assumes **all communicating parties use identical schemas**. This enables:

- **Zero overhead** for fixed structs (direct `memcpy`)
- **No vtables** or field tags
- **Fixed offsets** known at compile time
- **Compile-time validation** via type signatures

The trade-off: **No schema evolution**. Adding, removing, or reordering fields requires recompiling all parties simultaneously.

### FlatBuffers: "Evolve Without Breaking"

FlatBuffers prioritizes **forward/backward compatibility**:

- **Vtables** store field offsets, enabling missing field detection
- **New fields** can be added without breaking old readers
- **Old fields** can be deprecated (readers see default values)
- **Field order** doesn't affect wire format

The trade-off: **Overhead** from vtables and indirection, even for fixed data.

---

## Wire Format Comparison

### Fixed Data: Point { x: f32, y: f32 }

#### ZMEM Wire Format (8 bytes)

```
┌─────────────────┬─────────────────┐
│        x        │        y        │
│     4 bytes     │     4 bytes     │
└─────────────────┴─────────────────┘
Offset 0: x (f32)
Offset 4: y (f32)
Total: 8 bytes
```

**Overhead: 0 bytes (0%)**

Serialization: `memcpy(buffer, &point, 8)`
Deserialization: `memcpy(&point, buffer, 8)` or `Point* p = (Point*)buffer`

#### FlatBuffers Wire Format (32 bytes typical)

FlatBuffers distinguishes between **structs** (inline, no vtable) and **tables** (vtable-based).

**As a FlatBuffers Struct** (inline, 8 bytes):
```
┌─────────────────┬─────────────────┐
│        x        │        y        │
│     4 bytes     │     4 bytes     │
└─────────────────┴─────────────────┘
```
FlatBuffers structs are similar to ZMEM fixed structs—but can only be used inline within tables, not as root objects.

**As a FlatBuffers Table** (root object, ~32 bytes):
```
┌──────────────────────────────────────────────────────────────┐
│ Root offset │ Vtable │ Table data                            │
└──────────────────────────────────────────────────────────────┘

Detailed layout:
Offset  Size  Content
------  ----  -------
0       4     soffset to root table (e.g., 8)
4       4     file_identifier (optional, often omitted)
8       4     soffset to vtable (e.g., -8, pointing to offset 0)
12      4     x = 1.0f
16      4     y = 2.0f
--- Vtable (at offset 0, referenced by table) ---
0       2     vtable size = 8
2       2     table size = 12
4       2     offset of x = 4
6       2     offset of y = 8
```

**Overhead: ~24 bytes (300%)**

Even in the most compact case, FlatBuffers tables have:
- 4-byte root offset
- 4-byte vtable offset in table
- 2-byte vtable size
- 2-byte table size
- 2 bytes per field in vtable

### Variable Data: Entity { id: u64, weights: [f32] }

#### ZMEM Wire Format

```
Offset  Size  Content
------  ----  -------
0       8     size = 36 (payload after this field)
8       8     id = 123
16      8     weights.offset = 24
24      8     weights.count = 3
32      4     weights[0] = 1.0
36      4     weights[1] = 2.0
40      4     weights[2] = 3.0
------
Total: 44 bytes (8 header + 36 payload)
```

**Overhead: 8 bytes (size header)**

#### FlatBuffers Wire Format

```
Offset  Size  Content
------  ----  -------
0       4     root offset → table at 16
4       2     vtable: size = 8
6       2     vtable: table size = 12
8       2     vtable: id offset = 4
10      2     vtable: weights offset = 8
12      4     padding (alignment)
16      4     soffset to vtable = -12
20      8     id = 123
28      4     offset to weights vector = 8 → offset 36
32      4     padding
36      4     vector length = 3
40      4     weights[0] = 1.0
44      4     weights[1] = 2.0
48      4     weights[2] = 3.0
------
Total: 52 bytes
```

**Overhead: ~16 bytes (vtable + offsets)**

---

## Overhead Comparison

### By Structure Type

| Structure Type | ZMEM | FlatBuffers | ZMEM Advantage |
|---------------|-------|-------------|-----------------|
| Fixed struct (2 fields) | 8 B | 32 B | 4× smaller |
| Fixed struct (10 fields) | 40 B | 68 B | 1.7× smaller |
| Struct + 1 vector | 44 B | 52 B | 1.2× smaller |
| Struct + 3 vectors | 80 B | 100 B | 1.25× smaller |
| Array of 1000 fixed structs | 8,008 B | 8,012 B | ~Same |
| Deeply nested (5 levels) | Variable | Variable | Similar |

### By Field Count (Fixed Structs)

| Fields | Data Size | ZMEM Total | FlatBuffers Total | FB Overhead |
|--------|-----------|-------------|-------------------|-------------|
| 2 | 8 B | 8 B | 32 B | +300% |
| 5 | 20 B | 20 B | 44 B | +120% |
| 10 | 40 B | 40 B | 68 B | +70% |
| 20 | 80 B | 80 B | 108 B | +35% |
| 50 | 200 B | 200 B | 188 B | -6% (FB wins) |

**Note**: For very large tables (50+ fields), FlatBuffers' vtable sharing can amortize overhead. But most real-world structs have 5-20 fields, where ZMEM excels.

---

## Feature Comparison

### Schema Evolution

| Capability | ZMEM | FlatBuffers |
|-----------|-------|-------------|
| Add new fields | ❌ Requires recompile | ✅ Old readers ignore |
| Remove fields | ❌ Requires recompile | ✅ Deprecated fields return default |
| Reorder fields | ❌ Breaks compatibility | ✅ Vtable handles |
| Change field types | ❌ Breaks compatibility | ❌ Breaks compatibility |
| Rename fields | ✅ Wire-compatible | ✅ Wire-compatible |

**ZMEM position**: Schema evolution is explicitly a non-goal. If you need it, use FlatBuffers, Protobuf, or Cap'n Proto.

### Type System

| Feature | ZMEM | FlatBuffers |
|---------|-------|-------------|
| Primitive types | `bool`, `i8`-`i64`, `u8`-`u64`, `f32`, `f64` | Same |
| 128-bit integers | ✅ `i128`, `u128` | ❌ Not supported |
| Half-precision floats | ✅ `f16`, `bf16` | ❌ Not supported |
| Fixed strings | ✅ `str[N]` | ❌ Use fixed arrays |
| Variable strings | ✅ `string` | ✅ `string` |
| Fixed arrays | ✅ `T[N]` | ✅ Arrays in structs |
| Vectors | ✅ `[T]` | ✅ `[T]` |
| Nested vectors | ✅ `[[T]]` | ✅ `[[T]]` |
| Maps | ✅ `map<K,V>` (sorted) | ❌ Not native (use vector of pairs) |
| Optionals | ✅ `opt<T>` | ✅ Scalar optionals (via null default) |
| Enums | ✅ Any integer base | ✅ Limited base types |
| Unions | ✅ Tagged unions | ✅ Tagged unions |
| Type aliases | ✅ Zero-cost | ❌ Not supported |
| Constants | ✅ `const` declarations | ❌ Not in schema |

### String Handling

| Aspect | ZMEM | FlatBuffers |
|--------|-------|-------------|
| Fixed-size strings | `str[N]` (null-terminated, zero-padded) | Use `[uint8]` or `[int8]` |
| Variable strings | `string` (length-prefixed, no null) | `string` (length-prefixed, null-terminated) |
| String encoding | UTF-8 (validation optional) | UTF-8 required |
| String as map key | ✅ `str[N]` only | ❌ Not supported |
| Zero-copy string access | ✅ Yes | ✅ Yes |

### Vectors

| Aspect | ZMEM | FlatBuffers |
|--------|-------|-------------|
| Vector of primitives | ✅ Contiguous | ✅ Contiguous |
| Vector of structs | ✅ Contiguous (fixed) or offset table (variable) | ✅ Offset-based |
| Nested vectors | ✅ `[[T]]` | ✅ `[[T]]` |
| Vector of strings | ✅ `[string]` | ✅ `[string]` |
| Empty vector | ✅ count=0 | ✅ length=0 |
| Inline fixed array | ✅ `T[N]` in struct | ✅ Arrays in struct |

### Unions

| Aspect | ZMEM | FlatBuffers |
|--------|-------|-------------|
| Tag size | Configurable (`u8`, `u16`, `u32`, `u64`) | Fixed `uint8` |
| Max variants | 2^64 (with `u64` tag) | 255 |
| Unit variants | ✅ No payload | ✅ `NONE` type |
| Inline struct variants | ✅ Yes | ❌ Must reference table |
| Type reference variants | ✅ Yes | ✅ Yes |
| Union of unions | ✅ Yes | ❌ Not supported |

---

## Performance Characteristics

### Serialization

| Aspect | ZMEM | FlatBuffers |
|--------|-------|-------------|
| Fixed struct | `memcpy` (1 call) | Build vtable + copy fields |
| Variable struct | Single pass + offset patching | Builder pattern, multiple allocations |
| Memory allocation | Predictable (size known) | Builder allocates incrementally |
| CPU cache usage | Excellent (sequential writes) | Good (may jump for vtable) |

**ZMEM advantage**: For fixed structs, serialization is literally a single `memcpy`. No builder objects, no allocations, no vtable construction.

### Deserialization

| Aspect | ZMEM | FlatBuffers |
|--------|-------|-------------|
| Fixed struct | `memcpy` or pointer cast | Read through vtable |
| Field access | Direct offset (compile-time known) | Vtable lookup (runtime) |
| Zero-copy view | ✅ Direct pointer | ✅ Accessor objects |
| Random field access | O(1) direct | O(1) vtable lookup |
| Sequential scan | Optimal | Cache misses possible |

**ZMEM advantage**: Field offsets are compile-time constants. `entity.id` compiles to a single memory load at a fixed offset. FlatBuffers requires reading the vtable offset, then the field offset, then the value.

### Memory Access Patterns

```
ZMEM fixed struct access (1 memory load):
  ptr + compile_time_offset → value

FlatBuffers table field access (3 memory loads):
  ptr + vtable_offset → vtable_ptr
  vtable_ptr + field_index → field_offset
  ptr + field_offset → value
```

For hot paths accessing many fields, ZMEM's direct access can be significantly faster due to better cache utilization.

### Benchmarks (Conceptual)

| Operation | ZMEM | FlatBuffers | Ratio |
|-----------|-------|-------------|-------|
| Serialize fixed struct | ~5 ns | ~50 ns | 10× |
| Deserialize fixed struct | ~3 ns | ~20 ns | 7× |
| Access single field | ~1 ns | ~3 ns | 3× |
| Serialize 1000 structs | ~5 μs | ~60 μs | 12× |
| Iterate vector of structs | ~1 μs | ~3 μs | 3× |

*Note: Actual performance varies by CPU, compiler, and data. These ratios are illustrative based on the architectural differences.*

---

## Schema Language Comparison

### ZMEM Schema

```
version 1.0.0

namespace game

const MAX_NAME::u32 = 64
const MAX_INVENTORY::u32 = 100

type PlayerId = u64
type ItemId = u32

enum ItemType : u8 {
  Weapon = 0
  Armor = 1
  Consumable = 2
  Quest = 3
}

union ItemData : u16 {
  Weapon = 0 { damage::u32, range::f32 }
  Armor = 1 { defense::u32, weight::f32 }
  Consumable = 2 { effect_id::u32, duration::f32 }
  Quest = 3
}

struct Item {
  id::ItemId
  name::str[MAX_NAME]
  item_type::ItemType
  data::ItemData
  count::u32 = 1
}

struct Player {
  id::PlayerId
  name::str[MAX_NAME]
  position::f32[3]
  health::f32 = 100.0
  inventory::[Item]
  active_quests::[u32]
  notes::string
}
```

### Equivalent FlatBuffers Schema

```flatbuffers
namespace game;

enum ItemType : ubyte {
  Weapon = 0,
  Armor = 1,
  Consumable = 2,
  Quest = 3
}

table WeaponData {
  damage: uint32;
  range: float;
}

table ArmorData {
  defense: uint32;
  weight: float;
}

table ConsumableData {
  effect_id: uint32;
  duration: float;
}

table QuestData {}

union ItemData {
  WeaponData,
  ArmorData,
  ConsumableData,
  QuestData
}

table Item {
  id: uint32;
  name: string;  // No fixed-size option
  item_type: ItemType;
  data: ItemData;
  count: uint32 = 1;
}

table Player {
  id: uint64;
  name: string;
  position: [float];  // No fixed-size guarantee
  health: float = 100.0;
  inventory: [Item];
  active_quests: [uint32];
  notes: string;
}

root_type Player;
```

### Key Schema Differences

| Feature | ZMEM | FlatBuffers |
|---------|-------|-------------|
| Version declaration | Required | Not supported |
| Constants | `const NAME::type = value` | Not supported |
| Type aliases | `type Alias = Type` | Not supported |
| Fixed strings | `str[64]` | Not supported (use string) |
| Inline union variants | `Variant { field::type }` | Must define separate table |
| Field defaults | All types | Scalars only |
| Array dimensions | `f32[3]` (fixed) vs `[f32]` (variable) | `[float:3]` (fixed) vs `[float]` (variable) |

---

## Use Case Recommendations

### Choose ZMEM When:

1. **All parties deploy together**
   - Microservices in a monorepo
   - Game client and server released simultaneously
   - Internal IPC between processes you control

2. **Maximum performance is critical**
   - High-frequency trading (HFT)
   - Real-time game networking (60+ updates/sec)
   - Audio/video streaming
   - Sensor data pipelines

3. **Fixed structs dominate**
   - Most messages are small, fixed-layout
   - Direct `memcpy` is sufficient
   - Overhead matters at scale

4. **Deterministic serialization needed**
   - Content-addressed storage
   - Cryptographic hashing of messages
   - Binary diffing/patching

5. **Memory-mapped files**
   - Zero-copy access to persisted data
   - Direct pointer arithmetic into files

### Choose FlatBuffers When:

1. **Schema evolution required**
   - Mobile apps with async updates
   - Public APIs with versioning
   - Long-term file storage

2. **Cross-organization communication**
   - Different teams, release cycles
   - Can't guarantee simultaneous deployment

3. **Sparse data**
   - Many optional fields
   - Most fields absent in typical messages
   - Vtable overhead amortized

4. **Need official tooling**
   - `flatc` compiler mature and maintained
   - Bindings for 20+ languages
   - gRPC integration

5. **Tables with 50+ fields**
   - Vtable sharing reduces overhead
   - Schema evolution more valuable

---

## Migration Considerations

### From FlatBuffers to ZMEM

**When it makes sense**:
- You never use schema evolution
- Performance is critical and you've profiled vtable overhead
- You control all communicating parties

**Migration steps**:
1. Convert FlatBuffers tables to ZMEM structs
2. Replace `string` with `str[N]` where max length is known
3. Add explicit field ordering (ZMEM is order-dependent)
4. Remove deprecated fields entirely
5. Update all consumers before producers

**Breaking changes**:
- Wire format incompatible (requires full cutover)
- No gradual migration possible

### From ZMEM to FlatBuffers

**When it makes sense**:
- You need to support schema evolution
- Third parties will consume your data
- You need widespread language support

**Migration steps**:
1. Convert ZMEM structs to FlatBuffers tables
2. Replace `str[N]` with `string` (lose fixed-size guarantee)
3. Replace `const` with external configuration
4. Define union variants as separate tables

---

## Wire Format Deep Dive

### Root Object Handling

**ZMEM**: No root concept. Serialize any type directly.

```
Fixed struct → raw bytes
Variable struct → size header + data
Array → count header + elements
```

**FlatBuffers**: Root table with optional file identifier.

```
┌─────────────┬─────────────────┬─────────────┐
│ root offset │ file identifier │ root table  │
│   4 bytes   │    4 bytes      │   varies    │
└─────────────┴─────────────────┴─────────────┘
```

### Alignment

**ZMEM**: Natural alignment (size-based).
- 1-byte types: 1-byte aligned
- 2-byte types: 2-byte aligned
- 4-byte types: 4-byte aligned
- 8-byte types: 8-byte aligned
- 16-byte types: 16-byte aligned

**FlatBuffers**: Configurable, defaults to natural.
- `force_align` attribute for custom alignment
- Vectors can have custom alignment

### Endianness

Both use **little-endian** for all multi-byte values.

### Padding

**ZMEM**: Minimal padding following C struct rules.

**FlatBuffers**: May insert padding for:
- Field alignment within tables
- Vector alignment
- Vtable alignment

---

## Code Generation Comparison

### ZMEM (Conceptual C++ Output)

```cpp
// Generated from ZMEM schema
namespace game {

struct Item {
    uint32_t id;
    char name[64];
    ItemType item_type;
    ItemData data;
    uint32_t count;
};

static_assert(sizeof(Item) == /* calculated */);
static_assert(std::is_trivially_copyable_v<Item>);

// Serialize: just memcpy
// Deserialize: just memcpy or pointer cast

} // namespace game
```

### FlatBuffers (Actual C++ Output)

```cpp
// Generated by flatc
namespace game {

struct Item FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  enum FlatBuffersVTableOffset FLATBUFFERS_VTABLE_UNDERLYING_TYPE {
    VT_ID = 4,
    VT_NAME = 6,
    VT_ITEM_TYPE = 8,
    VT_DATA_TYPE = 10,
    VT_DATA = 12,
    VT_COUNT = 14
  };
  uint32_t id() const {
    return GetField<uint32_t>(VT_ID, 0);
  }
  const flatbuffers::String *name() const {
    return GetPointer<const flatbuffers::String *>(VT_NAME);
  }
  // ... more accessors
};

struct ItemBuilder {
  flatbuffers::FlatBufferBuilder &fbb_;
  flatbuffers::uoffset_t start_;
  void add_id(uint32_t id) { fbb_.AddElement<uint32_t>(Item::VT_ID, id, 0); }
  // ... more builder methods
};

} // namespace game
```

**Code size**: ZMEM generates minimal code. FlatBuffers generates extensive accessor and builder infrastructure.

---

## Summary Table

| Criterion | ZMEM | FlatBuffers | Winner |
|-----------|-------|-------------|--------|
| Fixed struct overhead | 0 bytes | 20-40 bytes | **ZMEM** |
| Variable struct overhead | 8 bytes | 12-20 bytes | **ZMEM** |
| Schema evolution | ❌ | ✅ | **FlatBuffers** |
| Serialization speed | `memcpy` | Builder pattern | **ZMEM** |
| Field access speed | Direct offset | Vtable lookup | **ZMEM** |
| Language support | C++, Rust, C | 20+ languages | **FlatBuffers** |
| Tooling maturity | New | Battle-tested | **FlatBuffers** |
| Zero-copy read | ✅ | ✅ | Tie |
| Deterministic output | ✅ Guaranteed | ⚠️ Builder-dependent | **ZMEM** |
| Fixed-size strings | ✅ `str[N]` | ❌ | **ZMEM** |
| Maps | ✅ Native | ❌ Manual | **ZMEM** |
| 128-bit integers | ✅ | ❌ | **ZMEM** |
| Half-precision floats | ✅ `f16`, `bf16` | ❌ | **ZMEM** |
| Sparse data | Wastes space | Efficient | **FlatBuffers** |
| Documentation | Comprehensive spec | Extensive docs + tutorials | **FlatBuffers** |

---

## Conclusion

**ZMEM** and **FlatBuffers** serve different needs:

- **ZMEM** is ideal for **controlled environments** where all parties are deployed together and **raw performance** is paramount. Its zero-overhead design for fixed structs and deterministic wire format make it perfect for HFT, game networking, and real-time systems.

- **FlatBuffers** is ideal for **evolving systems** where clients and servers update independently. Its vtable-based design enables schema evolution at the cost of some overhead, making it better for public APIs, mobile apps, and long-term storage.

If you control both ends and can tolerate synchronized deployments, **ZMEM will be faster**. If you need flexibility and broad language support, **FlatBuffers is safer**.

Choose based on your constraints, not ideology.
