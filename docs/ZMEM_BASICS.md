# ZMEM Format Basics

A beginner-friendly guide to understanding the ZMEM binary serialization format.

## What is ZMEM?

ZMEM (Zero-copy Memory Format) is a high-performance binary serialization format designed for:

- **Zero overhead** for fixed structs (direct memory representation)
- **Zero-copy reads** where possible (data can be accessed in-place)
- **Predictable performance** with no schema compilation required

## Core Concept: Fixed vs Variable Types

ZMEM categorizes all types into two categories:

### Fixed Types

Fixed types are **trivially copyable** - they can be serialized with a direct `memcpy`. These have **zero serialization overhead**.

```cpp
// Fixed types - written directly to wire
int32_t x = 42;           // 4 bytes on wire
float y = 3.14f;          // 4 bytes on wire
bool flag = true;         // 1 byte on wire

// Fixed struct - also zero overhead!
struct Point {
    float x, y;
};
Point p{1.0f, 2.0f};      // 8 bytes on wire (same as sizeof(Point))
```

**Fixed types include:**
- All primitives (integers, floats, bools)
- Enums
- `std::array<T, N>` where T is fixed
- C arrays (`T[N]`) where T is fixed
- Structs containing only fixed types

### Variable Types

Variable types contain **variable-length data** like vectors or strings. They need additional structure to track where the variable data lives.

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

**Size Header (8 bytes):** Total byte count of inline + variable sections

**Inline Section:** Fixed-size portion containing:
- Fixed members written directly
- 16-byte references for vectors/strings (offset + count/length)

**Variable Section:** Actual data for vectors and strings

#### Example: Variable Struct

```cpp
struct Entity {
    uint64_t id;
    std::vector<int32_t> values;
};

Entity e{42, {10, 20, 30}};
```

Wire layout:

```
Offset  Content
──────  ───────────────────────────────────
0       [Size: 36 bytes (8 + 16 + 12)]     ← 8-byte header
8       [id: 42]                           ← inline: uint64_t
16      [offset: 24, count: 3]             ← inline: vector reference
32      [10] [20] [30]                     ← variable: vector data
──────  ───────────────────────────────────
Total: 44 bytes (8 header + 36 content)
```

The vector reference contains:
- **offset**: Byte position of data relative to byte 8 (here: 24)
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

Wire layout:

```
Offset  Content
──────  ───────────────────────────────────
0       [Size: 26 bytes]                   ← 8-byte header
8       [timestamp: 1000]                  ← inline: uint64_t
16      [offset: 24, length: 2]            ← inline: string reference
32      [H] [i]                            ← variable: string data (no null terminator)
──────  ───────────────────────────────────
Total: 34 bytes
```

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
| Primitives | Direct bytes (little-endian) | 0 bytes |
| Fixed struct | Direct bytes (with padding) | 0 bytes |
| Fixed array | Direct bytes | 0 bytes |
| Variable struct | 8-byte header + inline + variable | 8 bytes + refs |
| Vector (fixed T) | 8-byte count + data | 8 bytes |
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

4. **No schema compilation**: Unlike Cap'n Proto or FlatBuffers, no separate build step required

5. **Offset-based references**: Variable data uses offsets (not pointers), enabling zero-copy deserialization and memory mapping
