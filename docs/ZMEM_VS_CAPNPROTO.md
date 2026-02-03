# ZMEM vs Cap'n Proto: A Technical Comparison

This document provides a detailed technical comparison between **ZMEM** and **Cap'n Proto**, two binary serialization formats designed for high-performance zero-copy access. While both eliminate parsing overhead, they make fundamentally different architectural decisions.

## Executive Summary

| Aspect | ZMEM | Cap'n Proto |
|--------|-------|-------------|
| **Primary Goal** | Maximum performance, minimal overhead | Zero-copy with schema evolution + RPC |
| **Schema Evolution** | ❌ Not supported | ✅ Forward-compatible |
| **Fixed Struct Overhead** | **0-7 bytes** (8-byte padding) | 8-24 bytes (segment + pointers) |
| **Alignment** | 8-byte struct sizes, 8-byte variable data | 64-bit words (8-byte minimum) |
| **Zero-Copy Read** | ✅ Yes | ✅ Yes |
| **Direct memcpy** | ✅ Yes (fixed structs) | ❌ No (pointer-based) |
| **Built-in RPC** | ❌ No | ✅ Yes (with pipelining) |
| **Best For** | IPC, HFT, game networking | RPC, distributed systems, capability security |

---

## Design Philosophy

### ZMEM: "Raw Speed, No Compromises"

ZMEM assumes **all communicating parties use identical schemas** at compile time. This enables:

- **Minimal overhead** for fixed structs (direct `memcpy` + 8-byte padding)
- **8-byte struct sizes** (all structs padded to multiples of 8)
- **8-byte alignment** for variable section data (safe zero-copy access)
- **Fixed offsets** known at compile time
- **No pointers** in fixed struct wire format
- **Compile-time validation** via type signatures

The trade-off: **No schema evolution, no RPC system**. ZMEM is purely a serialization format.

### Cap'n Proto: "Zero-Copy with Evolution and RPC"

Cap'n Proto prioritizes **zero-copy access with forward compatibility**:

- **Pointer-based layout** enables schema evolution
- **64-bit word alignment** simplifies parsing logic
- **Segment model** supports arena allocation
- **Built-in RPC** with promise pipelining
- **Capability-based security** for distributed systems

The trade-off: **Structural overhead** from pointers, word alignment, and segments. Every struct has a pointer section, even if empty.

---

## Architecture Comparison

### Memory Model

**ZMEM**: Direct struct layout
```
┌──────────────────────────────────────────┐
│           Struct bytes (contiguous)      │
│   Fields at compile-time-known offsets   │
└──────────────────────────────────────────┘
```

**Cap'n Proto**: Segmented message with pointers
```
┌─────────────────────────────────────────────────────────────┐
│ Segment Table │ Segment 0                                   │
│   (4+ bytes)  │ ┌─────────────────────────────────────────┐ │
│               │ │ Struct: [data section][pointer section] │ │
│               │ │         └─────────┬──────────────────┘  │ │
│               │ │                   ↓                     │ │
│               │ │         [pointed-to data...]            │ │
│               │ └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Pointer Encoding

Cap'n Proto uses 64-bit pointers with type information:

```
Struct pointer (64 bits):
┌────┬──────────────┬────────────────┬────────────────┐
│ 00 │ offset (30b) │ data size (16b)│ ptr count (16b)│
└────┴──────────────┴────────────────┴────────────────┘

List pointer (64 bits):
┌────┬──────────────┬────────────────┬─────────────────┐
│ 01 │ offset (30b) │ elem size (3b) │ elem count (29b)│
└────┴──────────────┴────────────────┴─────────────────┘
```

**ZMEM has no pointers** in fixed structs. Variable structs use simple offset+count pairs (16 bytes) without embedded type information.

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

**Overhead: 0 bytes** (Point is already 8-byte aligned)

#### Cap'n Proto Wire Format (24 bytes minimum)

```
Segment table (single segment):
┌────────────────────────────────────┐
│ segment count - 1 = 0   (4 bytes)  │
│ segment 0 size in words (4 bytes)  │
└────────────────────────────────────┘

Root pointer (in segment):
┌────────────────────────────────────────────────────────────────┐
│ Struct pointer: offset=0, data_size=1 word, ptr_count=0        │
│                           (8 bytes)                            │
└────────────────────────────────────────────────────────────────┘

Struct data (padded to 64-bit word):
┌─────────────────┬─────────────────┐
│        x        │        y        │
│     4 bytes     │     4 bytes     │
└─────────────────┴─────────────────┘

Total layout:
Offset  Size  Content
------  ----  -------
0       4     segment count - 1 = 0
4       4     segment 0 size = 2 words
8       8     root struct pointer
16      4     x = 1.0
20      4     y = 2.0
------
Total: 24 bytes
```

**Overhead: 16 bytes (200%)**

### Comparison: 8 Bytes vs 24 Bytes

| Component | ZMEM | Cap'n Proto |
|-----------|-------|-------------|
| Segment table | 0 | 8 bytes |
| Root pointer | 0 | 8 bytes |
| Data | 8 bytes | 8 bytes |
| **Total** | **8 bytes** | **24 bytes** |

### Variable Data: Entity { id: u64, name: string, weights: [f32] }

#### ZMEM Wire Format

```
Offset  Size  Content
------  ----  -------
0       8     size = 64 (content size, padded to 8-byte boundary)
8       8     id = 123
16      8     name.offset = 40 (relative to byte 8, 8-byte aligned)
24      8     name.length = 5
32      8     weights.offset = 48 (relative to byte 8, 8-byte aligned)
40      8     weights.count = 3
48      5     name data: "Alice"
53      3     alignment padding (to reach 8-byte boundary for weights)
56      4     weights[0] = 1.0
60      4     weights[1] = 2.0
64      4     weights[2] = 3.0
68      4     end padding (to reach 64 bytes content)
------
Total: 72 bytes (8 header + 64 content)
```

Note: All variable section data is 8-byte aligned for safe zero-copy access.

#### Cap'n Proto Wire Format

```
Offset  Size  Content
------  ----  -------
0       4     segment count - 1 = 0
4       4     segment 0 size = 9 words (72 bytes)
8       8     root struct pointer (offset=0, data=1w, ptrs=2)
16      8     id = 123 (1 word data section)
24      8     pointer to name (text pointer)
32      8     pointer to weights (list pointer)
40      8     name: length=5, "Alice\0" + padding to word
48      8     weights list: 3 × f32 + padding
------
Total: ~56-72 bytes (depends on exact encoding)
```

For this example, Cap'n Proto is more compact due to:
- Text doesn't require fixed-size padding
- List encoding is efficient

But ZMEM can also use variable-length `string`, making them comparable.

---

## Overhead Analysis

### Minimum Message Size

| Format | Empty Struct | Struct with 1 u64 |
|--------|--------------|-------------------|
| ZMEM (fixed) | 8 bytes (8-byte padding) | 8 bytes |
| ZMEM (variable) | 8 bytes (size header) | 16 bytes |
| Cap'n Proto | 16 bytes (seg table + root ptr) | 24 bytes |

### By Structure Type

| Structure | ZMEM | Cap'n Proto | ZMEM Advantage |
|-----------|-------|-------------|-----------------|
| Point { x, y: f32 } | 8 B | 24 B | 3× smaller |
| Vec3 { x, y, z: f32 } | 16 B (12 + 4 padding) | 24 B | 1.5× smaller |
| Particle (10 fields) | 40 B | 56 B | 1.4× smaller |
| Entity + 1 vector | 48 B | 48 B | ~Same |
| Deep nesting (5 levels) | Variable | Variable | ZMEM slightly smaller |

### 64-bit Word Alignment Cost

Cap'n Proto aligns all structs to 64-bit boundaries:

| Struct | Actual Data | ZMEM Size | Cap'n Proto Size | Notes |
|--------|-------------|------------|------------------|-------|
| { a: u8 } | 1 byte | 8 bytes | 8 bytes | Same (both pad to 8) |
| { a: u8, b: u16 } | 3 bytes | 8 bytes | 8 bytes | Same |
| { a: u8, b: u8, c: u8 } | 3 bytes | 8 bytes | 8 bytes | Same |
| { a: u32 } | 4 bytes | 8 bytes | 8 bytes | Same |
| { a: u64 } | 8 bytes | 8 bytes | 8 bytes | Same |

**ZMEM uses 8-byte struct sizes** - all structs are padded to multiples of 8 bytes for safe zero-copy access. Within structs, fields use natural alignment. Variable section data also uses 8-byte alignment. This is the same as Cap'n Proto's 64-bit word alignment for struct sizes.

---

## Feature Comparison

### Schema Evolution

| Capability | ZMEM | Cap'n Proto |
|------------|-------|-------------|
| Add new fields | ❌ Requires recompile | ✅ At end of struct |
| Remove fields | ❌ Requires recompile | ⚠️ Must keep slot (reads as default) |
| Reorder fields | ❌ Breaks compatibility | ❌ Slot positions fixed |
| Change field types | ❌ Breaks compatibility | ❌ Breaks compatibility |
| Rename fields | ✅ Wire-compatible | ✅ Wire-compatible |
| Grow lists | N/A | ✅ Supported |
| Default values | ✅ Codegen only | ✅ Wire-level defaults |

**Cap'n Proto evolution model**: New fields are added at the end with higher slot indices. Old readers ignore unknown slots. This requires maintaining slot assignments forever.

### Type System

| Feature | ZMEM | Cap'n Proto |
|---------|-------|-------------|
| Integers | `i8`-`i64`, `u8`-`u64` | `Int8`-`Int64`, `UInt8`-`UInt64` |
| 128-bit integers | ✅ `i128`, `u128` | ❌ Not supported |
| Floats | `f16`, `f32`, `f64`, `bf16` | `Float32`, `Float64` |
| Half-precision | ✅ `f16`, `bf16` | ❌ Not supported |
| Booleans | `bool` (1 byte) | `Bool` (1 bit, packed) |
| Fixed strings | ✅ `str[N]` | ❌ Not supported |
| Variable strings | ✅ `string` | ✅ `Text` |
| Binary data | `[u8]` | `Data` |
| Fixed arrays | ✅ `T[N]` | ❌ Use List with conventions |
| Lists/Vectors | ✅ `[T]` | ✅ `List(T)` |
| Nested lists | ✅ `[[T]]` | ✅ `List(List(T))` |
| Maps | ✅ `map<K,V>` (native, sorted) | ❌ Not native |
| Optionals | ✅ `opt<T>` | ⚠️ Unions or pointer nullability |
| Enums | ✅ Any integer base | ✅ `UInt16` based |
| Unions | ✅ Tagged unions | ✅ Anonymous unions in structs |
| Type aliases | ✅ Zero-cost | ❌ Use `using` (codegen only) |
| Constants | ✅ Schema-level | ✅ Schema-level |
| Generics | ❌ Not supported | ✅ Supported |
| Annotations | ❌ Not supported | ✅ Supported |

### Boolean Packing

**Cap'n Proto** packs booleans as individual bits:
- 8 bools in 1 byte
- Accessed via bit manipulation

**ZMEM** uses 1 byte per boolean:
- Simpler access (direct byte read)
- 8× larger for bool-heavy structs

For structs with many booleans, Cap'n Proto is more compact. For typical structs with few booleans, the difference is negligible.

### Strings and Text

| Aspect | ZMEM | Cap'n Proto |
|--------|-------|-------------|
| Fixed-size strings | ✅ `str[N]` (inline) | ❌ Not supported |
| Variable strings | ✅ `string` | ✅ `Text` |
| Encoding | UTF-8 (validation optional) | UTF-8 required |
| Null termination | `str[N]`: yes, `string`: no | Always NUL-terminated |
| String as key | ✅ `str[N]` in maps | ❌ No native maps |

Cap'n Proto's `Text` is always NUL-terminated for C compatibility, adding 1 byte per string.

### Lists (Vectors)

| Aspect | ZMEM | Cap'n Proto |
|--------|-------|-------------|
| Primitive lists | ✅ Contiguous | ✅ Contiguous |
| Struct lists | ✅ Contiguous (fixed) or offset table (variable) | ✅ Inline (fixed size per element) |
| Nested lists | ✅ `[[T]]` | ✅ `List(List(T))` |
| List of pointers | N/A | ✅ Used for variable-size elements |
| Empty lists | ✅ count=0 | ✅ Null pointer or length=0 |
| Inline fixed arrays | ✅ `T[N]` | ❌ Not supported |

**Cap'n Proto list encoding** uses a 3-bit element size field:
- 0: void (0 bits)
- 1: bit (1 bit)
- 2: byte (8 bits)
- 3: two bytes
- 4: four bytes
- 5: eight bytes
- 6: pointer (64 bits)
- 7: composite (inline structs)

This is elegant but adds complexity.

### Unions

| Aspect | ZMEM | Cap'n Proto |
|--------|-------|-------------|
| Syntax | `union Name : tag_type { ... }` | Anonymous within struct |
| Tag size | Configurable (u8, u16, u32, u64) | Fixed 16-bit |
| Max variants | 2^64 (with u64) | 65535 |
| Named unions | ✅ First-class type | ❌ Must embed in struct |
| Multiple unions | ✅ Multiple union fields | ⚠️ One anonymous union per struct |
| Union of unions | ✅ Supported | ❌ Not directly |

**ZMEM example**:
```
union Result : u32 {
  Ok = 0 { value::u64 }
  Err = 1 { code::i32, message::str[256] }
}

struct Response {
  id::u64
  result::Result    # Named union as field
  status::Status    # Another enum/union field
}
```

**Cap'n Proto example**:
```capnp
struct Response {
  id @0 :UInt64;
  union {
    ok @1 :UInt64;
    err :group {
      code @2 :Int32;
      message @3 :Text;
    }
  }
}
```

Cap'n Proto's anonymous unions are less flexible but integrate tightly with struct evolution.

---

## RPC and Distributed Systems

### Built-in RPC

| Feature | ZMEM | Cap'n Proto |
|---------|-------|-------------|
| RPC system | ❌ Not included | ✅ Full RPC framework |
| Promise pipelining | ❌ N/A | ✅ Reduces round trips |
| Capabilities | ❌ N/A | ✅ Object-capability security |
| Streaming | ❌ N/A | ✅ Supported |
| Cancellation | ❌ N/A | ✅ Supported |

**Cap'n Proto RPC** is a major feature:

```capnp
interface Calculator {
  evaluate @0 (expression :Expression) -> (value :Value);
  defFunction @1 (name :Text, body :Expression) -> (func :Function);
}
```

Promise pipelining allows chaining calls without waiting for responses:

```cpp
// Without pipelining: 3 round trips
auto promise1 = calculator.evaluateRequest().send();
auto result1 = promise1.wait();
auto promise2 = calculator.evaluateRequest().send();
auto result2 = promise2.wait();
// Use result2

// With pipelining: 1 round trip
auto promise = calculator.evaluateRequest().send();
auto pipelined = promise.getValue().evaluateRequest().send();
auto result = pipelined.wait();
```

**ZMEM is serialization-only**. Use it with your own transport (TCP, UDP, shared memory, etc.) or integrate with external RPC frameworks.

### Capability-Based Security

Cap'n Proto supports **object capabilities**:

```capnp
interface BlobStore {
  write @0 (data :Data) -> (blob :Blob);
}

interface Blob {
  read @0 () -> (data :Data);
  # Possessing a Blob capability grants read access
}
```

This enables fine-grained access control in distributed systems. ZMEM has no equivalent—security is external.

---

## Performance Characteristics

### Serialization

| Aspect | ZMEM | Cap'n Proto |
|--------|-------|-------------|
| Fixed struct | `memcpy` (1 call) | Arena allocation + set fields |
| Variable struct | Single pass + offset patching | Arena allocation + pointer setup |
| Memory model | Direct buffer write | Arena (MessageBuilder) |
| Zero-copy build | ⚠️ Requires pre-sized buffer | ✅ Arena supports |

**ZMEM** can serialize with a single `memcpy` for fixed structs. **Cap'n Proto** requires an arena allocator (MessageBuilder) and explicit field setting.

### Deserialization

| Aspect | ZMEM | Cap'n Proto |
|--------|-------|-------------|
| Fixed struct | `memcpy` or pointer cast | Wrap buffer + accessor calls |
| Field access | Direct offset (compile-time) | Pointer traversal (runtime) |
| Zero-copy view | ✅ Direct pointer | ✅ Reader objects |
| Bounds checking | Optional | Built into accessors |

**Memory access pattern comparison**:

```
ZMEM field access (1 load for fixed struct):
  ptr + compile_time_offset → value

Cap'n Proto field access (1-2 loads + compute):
  reader.getField() →
    compute offset from pointer →
    load value at offset
```

For fixed data, ZMEM is faster. For deeply nested data with many pointers, the difference narrows.

### Random Access vs Sequential

**ZMEM fixed structs**: Optimal for sequential and random access (direct offsets).

**Cap'n Proto**: Pointer chasing for nested access, but pointers are relative (cache-friendly within a segment).

### Benchmarks (Conceptual)

| Operation | ZMEM | Cap'n Proto | Ratio |
|-----------|-------|-------------|-------|
| Serialize Point{f32,f32} | ~5 ns | ~40 ns | 8× |
| Deserialize Point | ~3 ns | ~15 ns | 5× |
| Access nested field (3 deep) | ~2 ns | ~8 ns | 4× |
| Serialize 1000 structs | ~5 μs | ~50 μs | 10× |
| RPC call (local) | N/A | ~100 μs | - |

*Note: Cap'n Proto RPC includes significant functionality beyond serialization. These ratios are illustrative.*

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
}

union ItemData : u16 {
  Weapon = 0 { damage::u32, range::f32 }
  Armor = 1 { defense::u32, weight::f32 }
  Consumable = 2 { heal::f32, duration::f32 }
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
  active_effects::[[f32]]    # nested vectors
  notes::string
}
```

### Equivalent Cap'n Proto Schema

```capnp
@0xabcdef1234567890;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("game");

const maxName :UInt32 = 64;
const maxInventory :UInt32 = 100;

# No type aliases in Cap'n Proto

enum ItemType {
  weapon @0;
  armor @1;
  consumable @2;
}

struct Item {
  id @0 :UInt32;
  name @1 :Text;  # No fixed-size option
  itemType @2 :ItemType;

  union {
    weapon :group {
      damage @3 :UInt32;
      range @4 :Float32;
    }
    armor :group {
      defense @5 :UInt32;
      weight @6 :Float32;
    }
    consumable :group {
      heal @7 :Float32;
      duration @8 :Float32;
    }
  }

  count @9 :UInt32 = 1;
}

struct Player {
  id @0 :UInt64;
  name @1 :Text;
  position @2 :List(Float32);  # No fixed-size guarantee
  health @3 :Float32 = 100.0;
  inventory @4 :List(Item);
  activeEffects @5 :List(List(Float32));
  notes @6 :Text;
}
```

### Key Differences

| Feature | ZMEM | Cap'n Proto |
|---------|-------|-------------|
| File header | `version 1.0.0` | `@0x...;` (unique file ID) |
| Namespaces | `namespace game` | Annotations |
| Constants | `const NAME::type = value` | `const name :Type = value;` |
| Type aliases | `type Alias = Type` | Not supported |
| Fixed strings | `str[64]` | Not supported |
| Fixed arrays | `f32[3]` | Not supported |
| Named unions | `union Name { ... }` | Anonymous in struct |
| Field numbering | Implicit (order matters) | Explicit `@N` (order doesn't matter) |
| Evolution slots | N/A | Explicit via `@N` |
| Generics | Not supported | Supported |

---

## Determinism and Canonicalization

### ZMEM: Deterministic by Default

ZMEM guarantees deterministic serialization:
- Field order is fixed (schema order)
- Padding is zero-filled
- Optional absent values are zero-filled
- Map entries are sorted by key
- No builder-dependent variations

```cpp
// Same data always produces identical bytes
Entity e1{...}, e2{...};  // identical values
auto buf1 = serialize(e1);
auto buf2 = serialize(e2);
assert(buf1 == buf2);  // Guaranteed
```

### Cap'n Proto: Canonical Encoding Optional

Cap'n Proto has a canonical encoding mode, but it's not the default:
- Standard encoding allows variations (e.g., pointer ordering)
- Canonical mode enforces determinism but adds overhead
- Arena reuse can leave stale data

```cpp
// May produce different bytes for same logical data
capnp::MallocMessageBuilder builder1, builder2;
// ... set same data ...
// builder1 and builder2 may differ in internal layout
```

For content-addressed storage or cryptographic hashing, ZMEM's default determinism is advantageous.

---

## Memory Management

### ZMEM: Direct Buffers

```cpp
// Serialization: compute size, allocate, write
size_t size = zmem::serialized_size(entity);
std::vector<uint8_t> buffer(size);
zmem::serialize(entity, buffer.data());

// Deserialization: view or copy
auto view = zmem::view<Entity>(buffer.data(), buffer.size());
// or
Entity entity;
zmem::deserialize(buffer.data(), buffer.size(), entity);
```

Simple, predictable memory usage.

### Cap'n Proto: Arena Allocation

```cpp
// Serialization: arena allocator
capnp::MallocMessageBuilder message;
auto root = message.initRoot<Entity>();
root.setId(123);
root.setName("Alice");
// Arena grows as needed

auto segments = message.getSegmentsForOutput();
// Write segments to wire

// Deserialization: wrap buffer
capnp::FlatArrayMessageReader reader(buffer);
auto entity = reader.getRoot<Entity>();
// Reader provides zero-copy view
```

Arena allocation is efficient for building complex messages but adds complexity.

### Orphan Pattern

Cap'n Proto supports **orphans** for moving data between messages:

```cpp
auto orphan = message1.getOrphanage().newOrphan<Item>();
orphan.get().setId(42);
// ... later ...
entity.adoptItem(std::move(orphan));
```

ZMEM has no equivalent—data is always contiguous.

---

## Use Case Recommendations

### Choose ZMEM When:

1. **Maximum serialization speed**
   - HFT, real-time systems
   - Game networking
   - High-frequency telemetry

2. **Fixed struct dominance**
   - Most messages are small, fixed-layout
   - Direct `memcpy` is sufficient

3. **Deterministic serialization required**
   - Content-addressed storage
   - Cryptographic hashing
   - Binary diffing

4. **Memory-mapped files**
   - Zero-copy access to persisted data
   - Direct pointer arithmetic

5. **Controlled deployment**
   - All parties compiled together
   - No schema evolution needed

6. **Minimal dependencies**
   - No runtime library required (just headers)
   - Simple integration

### Choose Cap'n Proto When:

1. **RPC with pipelining**
   - Distributed systems with latency concerns
   - Complex call chains

2. **Schema evolution required**
   - Long-lived data formats
   - Services with independent deployment

3. **Capability-based security**
   - Fine-grained access control
   - Object capability model

4. **Complex message building**
   - Arena allocation beneficial
   - Messages built incrementally

5. **Boolean-heavy structs**
   - Bit-packing saves space
   - Many optional boolean fields

6. **Generic types needed**
   - Schema-level generics
   - Reusable parameterized types

---

## Migration Considerations

### From Cap'n Proto to ZMEM

**When it makes sense**:
- You're not using Cap'n Proto RPC
- Schema evolution is unused or problematic
- Performance is critical and you've profiled overhead
- You control all endpoints

**Migration steps**:
1. Identify all schema files
2. Convert structs (flatten groups, explicit unions)
3. Replace `Text` with `str[N]` or `string`
4. Remove evolution slots (reorder fields logically)
5. Replace `@0` annotations with field ordering
6. Update all clients/servers simultaneously

**Breaking changes**:
- Wire format completely incompatible
- Must be a full cutover

### From ZMEM to Cap'n Proto

**When it makes sense**:
- You need RPC with pipelining
- Schema evolution becomes necessary
- Capability security is required

**Migration steps**:
1. Add `@N` field numbers to preserve order
2. Convert `str[N]` to `Text`
3. Convert fixed arrays to `List` with conventions
4. Replace `type` aliases (codegen adjustment)
5. Convert named unions to anonymous (or wrapper structs)

---

## Interoperability

### Language Support

| Language | ZMEM | Cap'n Proto |
|----------|-------|-------------|
| C++ | ✅ Primary target | ✅ Primary target |
| Rust | ✅ Planned | ✅ Mature |
| C | ✅ Via C++ or pure C | ✅ Via C++ wrapper |
| Python | Planned | ✅ (pycapnp) |
| JavaScript | Planned | ✅ (capnp-js) |
| Java | Planned | ⚠️ Limited |
| Go | Planned | ✅ Mature |
| C# | Planned | ⚠️ Limited |

Cap'n Proto has broader language support due to its longer history.

### Transport Agnostic

Both formats are transport-agnostic:

**ZMEM**: Write bytes to any channel (TCP, UDP, shared memory, files)

**Cap'n Proto**: Same, plus built-in RPC over various transports (TCP, Unix sockets, etc.)

---

## Wire Format Deep Dive

### Segment Model

**ZMEM**: No segments. Single contiguous buffer.

**Cap'n Proto**: Messages consist of one or more segments:

```
Message:
┌─────────────────────────────────────────────────────────────┐
│ Segment count - 1 (4 bytes)                                 │
│ Segment 0 size in words (4 bytes)                           │
│ Segment 1 size in words (4 bytes, if multiple segments)     │
│ ...                                                         │
│ Padding to 8-byte boundary                                  │
├─────────────────────────────────────────────────────────────┤
│ Segment 0 data                                              │
├─────────────────────────────────────────────────────────────┤
│ Segment 1 data (if present)                                 │
├─────────────────────────────────────────────────────────────┤
│ ...                                                         │
└─────────────────────────────────────────────────────────────┘
```

Multi-segment messages enable:
- Arena allocation with fixed-size segments
- Large messages without single huge allocation
- Inter-segment pointers (far pointers)

But add complexity and overhead.

### Pointer Types

Cap'n Proto has four pointer types:

1. **Struct pointer**: Points to struct data
2. **List pointer**: Points to list elements
3. **Far pointer**: Points to another segment
4. **Capability pointer**: Points to RPC capability

Each is 64 bits with a 2-bit tag.

**ZMEM** uses simple offset+count pairs (16 bytes, no tagging).

### Null and Default Handling

**ZMEM**:
- `opt<T>`: Explicit presence byte + value
- No null pointers (vectors have count, not null)
- Defaults are codegen-only

**Cap'n Proto**:
- Null pointers (all zeros) represent absent/default
- Default values encoded in schema
- Missing fields return schema default

---

## Summary Comparison

| Criterion | ZMEM | Cap'n Proto | Winner |
|-----------|-------|-------------|--------|
| Fixed struct overhead | 0-7 bytes (padding) | 16-24 bytes | **ZMEM** |
| Variable struct overhead | 8 bytes | 16+ bytes | **ZMEM** |
| Schema evolution | ❌ | ✅ | **Cap'n Proto** |
| RPC system | ❌ | ✅ Full-featured | **Cap'n Proto** |
| Serialization speed | `memcpy` | Arena + setup | **ZMEM** |
| Alignment efficiency | 8-byte struct sizes + variable data | 64-bit words | Similar |
| Boolean packing | 1 byte each | 1 bit each | **Cap'n Proto** |
| Fixed-size strings | ✅ `str[N]` | ❌ | **ZMEM** |
| Maps | ✅ Native | ❌ Manual | **ZMEM** |
| 128-bit integers | ✅ | ❌ | **ZMEM** |
| Half-precision floats | ✅ | ❌ | **ZMEM** |
| Generics | ❌ | ✅ | **Cap'n Proto** |
| Capability security | ❌ | ✅ | **Cap'n Proto** |
| Deterministic output | ✅ Default | ⚠️ Optional | **ZMEM** |
| Language support | Limited | Broad | **Cap'n Proto** |
| Tooling maturity | New | Established | **Cap'n Proto** |

---

## Conclusion

**ZMEM** and **Cap'n Proto** target different use cases:

- **ZMEM** is a **pure serialization format** optimized for maximum speed in controlled environments. Its zero-overhead design for fixed structs, natural alignment, and deterministic output make it ideal for HFT, game networking, and real-time systems where all parties deploy together.

- **Cap'n Proto** is a **serialization + RPC framework** designed for distributed systems. Its pointer-based layout enables schema evolution and sophisticated features like promise pipelining and capability security, at the cost of structural overhead.

**Decision guide**:
- Need RPC with pipelining or capability security? → **Cap'n Proto**
- Need schema evolution for independent deployments? → **Cap'n Proto**
- Need maximum serialization speed for controlled systems? → **ZMEM**
- Need deterministic serialization for content addressing? → **ZMEM**
- Building a distributed system with complex interactions? → **Cap'n Proto**
- Building high-frequency IPC or game networking? → **ZMEM**

Choose based on your system architecture, not benchmarks alone.
