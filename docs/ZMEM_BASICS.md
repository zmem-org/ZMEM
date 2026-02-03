# ZMEM Format Basics

A beginner-friendly guide to understanding the ZMEM binary serialization format.

## What is ZMEM?

ZMEM (Zero-copy Memory Format) is a high-performance binary serialization format designed for:

- **Minimal overhead** for fixed structs (direct memory representation, padded to 8-byte boundaries)
- **Zero-copy reads** where possible (data can be accessed in-place)
- **Predictable performance** with optional schema (C++ uses reflection, no schema file required)

## Core Concept: Fixed vs Variable Types

ZMEM categorizes all types into two categories:

### Fixed Types

Fixed types are **trivially copyable** - they can be serialized with a direct `memcpy`. Wire sizes are padded to multiples of 8 bytes (minimum); higher alignment is honored when required.

```cpp
// Primitives - written directly to wire (within structs)
int32_t x = 42;           // 4 bytes
float y = 3.14f;          // 4 bytes
bool flag = true;         // 1 byte

// Fixed struct - padded to multiple of 8 bytes
struct Point {
    float x, y;
};
Point p{1.0f, 2.0f};      // 8 bytes on wire (sizeof=8, already multiple of 8)

struct Triangle {
    float vertices[9];    // 36 bytes data
};
Triangle t{...};          // 40 bytes on wire (padded to multiple of 8)
```

**Fixed types include:**
- All primitives (integers, floats, bools)
- Enums
- `std::array<T, N>` where T is fixed
- C arrays (`T[N]`) where T is fixed
- Structs containing only fixed types

### Variable Types

Variable types contain **variable-length data** like vectors, strings, or maps. They need additional structure to track where the variable data lives.

```cpp
// Variable types - have variable-length members
struct Entity {
    uint64_t id;              // fixed member
    std::vector<float> data;  // variable-length!
    std::string name;         // variable-length!
};
```

## Wire Format

### Fixed Types: Direct Representation

Fixed types are written exactly as they appear in memory (with little-endian byte order):

```
uint32_t value = 0x12345678

Wire bytes: [78] [56] [34] [12]
            └─ least significant byte first (little-endian)
```

Fixed structs are written field-by-field, preserving padding:

```
struct Point { float x, y; }
Point p{1.0f, 2.0f}

Wire bytes: [00 00 80 3F] [00 00 00 40]
            └─── x=1.0 ───┘└─── y=2.0 ───┘
```

### Variable Structs: Header + Inline + Variable

Variable structs use a three-part layout:

```
┌────────────────────────────────────────────────────────────┐
│ [Size Header: 8 bytes] [Inline Section] [Variable Section] │
└────────────────────────────────────────────────────────────┘
```

**Size Header (8 bytes):** Total byte count of inline + variable sections (including alignment padding)

**Inline Section:** Fixed-size portion containing:
- Fixed members written directly
- 16-byte references for vectors/strings/maps (offset + count/length)

**Variable Section:** Actual data for vectors, strings, and maps, with **8-byte alignment padding** before each field's data

**Alignment:** All variable section data is 8-byte aligned for safe zero-copy access. The total content size is also padded to a multiple of 8 bytes.

#### Example: Variable Struct

```cpp
struct Entity {
    uint64_t id;
    std::vector<int32_t> values;
};

Entity e{42, {10, 20, 30}};
```

Wire layout (with 8-byte alignment):

```
Offset  Content
──────  ───────────────────────────────────
0       [Size: 40 bytes]                   ← 8-byte header (content is padded to 40)
8       [id: 42]                           ← inline: uint64_t (8 bytes)
16      [offset: 24, count: 3]             ← inline: vector reference (16 bytes)
32      [padding: 0]                       ← 8-byte alignment padding (already aligned)
32      [10] [20] [30]                     ← variable: vector data (12 bytes)
44      [padding: 4 bytes]                 ← end padding to reach 40 bytes content
──────  ───────────────────────────────────
Total: 48 bytes (8 header + 40 content)
```

The vector reference contains:
- **offset**: Byte position of data relative to the inline section start (byte 8 for typical types; byte 16 if the inline section is align-16)
- **count**: Number of elements (here: 3)

### Strings

Strings work like vectors but store byte length instead of element count:

```cpp
struct Message {
    uint64_t timestamp;
    std::string text;
};

Message m{1000, "Hi"};
```

Wire layout (with 8-byte alignment):

```
Offset  Content
──────  ───────────────────────────────────
0       [Size: 32 bytes]                   ← 8-byte header (content padded to 32)
8       [timestamp: 1000]                  ← inline: uint64_t (8 bytes)
16      [offset: 24, length: 2]            ← inline: string reference (16 bytes)
32      [H] [i]                            ← variable: string data at 8-byte aligned offset
34      [padding: 6 bytes]                 ← end padding to reach 32 bytes content
──────  ───────────────────────────────────
Total: 40 bytes (8 header + 32 content)
```

String data is 8-byte aligned for consistency with vector data, ensuring all variable section pointers are safely dereferenceable.

### Vectors and Arrays

**Fixed element vectors** (contiguous data):
```
[count: 8 bytes] [element₀] [element₁] ... [elementₙ]
```

**Variable element vectors** (need offset table for random access):
```
[count: 8 bytes] [offset₀] [offset₁] ... [offsetₙ] [sentinel] [element₀] [element₁] ...
```

The offset table enables O(1) access to any element without scanning.

## Quick Reference

| Type | Wire Format | Overhead |
|------|-------------|----------|
| Primitives (in struct) | Direct bytes (little-endian) | 0 bytes |
| Fixed struct (top-level) | Direct bytes + padding to 8-byte boundary | 0-7 bytes |
| Fixed array (in struct) | Contiguous elements, no padding | 0 bytes |
| Variable struct | 8-byte header + inline + variable | 8 bytes + refs |
| Vector (fixed T) | 8-byte count + contiguous elements + end padding | 8 bytes + 0-7 end |
| Vector (variable T) | 8-byte count + offset table + data | 8 + (n+1)×8 bytes |
| String | 8-byte length + data | 8 bytes |

## C++ API Usage

### Basic Serialization

```cpp
#include "glaze/zmem.hpp"

// Write
MyStruct data{...};
std::string buffer;
auto err = glz::write_zmem(data, buffer);

// Read
MyStruct result;
err = glz::read_zmem(result, buffer);
```

### Pre-allocated Serialization (Maximum Performance)

```cpp
// Compute exact size, allocate once, write without bounds checks
std::string buffer;
auto err = glz::write_zmem_preallocated(data, buffer);
```

### Size Computation

```cpp
// Get exact serialized size without actually serializing
size_t size = glz::size_zmem(data);
```

## Supported Types

ZMEM supports any type that Glaze can reflect:

- **Reflectable aggregates**: Structs with public members (automatic reflection)
- **glaze_object_t types**: Structs with explicit `glz::meta<T>` metadata
- **Containers**: `std::vector`, `std::array`, `std::map`, `std::string`
- **Optionals**: `std::optional<T>`, `glz::zmem::optional<T>`
- **Nested structures**: Any combination of the above

```cpp
// Automatic reflection (reflectable aggregate)
struct Point {
    float x, y;
};

// Explicit metadata (glaze_object_t)
struct Entity {
    uint64_t id;
    std::string name;
};

template<>
struct glz::meta<Entity> {
    using T = Entity;
    static constexpr auto value = object(
        "id", &T::id,
        "name", &T::name
    );
};
```

## Design Philosophy

1. **Fixed types pay nothing**: If your struct is trivially copyable, serialization is just `memcpy`

2. **Predictable layout**: The wire format matches memory layout for fixed types, making debugging straightforward

3. **Little-endian**: All multi-byte values use little-endian encoding (matches x86/ARM)

4. **Optional schema**: C++ with Glaze uses compile-time reflection (no schema file needed). For cross-language use, an optional `.zmem` schema language is available

5. **Offset-based references**: Variable data uses offsets (not pointers), enabling zero-copy deserialization and memory mapping
