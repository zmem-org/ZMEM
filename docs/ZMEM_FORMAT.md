# ZMEM Binary Format Specification

**Version**: 1.0
**Status**: Draft

## Overview

ZMEM is a binary serialization format designed for maximum performance zero-copy deserialization. The format enables direct `memcpy` operations with **zero overhead** for fixed structs, while maintaining cross-language compatibility through strict layout rules and compile-time validation.

### Goals

1. **Maximum performance** - Zero header overhead for fixed structs
2. **Zero-copy deserialization** - Direct memory mapping without parsing
3. **Cross-language compatibility** - Deterministic layout across C++, Rust, and C
4. **Compile-time type safety** - Static validation of struct conformance

### Design Philosophy

ZMEM prioritizes performance. It requires that a sender and receiver agree on the schema at compile time. There is no runtime type verification in the wire format. This trade-off enables zero-overhead serialization for fixed structs.

### Non-Goals

- Variable-length strings (use fixed-size `str[N]` instead)
- Variable-length map keys (use integer or `str[N]` keys)
- Schema evolution / backwards compatibility
- Compression
- Self-describing format (requires schema knowledge)

### Schema Compatibility

**ZMEM requires identical schemas on all communicating parties.** This is a deliberate design choice for peak performance.

**What this means**:
- Sender and receiver must use the exact same struct definitions
- Field names, types, and order must match exactly
- Adding, removing, or reordering fields breaks compatibility
- All parties must be recompiled together when schemas change

**How mismatches are detected**:
- Each type has a canonical type signature string
- Sender and receiver should validate matching signatures at compile/link time
- Mismatched schemas that slip through cause **silent data corruption**

**Rationale**:
- Zero runtime overhead (no version checks, no field tags, no vtables)
- Fixed offsets enable direct pointer casts and `memcpy`
- Compile-time validation catches errors before deployment
- Matches requirements of IPC, real-time systems, HFT, game engines

ZMEM is the right choice when all parties are deployed together and peak performance matters more than independent versioning.

### Memory Model and Mutable State

**ZMEM fixed structs can serve as your application's mutable data model** directly, without separate builder types or arena allocation. This provides the enormous benefit of being able to use language level structs directly.

#### Native Structs as State

ZMEM fixed structs have **no builder pattern and no arena**. They are ordinary language structs with wire-compatible layout:

```cpp
// ZMEM fixed struct - it's just a normal C++ struct
struct Player {
    uint64_t id;
    char name[64];
    float position[3];
    float health;
};

// Use as mutable application state
Player player;
player.id = 12345;
std::strcpy(player.name, "Alice");
player.health = 100.0f;

// Modify freely over application lifetime - normal memory semantics
player.health -= 25.0f;
std::strcpy(player.name, "Bob");  // No leak, no arena bloat

// Serialize only when needed (zero-copy!)
send(socket, &player, sizeof(Player));

// Receive and use directly
Player received;
recv(socket, &received, sizeof(Player));
// received is immediately usable - no deserialization step
```

#### Variable Structs

For variable structs (those with vector fields), ZMEM uses a different approach:

1. **Application state**: Use native language types (`std::vector<T>`, `Vec<T>`)
2. **Serialization**: Convert to ZMEM wire format when transmitting
3. **Deserialization**: Convert back to native types, or use zero-copy views for read-only access

#### When to Use Each Approach

| Data Shape | Recommendation |
|------------|----------------|
| Fixed-size, frequently modified | Fixed struct as mutable state |
| Fixed-size, read-heavy | Fixed struct or memory-mapped file |
| Variable-size, write-once | Serialize directly to buffer |
| Variable-size, frequently modified | Native types → serialize when needed |

### Random Access Reads

ZMEM supports efficient random access to message content. You can `mmap()` a large file containing serialized data and read specific fields without paging the entire file from disk.

#### ZMEM Random Access

ZMEM provides O(1) random access to fields, with the access pattern depending on struct category:

##### Fixed Structs: O(1) Direct Access

Fixed structs have **compile-time-known offsets**. No parsing, no pointer chasing:

```cpp
// Memory-map a 2GB file containing Player records
auto* data = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

// Access any field directly - only that page faults in
struct Player {
    uint64_t id;       // offset 0
    char name[64];     // offset 8
    float position[3]; // offset 72
    float health;      // offset 84
};

auto* players = reinterpret_cast<const Player*>(data + 8);  // skip count header

// Random access to player 1,000,000's health - O(1), single page fault
float health = players[1000000].health;

// The OS only pages in the 4KB page containing that field
// The other 1,999,996 KB remain untouched on disk
```

Key advantages:
- No pointer indirection (offsets are compile-time constants)
- Direct address arithmetic: `base + index * sizeof(T) + field_offset`

##### Variable Structs: O(1) with Offset Lookup

Variable structs (with vector fields) use offset-based access:

```cpp
// Inline fields: O(1) direct access (compile-time offsets)
uint64_t id = read_u64(data + 8);  // First field after size header

// Vector fields: O(1) with one indirection
uint64_t weights_offset = read_u64(data + 16);
uint64_t weights_count = read_u64(data + 24);
const float* weights = reinterpret_cast<const float*>(data + 8 + weights_offset);
// Access weights[i] directly
```

##### Arrays of Variable Structs: O(1) via Offset Table

For `[VariableStruct]`, an offset table enables random access:

```cpp
// Offset table at start of vector data
// offsets[i] gives byte position of element i

uint64_t element_offset = read_u64(offset_table + i * 8);
const uint8_t* element = data_start + element_offset;
// Now access fields within element
```

##### Nested Vectors: O(depth) Access

For deeply nested structures like `[[[T]]]`, random access requires traversing offset tables at each level—O(depth) lookups.

#### Memory-Mapped File Access

ZMEM is ideal for memory-mapped files:

```cpp
// Map a file containing an array of 10 million Particle structs
int fd = open("particles.zmem", O_RDONLY);
struct stat st;
fstat(fd, &st);

auto* data = static_cast<const uint8_t*>(
    mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)
);

// Read element count
uint64_t count = *reinterpret_cast<const uint64_t*>(data);

// Create zero-copy view
auto particles = std::span<const Particle>(
    reinterpret_cast<const Particle*>(data + 8),
    count
);

// Random access - only touched pages are loaded
Particle p = particles[5000000];  // Single page fault

// Sequential scan - OS prefetches efficiently
for (const auto& p : particles) {
    // Kernel read-ahead optimizes this
}

// Cleanup
munmap(const_cast<uint8_t*>(data), st.st_size);
close(fd);
```

#### Access Complexity Summary

| Data Structure | Access Pattern | Complexity |
|---------------|----------------|------------|
| Fixed struct field | `base + compile_time_offset` | O(1) |
| Fixed struct array element | `base + 8 + i * sizeof(T)` | O(1) |
| Variable struct inline field | `base + 8 + compile_time_offset` | O(1) |
| Variable struct vector element | Offset lookup + index | O(1) |
| `[VariableStruct]` element | Offset table lookup | O(1) |
| `[[T]]` nested element | 2 offset table lookups | O(1) |
| `[[[T]]]` deeply nested | depth × offset lookups | O(depth) |

---

## Schema Format

ZMEM uses a minimal schema language for defining types. Schema files use the `.zmem` extension.

### Basic Syntax

```
version 1.0.0

# Type aliases (zero-cost abstractions)
type EntityId = u64
type Timestamp = i64
type Name = str[64]

enum Status : u8 {
  Inactive = 0
  Active = 1
  Paused = 2
}

union Result : u32 {
  Ok = 0 { value::u64 }
  Err = 1 { code::i32, message::str[128] }
}

struct Vec3 {
  x::f32
  y::f32
  z::f32
}

struct Entity {
  id::EntityId              # type alias
  name::Name                # type alias (expands to str[64])
  created::Timestamp        # type alias
  status::Status            # enum field
  result::Result            # union field
  weights::[f32]            # vector of f32
  position::Vec3
  flags::u8[4]              # fixed array of 4 u8s
  tags::map<str[32], i32>   # string-keyed map
}
```

### Grammar

```
schema      = version_decl (namespace_decl | import_decl | const_def | type_def | struct_def | enum_def | union_def | comment | blank)*
version_decl = "version" version_num "." version_num "." version_num   # required, must be first
version_num = [0-9]+                                       # non-negative integer
namespace_decl = "namespace" identifier
import_decl = "import" path ("as" identifier)?
path        = identifier ("." identifier)*                 # e.g., math.zmem, physics.zmem
const_def   = "const" identifier "::" const_type "=" const_value   # named constant (codegen only)
const_type  = primitive | str_type | primitive "[" integer "]"   # primitives, strings, or arrays of primitives
const_value = literal | array_literal | identifier              # identifier = reference to another constant
type_def    = "type" identifier "=" alias_type                  # type alias (zero-cost)
alias_type  = primitive | str_type | opt_type | map_type | identifier
            | alias_type "[" integer "]"         # fixed arrays allowed
struct_def  = "struct" identifier "{" field* "}"
enum_def    = "enum" identifier ":" int_primitive "{" enum_variant* "}"
enum_variant = identifier ("=" integer)? "default"?   # mark default variant
union_def   = "union" identifier (":" int_primitive)? "{" union_variant+ "}"
union_variant = identifier ("=" integer)? ("::" identifier | "{" field* "}")?
              # unit: Name = N
              # type ref: Name = N :: ExistingType
              # struct: Name = N { field::type, ... }
field       = identifier "::" type ("=" default_value)?

# Type grammar (unambiguous: [N] suffix cannot apply to vectors)
atomic_type = primitive | str_type | string_type | opt_type | map_type | identifier
fixed_type  = atomic_type
            | fixed_type "[" integer "]"         # fixed array, chainable: f32[4][4]
type        = fixed_type
            | "[" type "]"                       # vector (prefix notation, any element)

primitive   = "bool" | "i8" | "i16" | "i32" | "i64" | "i128"
            | "u8" | "u16" | "u32" | "u64" | "u128" | "f16" | "f32" | "f64" | "bf16"
str_type    = "str[" integer "]"              # fixed-size string, N ≥ 1
string_type = "string"                        # variable-length string (no size limit)
opt_type    = "opt<" type ">"                 # optional value
map_type    = "map<" key_type "," type ">"
key_type    = int_primitive | str_type | identifier  # identifier = enum/alias type
int_primitive = "i8" | "i16" | "i32" | "i64" | "i128" | "u8" | "u16" | "u32" | "u64" | "u128"

# Default values (no wire format impact - codegen only)
default_value = literal_value | identifier            # identifier = constant or enum variant
literal_value = literal | array_literal | struct_literal  # recursive for nesting
literal     = int_literal | float_literal | string_literal | bool_literal
int_literal = "-"? [0-9]+
float_literal = "-"? [0-9]+ "." [0-9]+ (("e" | "E") "-"? [0-9]+)?
string_literal = '"' [^"]* '"'
bool_literal = "true" | "false"
array_literal = "[" (literal_value ("," literal_value)*)? "]"  # [1, 2, 3] or [[1,2],[3,4]]
struct_literal = "{" field_init ("," field_init)* "}"          # {x = 0.0, y = 0.0, z = 0.0}
field_init  = identifier "=" literal_value                     # field designator

identifier  = [a-zA-Z_][a-zA-Z0-9_]*
integer     = "-"? [0-9]+                     # signed for enums with signed type
comment     = "#" .* newline
```

#### Grammar Disambiguation

The grammar is **unambiguous** by design. The `[N]` suffix (fixed array) can only be applied to `atomic_type` or another `fixed_type`, never to a vector:

| Expression | Parse | Meaning |
|------------|-------|---------|
| `f32[4]` | `fixed_type` | Fixed array of 4 f32 |
| `f32[4][4]` | `fixed_type` (chained) | 2D fixed array |
| `[f32]` | `"[" type "]"` | Vector of f32 |
| `[[f32]]` | `"[" type "]"` nested | Nested vector |
| `[f32[4]]` | `"[" fixed_type "]"` | Vector of fixed arrays |
| `[f32][4]` | **SYNTAX ERROR** | Cannot apply `[4]` to a vector |

To express "vector of fixed arrays", use `[f32[4]]` (unambiguous).

This design ensures:
- No parsing ambiguity between prefix `[T]` and suffix `[N]`
- Clear mental model: suffix binds to base types only
- Explicit syntax prevents accidental misinterpretation

### Type Syntax

| Syntax | Meaning | Example |
|--------|---------|---------|
| `T` | Primitive, enum, union, struct, or alias | `f32`, `Status`, `Result`, `Vec3`, `EntityId` |
| `str[N]` | Fixed-size string (N bytes, null-terminated) | `str[64]`, `str[4096]` |
| `string` | Variable-length string (no size limit) | `string` |
| `opt<T>` | Optional value (present flag + value) | `opt<u64>`, `opt<str[32]>` |
| `[T]` | Vector (prefix notation, any element) | `[f32]`, `[Entity]`, `[[i32]]`, `[string]` |
| `T[N]` | Fixed array (suffix notation, fixed-size T only) | `u8[16]`, `Vec3[4]`, `f32[4][4]` |
| `[T[N]]` | Vector of fixed arrays | `[f32[4]]`, `[Vec3[3]]` |
| `map<K,V>` | Flat map (vector of key-value pairs) | `map<u64,f32>`, `map<str[32],Config>` |
| `type A = T` | Type alias (zero-cost) | `type EntityId = u64` |
| `const N::T = V` | Named constant (codegen only, type required) | `const MAX_SIZE::u32 = 64`, `const PI::f64 = 3.14159` |
| `field = val` | Default value (codegen only) | `count::u32 = 0`, `pos::Vec3 = {x = 0, y = 0, z = 0}` |

**Note**: `[T][N]` is a syntax error. The `[N]` suffix binds only to atomic types (see Grammar Disambiguation).

### Schema Version

Every ZMEM schema file **must** begin with a version declaration:

```
version 1.0.0
```

**Format**: `version major.minor.patch` (all three integers required)

| Component | Meaning | Compatibility |
|-----------|---------|---------------|
| Major | Breaking grammar changes | Parsers for version N may not parse version N+1 |
| Minor | New grammar features | Backward compatible (old schemas still valid) |
| Patch | Spec clarifications, bug fixes | No grammar impact |

**Rules**:
- Version declaration must be the first non-comment line
- All three version components are required
- Current version: `1.0.0`

**Example**:
```
version 1.0.0

namespace myapp

struct User {
  id::u64
  name::str[64]
}
```

**Tooling behavior**:
- Parsers should reject schemas with unsupported major versions
- Parsers should warn on schemas with newer minor versions
- Patch version differences are always safe to ignore

### Namespaces

```
namespace graphics

struct Vertex {
  position::Vec3
  color::u8[4]
}
```

Namespaces map to:
- C++: `namespace graphics { ... }`
- Rust: `mod graphics { ... }`
- C: `graphics_Vertex` prefix

### Imports

```
import math.zmem            # import all types
import physics.zmem as phys # namespaced import

struct World {
  origin::Vec3           # from math.zmem
  bodies::[phys.Body]    # from physics.zmem
}
```

### Complete Example

```
# geometry.zmem
version 1.0.0

namespace geo

# Size constants
const NAME_LEN::u32 = 64
const PATH_LEN::u32 = 256
const MSG_LEN::u32 = 128
const KEY_LEN::u32 = 32

# Default value constants
const WHITE::f32[4] = [1.0, 1.0, 1.0, 1.0]
const ORIGIN::f32[3] = [0.0, 0.0, 0.0]
const UNIT_SCALE::f32[3] = [1.0, 1.0, 1.0]

# Type aliases for semantic clarity
type MeshId = u64
type MaterialId = u64
type Name = str[NAME_LEN]
type Path = str[PATH_LEN]
type Color = f32[4]

enum RenderMode : u8 {
  Solid = 0 default                  # default render mode
  Wireframe = 1
  Points = 2
}

enum CullFace : u8 {
  None = 0
  Front = 1
  Back = 2 default                   # default cull mode
}

union LoadResult : u32 {
  Success = 0 { mesh_id::MeshId }
  FileNotFound = 1 { path::Path }
  ParseError = 2 { line::u32, column::u32, message::str[MSG_LEN] }
  OutOfMemory = 3
}

struct Vec2 {
  x::f32 = 0.0
  y::f32 = 0.0
}

struct Vec3 {
  x::f32 = 0.0
  y::f32 = 0.0
  z::f32 = 0.0
}

struct Transform {
  position::f32[3] = ORIGIN          # uses constant default
  rotation::f32[3] = ORIGIN          # uses constant default
  scale::f32[3] = UNIT_SCALE         # uses constant default [1,1,1]
}

struct Material {
  id::MaterialId
  name::Name = ""                    # empty string default
  color::Color = WHITE               # uses constant default
  render_mode::RenderMode            # uses enum default (Solid)
  cull_face::CullFace                # uses enum default (Back)
  properties::map<str[KEY_LEN], f32>   # fixed map: {"roughness": 0.5}
  textures::map<str[KEY_LEN], [u8]>    # variable map: {"diffuse": [...bytes...]}
}

struct Mesh {
  id::MeshId
  name::Name = ""
  vertices::[Vec3]
  normals::[Vec3]
  indices::[u32]
  transform::Transform
  material::Material
}

# Examples of variable-length strings and nested vectors

struct LogEntry {
  timestamp::u64
  level::u8
  message::string              # variable-length string
  source::str[PATH_LEN]        # fixed-length for indexing
}

struct Document {
  title::string                # variable-length
  content::string              # variable-length
  tags::[string]               # vector of variable-length strings
}

struct Matrix {
  rows::[[f32]]                # nested vector: vector of vectors
}

struct Scene {
  name::string
  meshes::[Mesh]               # vector of variable type (Mesh has vectors)
}

struct Animation {
  name::str[NAME_LEN]
  keyframes::[[f32]]           # nested: each keyframe is a vector of floats
  bone_indices::[u32]
}
```

### Code Generation

The schema compiler generates:
- Type definitions with correct layout
- Compile-time hash constants for validation
- Serialization/deserialization functions
- Zero-copy view types

Example output (C++):
```cpp
// Generated from geometry.zmem
namespace geo {

struct Vec3 {
    float x;
    float y;
    float z;
};

static_assert(sizeof(Vec3) == 12);
static_assert(offsetof(Vec3, x) == 0);
static_assert(offsetof(Vec3, y) == 4);
static_assert(offsetof(Vec3, z) == 8);

// Type signature: "Vec3{x::f32,y::f32,z::f32}"

} // namespace geo
```

---

## Type System

### Constants

Constants are named values known at compile time. They are a codegen-only feature with **no wire format impact**.

```
# Size constants (for array dimensions)
const MAX_NAME_LENGTH::u32 = 64
const MAX_PLAYERS::u32 = 32
const MATRIX_DIM::u32 = 4

# Typed constants
const DEFAULT_PORT::u16 = 8080
const DEFAULT_TIMEOUT::u64 = 30000
const PI::f64 = 3.14159265358979

# Array constants (for defaults)
const ORIGIN::f32[3] = [0.0, 0.0, 0.0]
const WHITE::u8[4] = [255, 255, 255, 255]

# String and boolean constants
const DEFAULT_HOST::str[16] = "localhost"
const DEBUG_MODE::bool = false
```

#### Syntax

```
const NAME::type = value          # type is required
```

- **NAME**: Identifier for the constant (conventionally UPPER_SNAKE_CASE)
- **type**: Required type annotation (primitive, `str[N]`, or `primitive[N]`)
- **value**: Literal, array literal, or reference to another constant

#### What Constants Can Be

| Category | Example | Valid |
|----------|---------|-------|
| Integer literals | `const SIZE::u32 = 64` | ✓ |
| Float literals | `const PI::f64 = 3.14159` | ✓ |
| Boolean literals | `const ENABLED::bool = true` | ✓ |
| String literals | `const NAME::str[16] = "default"` | ✓ |
| Array literals | `const ORIGIN::f32[3] = [0.0, 0.0, 0.0]` | ✓ |
| Other constants | `const TIMEOUT::u64 = DEFAULT_TIMEOUT` | ✓ |
| Computed expressions | `const HALF::u32 = SIZE / 2` | ✗ (not supported) |

#### Where Constants Can Be Used

| Context | Example | Notes |
|---------|---------|-------|
| Array sizes | `data::f32[SIZE]` | Must be positive integer |
| Default values | `port::u16 = DEFAULT_PORT` | Type must match |
| Other constants | `const B::u32 = A` | No circular references |

#### Wire Format

**Constants have no wire format impact.** They are purely a schema-level abstraction for codegen convenience. When a constant is used:
- In an array size: the literal value is used
- As a default: the literal value is used

#### Type Signatures

**Constants are expanded to their literal values** in type signatures. The constant name never appears in the signature:

```
const SIZE::u32 = 4

struct Data {
  values::f32[SIZE]
}
```

Signature: `Data{values::f32[4]}`

#### Constraints

| Constraint | Description |
|------------|-------------|
| No forward references | Constants must be defined before use |
| No circular references | `const A::u32 = B; const B::u32 = A;` is invalid |
| Array size type | Array size constants must evaluate to positive integers |
| Type compatibility | Constant type must match usage context |
| No expressions | Arithmetic and computed values are not supported |

#### C++ Mapping

```cpp
// Integer constants -> constexpr (size_t for array sizes)
constexpr size_t MAX_NAME_LENGTH = 64;
constexpr size_t MAX_PLAYERS = 32;

// Typed constants -> constexpr with explicit type
constexpr uint16_t DEFAULT_PORT = 8080;
constexpr uint64_t DEFAULT_TIMEOUT = 30000;
constexpr double PI = 3.14159265358979;

// Array constants -> constexpr std::array
constexpr std::array<float, 3> ORIGIN = {0.0f, 0.0f, 0.0f};
constexpr std::array<uint8_t, 4> WHITE = {255, 255, 255, 255};

// String constants -> constexpr string_view
constexpr std::string_view DEFAULT_HOST = "localhost";

// Boolean constants
constexpr bool DEBUG_MODE = false;
```

#### Complete Example

```
const MAX_NAME::u32 = 64
const MAX_ITEMS::u32 = 100
const DEFAULT_PORT::u16 = 8080
const DEFAULT_TIMEOUT::u64 = 30000
const ORIGIN::f32[3] = [0.0, 0.0, 0.0]
const WHITE::u8[4] = [255, 255, 255, 255]

struct Vec3 {
  x::f32 = 0.0
  y::f32 = 0.0
  z::f32 = 0.0
}

struct ServerConfig {
  host::str[MAX_NAME] = "localhost"
  port::u16 = DEFAULT_PORT
  timeout::u64 = DEFAULT_TIMEOUT
}

struct Player {
  id::u64
  name::str[MAX_NAME]
  position::f32[3] = ORIGIN
  color::u8[4] = WHITE
}

struct Inventory {
  items::Item[MAX_ITEMS]
}
```

---

### Type Aliases

Type aliases create named references to existing types. They are a zero-cost abstraction with no wire format impact.

```
type EntityId = u64
type Timestamp = i64
type Vec2 = f32[2]
type Color = u8[4]
type Name = str[64]
type MaybeScore = opt<f32>
type Settings = map<str[32], f64>
```

#### Syntax

```
type AliasName = UnderlyingType
```

- **AliasName**: Identifier for the new type name
- **UnderlyingType**: Any valid ZMEM type

#### What Can Be Aliased

| Category | Example | Valid |
|----------|---------|-------|
| Primitives | `type Score = f64` | ✓ |
| Fixed arrays | `type Vec3 = f32[3]` | ✓ |
| Strings | `type Name = str[64]` | ✓ |
| Optionals | `type MaybeId = opt<u64>` | ✓ |
| Maps | `type Config = map<str[32], f64>` | ✓ |
| Structs | `type Point = Vec3` | ✓ |
| Enums | `type State = Status` | ✓ |
| Unions | `type Outcome = Result` | ✓ |
| Other aliases | `type UserId = EntityId` | ✓ |
| Vectors | `type Numbers = [f32]` | ✗ (vectors only in struct fields) |

#### Wire Format

**Type aliases have no wire format impact.** They are purely a schema-level abstraction. The alias is fully expanded to its underlying type in all contexts.

```
type EntityId = u64

struct Entity {
  id::EntityId    # Wire format: u64 (8 bytes)
}
```

The wire format for `Entity` is identical whether you use `EntityId` or `u64` directly.

#### Type Signatures

In type signatures, **aliases are always expanded** to their underlying type. This ensures wire compatibility between schemas that use different alias names for the same underlying type.

```
type EntityId = u64
type Timestamp = i64

struct Event {
  id::EntityId
  created::Timestamp
}
```

Type signature: `Event{id::u64,created::i64}` (NOT `Event{id::EntityId,created::Timestamp}`)

**Rationale**: Two programs with different alias names but identical underlying types should be wire-compatible.

#### Constraints

- **No circular references**: An alias cannot directly or indirectly reference itself
  ```
  type A = B
  type B = A    # Error: circular reference
  ```

- **Must resolve to concrete type**: Aliases must ultimately resolve to a primitive, struct, enum, or union

- **Order-independent**: Aliases can be declared in any order (forward references allowed)
  ```
  type UserId = EntityId    # OK: EntityId defined later
  type EntityId = u64
  ```

#### Use Cases

**Semantic clarity**:
```
type UserId = u64
type PostId = u64
type Timestamp = i64

struct Post {
  id::PostId
  author::UserId
  created::Timestamp
}
```

**Domain modeling**:
```
type Meters = f64
type Seconds = f64
type MetersPerSecond = f64

struct Velocity {
  x::MetersPerSecond
  y::MetersPerSecond
  z::MetersPerSecond
}
```

**Compound type shorthand**:
```
type Vec2 = f32[2]
type Vec3 = f32[3]
type Vec4 = f32[4]
type Mat4 = f32[16]
type Color = u8[4]

struct Vertex {
  position::Vec3
  normal::Vec3
  uv::Vec2
  color::Color
}
```

#### C++ Mapping

```cpp
// Generated from type aliases
using EntityId = uint64_t;
using Timestamp = int64_t;
using Vec2 = std::array<float, 2>;
using Color = std::array<uint8_t, 4>;
using Name = char[64];  // or std::array<char, 64>

struct Event {
    EntityId id;        // uint64_t
    Timestamp created;  // int64_t
};

// Codegen may also provide type traits
template<> struct zmem_alias<EntityId> {
    using underlying = uint64_t;
    static constexpr std::string_view name = "EntityId";
};
```

### Default Values

Default values provide initialization hints for code generation. **They have no wire format impact** — defaults are purely a schema-level convenience.

```
struct GameConfig {
  max_players::u32 = 16
  tick_rate::f32 = 60.0
  server_name::str[64] = "Default Server"
  friendly_fire::bool = false
  team_colors::u8[4] = [255, 0, 0, 255]
}
```

#### Syntax

```
field::type = default_value
```

- **field**: Field name
- **type**: Any valid ZMEM type
- **default_value**: Compile-time constant matching the type

#### Supported Default Types

| Type | Syntax | Example |
|------|--------|---------|
| Integers | Integer literal | `count::u32 = 100` |
| Floats | Float literal | `scale::f32 = 1.0` |
| Booleans | `true` / `false` | `enabled::bool = true` |
| Strings | String literal | `name::str[64] = "unnamed"` |
| Fixed arrays | Array literal | `color::u8[4] = [255, 255, 255, 255]` |
| Multi-dim arrays | Nested array literal | `mat::f32[2][2] = [[1, 0], [0, 1]]` |
| Fixed structs | Struct literal | `pos::Vec3 = {x = 0.0, y = 0.0, z = 0.0}` |
| Optionals | Value when absent | `timeout::opt<u64> = 5000` |
| Enums | Variant name | `status::Status = Active` |

#### NOT Supported

| Type | Reason |
|------|--------|
| Variable structs | Contain vectors, strings, maps, or optionals |
| Unions | Ambiguous which variant |
| Vectors | Variable size |
| Maps | Variable size |

#### Wire Format

**Defaults have NO wire format impact.** All fields are always fully present on the wire.

```
struct Config {
  timeout::u64 = 5000
}
```

Wire format (unchanged):
```
┌─────────────────┐
│ timeout (8B)    │   ← Always present, always 8 bytes
└─────────────────┘
```

Defaults only affect:
1. Generated code (default member initializers)
2. Optional accessor methods (`value_or`)
3. Documentation / schema understanding

#### Field Defaults

Field defaults provide initialization values for generated code:

```
struct NetworkConfig {
  port::u16 = 8080
  timeout_ms::u32 = 30000
  max_connections::u32 = 1000
  hostname::str[256] = "localhost"
  use_tls::bool = true
}
```

#### Optional Defaults

For optional fields, defaults specify the fallback value when `present == 0`:

```
struct Request {
  timeout_ms::opt<u64> = 30000     # 30 seconds if unset
  max_retries::opt<u32> = 3
  priority::opt<f32> = 1.0
}
```

The default is used by generated accessor methods:
```cpp
uint64_t get_timeout_ms() const {
    return timeout_ms.value_or(30000);
}
```

#### Array Defaults

Fixed arrays support array literal defaults:

```
struct Vertex {
  position::f32[3] = [0.0, 0.0, 0.0]
  color::u8[4] = [255, 255, 255, 255]    # White, fully opaque
}

struct Transform2D {
  matrix::f32[2][2] = [[1, 0], [0, 1]]   # Identity matrix
}
```

**Constraints**:
- Array literal length must match the array dimension
- All elements must be the same type
- Multi-dimensional arrays use nested literals

#### Struct Defaults

Fixed structs support struct literal defaults using designated initializer syntax:

```
struct Vec3 {
  x::f32
  y::f32
  z::f32
}

struct Color {
  r::u8
  g::u8
  b::u8
  a::u8
}

struct Vertex {
  position::Vec3 = {x = 0.0, y = 0.0, z = 0.0}
  normal::Vec3 = {x = 0.0, y = 1.0, z = 0.0}
  color::Color = {r = 255, g = 255, b = 255, a = 255}
}
```

**Nested struct defaults**:

```
struct Transform {
  position::Vec3
  scale::Vec3
}

struct Entity {
  transform::Transform = {
    position = {x = 0.0, y = 0.0, z = 0.0},
    scale = {x = 1.0, y = 1.0, z = 1.0}
  }
}
```

**Constraints**:
- **Declaration order**: Fields must be listed in declaration order (like C++20 designated initializers)
- **All fields required**: Every field must be specified (no partial initialization)
- **Fixed structs only**: The struct being defaulted must be a fixed struct (no vectors, strings, maps, or optionals)
- **Field names must match**: Each designator must name a valid field

```
# Valid
position::Vec3 = {x = 0.0, y = 0.0, z = 0.0}

# Invalid: wrong order
position::Vec3 = {y = 0.0, x = 0.0, z = 0.0}   # Error: fields out of order

# Invalid: missing field
position::Vec3 = {x = 0.0, y = 0.0}             # Error: missing z

# Invalid: variable struct
data::VariableStruct = {...}                     # Error: contains vector/string/map/optional
```

**Why fixed structs only?**

Variable structs contain variable-size or heap-allocated data (vectors, strings, maps) or require special handling (optionals). Defaults for these types would require:
- Heap allocation at initialization time
- Complex literal syntax for variable-length data
- Ambiguous semantics for optionals (present with value vs absent)

Fixed structs are contiguous, fixed-size, and can be initialized with a direct memory copy.

#### Enum Defaults

Enums can mark a default variant with the `default` keyword:

```
enum ConnectionState : u8 {
  Disconnected = 0 default
  Connecting = 1
  Connected = 2
  Error = 3
}
```

**Rules**:
- At most one variant can be marked `default`
- The default variant SHOULD have value 0 (for zero-initialization safety)
- If no variant is marked default, the first variant (value 0) is implicitly default

Using enum in a field with default:

```
enum Priority : u8 {
  Low = 0
  Normal = 1 default
  High = 2
  Critical = 3
}

struct Task {
  id::u64
  priority::Priority = Normal    # Default to Normal priority
}
```

#### Type Signatures

**Defaults are NOT included in type signatures.** They don't affect wire compatibility.

```
struct Config {
  timeout::u64 = 5000
}
```

Type signature: `Config{timeout::u64}` (NOT `Config{timeout::u64=5000}`)

**Rationale**: Two schemas with different defaults but same types are wire-compatible. Only the structure matters for wire format.

#### Constraints

1. **Type match**: Default must match the field type
   ```
   timeout::u64 = "hello"    # Error: string for u64
   ```

2. **Compile-time constant**: Defaults must be literal values
   ```
   timestamp::u64 = now()    # Error: not a constant
   ```

3. **Array length match**: Array defaults must have correct dimensions
   ```
   color::u8[4] = [1, 2, 3]  # Error: expected 4 elements
   ```

4. **Enum variant exists**: Enum defaults must name a valid variant
   
   ```
   status::Status = Unknown  # Error if Unknown not defined
   ```
   
5. **No defaults for variable types**: Variable structs, unions, vectors, maps cannot have defaults

   ```
   data::VariableStruct = ???  # Error: variable struct (contains vector/string/map/optional)
   result::Result = ???       # Error: union
   items::[Item] = ???        # Error: vector
   config::map<K,V> = ???     # Error: map
   ```

   Fixed structs (containing only primitives, `str[N]`, fixed arrays, and other fixed structs) DO support defaults:

   ```
   position::Vec3 = {x = 0.0, y = 0.0, z = 0.0}   # OK: fixed struct
   ```

#### C++ Mapping

**Field defaults** become default member initializers:

```cpp
// struct NetworkConfig { port::u16 = 8080, use_tls::bool = true, ... }
struct NetworkConfig {
    uint16_t port = 8080;
    uint32_t timeout_ms = 30000;
    uint32_t max_connections = 1000;
    char hostname[256] = "localhost";
    bool use_tls = true;
};
```

**Optional defaults** provide `value_or` accessors:

```cpp
// struct Request { timeout_ms::opt<u64> = 30000, max_retries::opt<u32> = 3 }
struct Request {
    zmem_optional<uint64_t> timeout_ms;
    zmem_optional<uint32_t> max_retries;

    uint64_t get_timeout_ms() const { return timeout_ms.value_or(30000); }
    uint32_t get_max_retries() const { return max_retries.value_or(3); }
};
```

**Array defaults**:

```cpp
// struct Vertex { color::u8[4] = [255, 255, 255, 255] }
struct Vertex {
    uint8_t color[4] = {255, 255, 255, 255};
};
```

**Struct defaults** (C++20 designated initializers):

```cpp
// struct Vec3 { x::f32, y::f32, z::f32 }
struct Vec3 {
    float x;
    float y;
    float z;
};

// struct Vertex { position::Vec3 = {x = 0.0, y = 0.0, z = 0.0}, ... }
struct Vertex {
    Vec3 position = {.x = 0.0f, .y = 0.0f, .z = 0.0f};
    Vec3 normal = {.x = 0.0f, .y = 1.0f, .z = 0.0f};
};

// Nested struct defaults
// struct Entity { transform::Transform = {position = {x = 0, ...}, scale = {x = 1, ...}} }
struct Entity {
    Transform transform = {
        .position = {.x = 0.0f, .y = 0.0f, .z = 0.0f},
        .scale = {.x = 1.0f, .y = 1.0f, .z = 1.0f}
    };
};
```

**Enum defaults**:

```cpp
// enum ConnectionState : u8 { Disconnected = 0 default, ... }
enum class ConnectionState : uint8_t {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Error = 3
};

inline constexpr ConnectionState ConnectionState_default = ConnectionState::Disconnected;

// Zero-initialization produces valid default
ConnectionState state{};  // == Disconnected
```

### Primitive Types

| Type | Size | Align | C++ | Rust | C |
|------|------|-------|-----|------|---|
| `bool` | 1 | 1 | `bool` | `bool` | `_Bool` |
| `i8` | 1 | 1 | `int8_t` | `i8` | `int8_t` |
| `i16` | 2 | 2 | `int16_t` | `i16` | `int16_t` |
| `i32` | 4 | 4 | `int32_t` | `i32` | `int32_t` |
| `i64` | 8 | 8 | `int64_t` | `i64` | `int64_t` |
| `i128` | 16 | 16 | `__int128` / `std::array<uint64_t, 2>` | `i128` | `__int128` / struct |
| `u8` | 1 | 1 | `uint8_t` | `u8` | `uint8_t` |
| `u16` | 2 | 2 | `uint16_t` | `u16` | `uint16_t` |
| `u32` | 4 | 4 | `uint32_t` | `u32` | `uint32_t` |
| `u64` | 8 | 8 | `uint64_t` | `u64` | `uint64_t` |
| `u128` | 16 | 16 | `unsigned __int128` / `std::array<uint64_t, 2>` | `u128` | `unsigned __int128` / struct |
| `f16` | 2 | 2 | `_Float16` / `uint16_t` | `f16` (half crate) | `uint16_t` |
| `f32` | 4 | 4 | `float` | `f32` | `float` |
| `f64` | 8 | 8 | `double` | `f64` | `double` |
| `bf16` | 2 | 2 | `std::bfloat16_t` / `uint16_t` | `bf16` (half crate) | `uint16_t` |

#### Bool Representation

**Reading** (permissive, matches C/C++):
- `0x00` = false
- `0x01`-`0xFF` (any non-zero) = true

**Writing** (canonical):
- false MUST be written as `0x00`
- true MUST be written as `0x01`

This matches C/C++ behavior where any non-zero value converts to true, but storing true produces 1. Round-tripping data may normalize non-canonical values (e.g., `0x42` read as true, written back as `0x01`).

**Cross-language note**: Rust requires `bool` to be exactly 0 or 1. Rust implementations should read the byte as `u8` and compare against zero (`value != 0`) to safely produce a valid `bool`.

#### 128-bit Integers

`i128` and `u128` provide 128-bit integer storage:
- Size: 16 bytes
- Alignment: 16 bytes
- Wire format: Little-endian (low 64 bits first, then high 64 bits)

**Use cases**:
- UUIDs (128-bit identifiers)
- Large counters and timestamps
- Cryptographic values
- Database keys (some systems use 128-bit IDs)

**Language support**:

| Language | Native Type | Fallback |
|----------|-------------|----------|
| **Rust** | `i128`, `u128` | — (native support) |
| **C++ (GCC/Clang)** | `__int128`, `unsigned __int128` | `std::array<uint64_t, 2>` |
| **C++ (MSVC)** | Not supported | `std::array<uint64_t, 2>` or struct |
| **C (GCC/Clang)** | `__int128`, `unsigned __int128` | Struct of two `uint64_t` |

**Fallback struct** (for compilers without native 128-bit support):
```cpp
struct alignas(16) int128_t {
    uint64_t lo;  // Low 64 bits (at lower address)
    uint64_t hi;  // High 64 bits (at higher address)
};
static_assert(sizeof(int128_t) == 16);
static_assert(alignof(int128_t) == 16);
```

**Why 16-byte alignment?** Native `__int128` types have 16-byte alignment on most platforms. ZMEM requires 16-byte alignment for `i128`/`u128` to ensure:
- Consistent struct layouts across native and fallback implementations
- Optimal performance for SIMD operations
- Compatibility with memory-mapped files and DMA transfers

Without `alignas(16)`, a struct of two `uint64_t` would only have 8-byte alignment, causing layout mismatches.

#### Floating Point

IEEE 754 binary representation:
- `f16`: IEEE 754 binary16 (half precision)
- `f32`: IEEE 754 binary32 (single precision)
- `f64`: IEEE 754 binary64 (double precision)
- `bf16`: bfloat16 (Brain Floating Point)

##### Half-Precision Types

| Type | Format | Sign | Exponent | Mantissa | Range | Use Case |
|------|--------|------|----------|----------|-------|----------|
| `f16` | IEEE 754 binary16 | 1 bit | 5 bits | 10 bits | ±65504 | Graphics, ML inference |
| `bf16` | bfloat16 | 1 bit | 8 bits | 7 bits | ±3.4e38 | ML training |

**f16** (IEEE 754 half-precision):
- Compact storage with reasonable precision (~3.3 decimal digits)
- Supported by GPUs, ARM (ARMv8.2-A+), x86 (F16C extension)
- Common in graphics shaders and ML inference

**bf16** (bfloat16):
- Same exponent range as f32 (no overflow/underflow issues)
- Lower precision (~2.4 decimal digits) but faster training convergence
- Supported by Google TPUs, Intel (AVX-512 BF16), NVIDIA (Ampere+), AMD (RDNA 3+)
- Truncated f32: upper 16 bits of f32 representation

##### Language Support

**C++**:
- `f16`: `_Float16` (GCC 12+, Clang 15+), or store as `uint16_t` with conversion helpers
- `bf16`: `std::bfloat16_t` (C++23), `__bf16` (compiler extension), or `uint16_t`

**Rust**:
- `half` crate provides `f16` and `bf16` types
- No native language support yet

**C**:
- No native support; use `uint16_t` with conversion helpers
- Some compilers offer `_Float16` as an extension

**Fallback**: When native types are unavailable, store as `uint16_t` and provide conversion functions to/from `float`.

##### Special Values (NaN, Infinity, Zero)

All IEEE 754 special values are valid and preserved bit-for-bit:

| Value | Valid | Bit Pattern | Notes |
|-------|-------|-------------|-------|
| +Infinity | ✓ | Single pattern | Preserved exactly |
| -Infinity | ✓ | Single pattern | Preserved exactly |
| +0 | ✓ | `0x00000000` (f32) | Preserved exactly |
| -0 | ✓ | `0x80000000` (f32) | Preserved exactly (distinct from +0) |
| Denormals | ✓ | Various | Preserved exactly |
| Quiet NaN | ✓ | Multiple patterns | Preserved exactly |
| Signaling NaN | ✓ | Multiple patterns | Preserved exactly |

**Key properties**:
- Bit patterns are preserved exactly (no canonicalization)
- Zero serialization overhead (direct memcpy)
- All valid IEEE 754 bit patterns are accepted

**NaN considerations**:
- NaN has multiple valid bit representations (payload bits vary)
- Different operations may produce different NaN bit patterns
- `memcmp` equality may differ from semantic float equality when NaN is present
- Applications requiring deterministic NaN handling should canonicalize at the application level

**Negative zero**: +0 and -0 are semantically equal but have different bit patterns. ZMEM preserves the distinction; applications that require +0 == -0 for `memcmp` should normalize at the application level.

### Enum Types

Enums define a set of named integer constants with a specified underlying type:

```
enum Status : u8 {
  Pending = 0
  Active = 1
  Completed = 2
  Failed = 3
}

enum ErrorCode : i32 {
  Success = 0
  NotFound = -1
  PermissionDenied = -2
  Timeout = -3
}
```

#### Syntax

```
enum Name : underlying_type {
  Variant1 = value1
  Variant2 = value2
  ...
}
```

- **Name**: Identifier for the enum type
- **underlying_type**: Any integer primitive (`i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`)
- **Variant**: Named constant with explicit or auto-incremented value

#### Value Assignment

Values can be explicit or auto-incremented:

```
enum Color : u8 {
  Red           # 0 (auto: starts at 0)
  Green         # 1 (auto: previous + 1)
  Blue          # 2 (auto: previous + 1)
}

enum Priority : u8 {
  Low = 0       # explicit
  Medium = 5    # explicit (gap allowed)
  High          # 6 (auto: previous + 1)
  Critical = 10 # explicit
}
```

**Rules**:
- If no value is specified, auto-increment from 0 or previous value + 1
- Explicit values override auto-increment
- Values must fit in the underlying type
- Duplicate variant names are not allowed
- Duplicate values are not allowed (unlike C/C++)
- Gaps between values are allowed

#### Wire Format

Enums are stored as their underlying integer type:

| Enum Type | Wire Format | Size | Alignment |
|-----------|-------------|------|-----------|
| `enum E : u8` | `u8` | 1 | 1 |
| `enum E : u16` | `u16` | 2 | 2 |
| `enum E : u32` | `u32` | 4 | 4 |
| `enum E : u64` | `u64` | 8 | 8 |
| `enum E : i8` | `i8` | 1 | 1 |
| `enum E : i16` | `i16` | 2 | 2 |
| `enum E : i32` | `i32` | 4 | 4 |
| `enum E : i64` | `i64` | 8 | 8 |

**Zero overhead**: Enums have identical wire representation to their underlying type.

#### Usage in Types

Enums can be used anywhere an integer type is valid:

```
struct Task {
  id::u64
  status::Status          # enum field
  priorities::Priority[4] # fixed array of enums
  error::opt<ErrorCode>   # optional enum
}

# Enums as map keys (since they're integers)
struct Config {
  settings::map<Priority, str[64]>
}
```

#### Validation

**Invalid enum values are undefined behavior.** If the wire contains a value that doesn't match any variant (e.g., `5` for a `Status` enum with variants 0-3), behavior is implementation-defined.

Implementations MAY:
- Provide optional validation that checks values at deserialization
- Treat invalid values as undefined (maximum performance)
- Map invalid values to a default/error variant

The specification does not mandate validation to preserve zero-overhead deserialization.

#### C++ Mapping

```cpp
// Generated from: enum Status : u8 { ... }
enum class Status : uint8_t {
    Pending = 0,
    Active = 1,
    Completed = 2,
    Failed = 3
};

static_assert(sizeof(Status) == 1);
static_assert(alignof(Status) == 1);
static_assert(std::is_trivially_copyable_v<Status>);

// Usage in struct
struct Task {
    uint64_t id;
    Status status;
};

static_assert(sizeof(Task) == 16);  // 8 + 1 + 7 padding
static_assert(offsetof(Task, status) == 8);
```

### Union Types (Tagged Unions)

Unions define a tagged (discriminated) sum type where exactly one variant is active at a time:

```
union Result : u32 {
  Ok = 0 { value::u64 }
  Err = 1 { code::i32, msg::str[256] }
}

union Message : u32 {
  Ping = 0
  Pong = 1 { timestamp::u64 }
  Data = 2 { id::u64, payload::[u8] }
}
```

#### Syntax

```
union Name : tag_type {
  Variant1 = value1
  Variant2 = value2 { field1::type1, field2::type2 }
  Variant3 = value3 :: ExistingType
  ...
}
```

- **Name**: Identifier for the union type
- **tag_type**: Integer primitive for the discriminant (`u8`, `u16`, `u32`, `u64`). Default: `u32` if omitted
- **Variant**: Named alternative with optional fields

#### Variant Forms

| Form | Syntax | Description |
|------|--------|-------------|
| Unit | `Name = N` | No associated data |
| Struct | `Name = N { field::type, ... }` | Inline field definitions |
| Type reference | `Name = N :: Type` | Reference existing struct |

**Examples**:
```
union Option : u8 {
  None = 0                           # unit variant
  Some = 1 { value::u64 }            # struct variant
}

union Shape : u32 {
  Circle = 0 { center::Vec2, radius::f32 }
  Rect = 1 { min::Vec2, max::Vec2 }
  Point = 2 :: Vec2                  # type reference
}
```

#### Value Assignment

Like enums, variant tag values can be explicit or auto-incremented:

```
union Event : u16 {
  Start             # 0 (auto)
  Stop              # 1 (auto)
  Pause = 10        # 10 (explicit)
  Resume            # 11 (auto: previous + 1)
}
```

**Rules**:
- If no value specified, auto-increment from 0 or previous value + 1
- Explicit values override auto-increment
- Values must fit in the tag type
- Duplicate variant names are not allowed
- Duplicate tag values are not allowed
- Gaps between values are allowed
- At least one variant is required (empty unions are invalid)

#### Union Categories

Like structs, unions are categorized by whether any variant contains vectors:

| Category | Description | Wire Format |
|----------|-------------|-------------|
| **Fixed Union** | All variants are trivially copyable | Fixed-size inline layout |
| **Variable Union** | Any variant contains vectors | Size header + inline + variable section |

#### Fixed Union Layout

For unions where all variants are trivially copyable:

```
┌──────────┬──────────┬────────────────────────────────┐
│   tag    │ padding  │   variant data (max size)      │
│ tag_size │ to align │   max(sizeof(variants))        │
└──────────┴──────────┴────────────────────────────────┘
```

**Size**: `alignof(max_variant) + max(sizeof(variants))`
**Alignment**: `max(alignof(tag), max(alignof(variants)))`

**Example**:
```
union Result : u32 {
  Ok = 0 { value::u64 }
  Err = 1 { code::i32, msg::str[64] }
}
```

| Variant | Fields | Size | Alignment |
|---------|--------|------|-----------|
| `Ok` | `value::u64` | 8 | 8 |
| `Err` | `code::i32, msg::str[64]` | 68 | 4 |

- Max variant size: 68 bytes
- Max variant alignment: 8 bytes
- Tag: 4 bytes (u32)
- Padding after tag: 4 bytes (to align to 8)

```
┌──────────┬──────────┬────────────────────────────────┐
│   tag    │ padding  │   variant data                 │
│ 4 bytes  │ 4 bytes  │   68 bytes                     │
└──────────┴──────────┴────────────────────────────────┘
Total: 76 bytes, Alignment: 8
```

Wire layout for `Result::Ok { value: 42 }`:
```
Offset  Size  Field
------  ----  -----
0       4     tag = 0 (Ok)
4       4     [padding]
8       8     value = 42
16      60    [unused - padding to max variant size]
------
Total: 76 bytes
```

Wire layout for `Result::Err { code: -1, msg: "error" }`:
```
Offset  Size  Field
------  ----  -----
0       4     tag = 1 (Err)
4       4     [padding]
8       4     code = -1
12      64    msg = "error\0..."
------
Total: 76 bytes
```

#### Compact Fixed Union Example

```
union Option : u8 {
  None = 0
  Some = 1 { value::u32 }
}
```

| Variant | Size | Alignment |
|---------|------|-----------|
| `None` | 0 | 1 |
| `Some` | 4 | 4 |

```
┌──────────┬──────────┬────────────────────┐
│   tag    │ padding  │   variant data     │
│ 1 byte   │ 3 bytes  │   4 bytes          │
└──────────┴──────────┴────────────────────┘
Total: 8 bytes, Alignment: 4
```

#### Variable Union Layout

For unions where any variant contains vectors:

```
┌──────────┬──────────┬──────────┬─────────────────────┬──────────────┐
│   size   │   tag    │ padding  │  inline section     │  var section │
│ 8 bytes  │ tag_size │ to align │  max inline size    │   varies     │
└──────────┴──────────┴──────────┴─────────────────────┴──────────────┘
```

**Example**:
```
union Payload : u32 {
  Empty = 0
  Text = 1 { content::str[256] }
  Binary = 2 { data::[u8] }           # vector makes this variable
}
```

| Variant | Inline Size | Contains Vector |
|---------|-------------|-----------------|
| `Empty` | 0 | No |
| `Text` | 256 | No |
| `Binary` | 16 (offset + count) | Yes |

Max inline size: 256 bytes

Wire layout for `Payload::Empty`:
```
Offset  Size  Field
------  ----  -----
0       8     size = 264 (payload size after this field)
8       4     tag = 0 (Empty)
12      4     [padding to align inline section]
16      256   [unused inline section, padded to max]
------
Total: 272 bytes (8 + 264)
```

Wire layout for `Payload::Binary { data: [1,2,3,4] }`:
```
Offset  Size  Field
------  ----  -----
0       8     size = 264 (inline payload size after this field)
8       4     tag = 2 (Binary)
12      4     [padding to align inline section]
16      8     data.offset = 264 (relative to byte 8, points to variable section)
24      8     data.count = 4
32      240   [padding to max inline size]
--- Variable section (offset 8 + 264 = 272 from start) ---
272     4     data bytes: [1, 2, 3, 4]
------
Total: 276 bytes (8 + 264 + 4)
```

#### Usage in Types

Unions can be used as field types in structs:

```
struct Response {
  id::u64
  result::Result
  timestamp::u64
}

struct EventLog {
  events::[Event]             # vector of union type
}
```

Unions can be nested:
```
union Outer : u32 {
  A = 0 { inner::Inner }      # Inner is another union
  B = 1 { value::u64 }
}
```

#### Validation

**Invalid tag values are undefined behavior.** If the wire contains a tag value that doesn't match any variant, behavior is implementation-defined.

The specification does not mandate validation to preserve zero-overhead deserialization.

#### C++ Mapping

```cpp
// Generated from: union Result : u32 { Ok = 0 { value::u64 }, Err = 1 { code::i32, msg::str[64] } }

struct Result {
    enum class Tag : uint32_t {
        Ok = 0,
        Err = 1
    };

    Tag tag;

private:
    // Padding handled by alignment
    union {
        struct { uint64_t value; } ok;
        struct { int32_t code; char msg[64]; } err;
    } data_;

public:
    // Tag check
    bool is_ok() const { return tag == Tag::Ok; }
    bool is_err() const { return tag == Tag::Err; }

    // Safe accessors (assert on wrong variant)
    uint64_t& as_ok() {
        assert(tag == Tag::Ok);
        return data_.ok.value;
    }
    const uint64_t& as_ok() const {
        assert(tag == Tag::Ok);
        return data_.ok.value;
    }

    struct ErrData { int32_t code; char msg[64]; };
    ErrData& as_err() {
        assert(tag == Tag::Err);
        return data_.err;
    }
    const ErrData& as_err() const {
        assert(tag == Tag::Err);
        return data_.err;
    }

    // Factory functions
    static Result make_ok(uint64_t value) {
        Result r;
        r.tag = Tag::Ok;
        r.data_.ok.value = value;
        return r;
    }

    static Result make_err(int32_t code, const char* msg) {
        Result r;
        r.tag = Tag::Err;
        r.data_.err.code = code;
        std::strncpy(r.data_.err.msg, msg, 63);
        r.data_.err.msg[63] = '\0';
        return r;
    }
};

static_assert(sizeof(Result) == 76);
static_assert(alignof(Result) == 8);
static_assert(std::is_trivially_copyable_v<Result>);
static_assert(offsetof(Result, tag) == 0);
```

### Strings

All string types in ZMEM (`str[N]` and `string`) use **UTF-8 encoding**.

**UTF-8 Validation**: Implementation-defined. Implementations may be:
- **Permissive** (recommended): Accept any byte sequence, interpret as UTF-8
- **Strict**: Reject invalid UTF-8 sequences on read/write

The permissive approach aligns with ZMEM's zero-copy philosophy—validation adds overhead and most data sources already produce valid UTF-8. Strict validation is appropriate for untrusted input or when UTF-8 correctness is critical.

**Raw binary data**: For arbitrary byte sequences that aren't text, use `[u8]` instead of strings. The `[u8]` type makes no encoding assumptions and is appropriate for binary blobs, encrypted data, or legacy encodings.

#### Fixed-Size Strings (`str[N]`)

ZMEM supports fixed-size strings via the `str[N]` type:

```
str[N]    # N-byte buffer, null-terminated, zero-padded
```

**Constraints**:
- `N` must be ≥ 1
- Maximum string length is N-1 bytes (last byte reserved for null terminator)
- Content must be null-terminated (`\0`)
- All bytes after the null terminator must be zero (zero-padded)
- Alignment: 1 byte

**Layout**: Exactly N bytes, same as `u8[N]` but with null-termination semantics.

| Type | Size | Max Content | C++ | Rust | C |
|------|------|-------------|-----|------|---|
| `str[32]` | 32 | 31 bytes | `char[32]` | `[u8; 32]` | `char[32]` |
| `str[256]` | 256 | 255 bytes | `char[256]` | `[u8; 256]` | `char[256]` |
| `str[4096]` | 4096 | 4095 bytes | `char[4096]` | `[u8; 4096]` | `char[4096]` |

**UTF-8 and capacity**: Since UTF-8 uses 1-4 bytes per Unicode code point, `str[32]` can hold 31 ASCII characters but fewer non-ASCII characters (e.g., 10 emoji at 4 bytes each, or 15 CJK characters at 3 bytes each). Size buffers for worst-case byte length, not code point count.

**Note**: Large strings directly impact struct size. A `str[4096]` field adds 4KB to every struct instance. Choose sizes appropriate for your use case; use constants for clarity:

```
const PATH_MAX::u32 = 4096
const URL_MAX::u32 = 8192

struct FileInfo {
  path::str[PATH_MAX]
  url::str[URL_MAX]
}
```

**Zero-padding requirement**: Ensures `memcmp` produces the same ordering as `strcmp`, enabling efficient binary search on sorted string keys.

**Byte-level layout examples** for `str[8]`:

```
Short string "hi" (2 chars):
┌───┬───┬───┬───┬───┬───┬───┬───┐
│ h │ i │\0 │\0 │\0 │\0 │\0 │\0 │
└───┴───┴───┴───┴───┴───┴───┴───┘
  0   1   2   3   4   5   6   7
          ↑   └───────┬───────┘
         null      zero padding

Max-length string "hello12" (7 chars):
┌───┬───┬───┬───┬───┬───┬───┬───┐
│ h │ e │ l │ l │ o │ 1 │ 2 │\0 │
└───┴───┴───┴───┴───┴───┴───┴───┘
  0   1   2   3   4   5   6   7
                              ↑
                     null terminator in final byte
                     (no padding needed)

Empty string "" (0 chars):
┌───┬───┬───┬───┬───┬───┬───┬───┐
│\0 │\0 │\0 │\0 │\0 │\0 │\0 │\0 │
└───┴───┴───┴───┴───┴───┴───┴───┘
  0   1   2   3   4   5   6   7
  ↑   └───────────┬───────────┘
 null          zero padding
```

**Rule**: All bytes after the first `\0` must be zero. For max-length strings, the null terminator occupies the final byte, so no padding exists.

#### Variable-Length Strings (`string`)

ZMEM supports variable-length strings via the `string` type:

```
string    # variable-length string, no size limit
```

Unlike `str[N]`, variable-length strings have no compile-time size limit and are stored in the variable section of variable structs.

**Inline representation** (in struct):
```
┌────────────────┬────────────────┐
│     offset     │     length     │
│    8 bytes     │    8 bytes     │
└────────────────┴────────────────┘
```

```cpp
struct alignas(8) zmem_string_ref {
    uint64_t offset;  // Byte offset to string data (relative to byte 8 of containing struct)
    uint64_t length;  // Byte length of string (NOT including any null terminator)
};
```

**Variable section**: Raw UTF-8 bytes, NOT null-terminated (length is explicit).

**Size**: 16 bytes inline + variable data
**Alignment**: 8 bytes (platform-independent, required for 32-bit/64-bit compatibility)

| Type | Inline Size | Alignment | Data Location |
|------|-------------|-----------|---------------|
| `str[64]` | 64 | 1 | Inline (fixed) |
| `string` | 16 | 8 | Variable section |

**When to use each**:

| Use Case | Recommended Type |
|----------|------------------|
| Known max length, hot path | `str[N]` (zero-copy) |
| Unknown length, rare access | `string` (flexible) |
| Interop with C strings | `str[N]` (null-terminated) |
| Large text, documents | `string` (no wasted space) |

**Constraints**:
- `string` fields make the containing struct **variable**
- `opt<string>` is supported
- `[string]` (vector of strings) is supported
- `string` cannot be used as a map key (use `str[N]` for keys)

**Example**:

```
struct LogEntry {
  timestamp::u64
  level::u8
  message::string       # variable-length
  source::str[64]       # fixed-length (for indexing)
}
```

Wire layout for `LogEntry { timestamp: 1000, level: 2, message: "Hello, World!", source: "main.cpp" }`:

```
Offset  Size  Field
------  ----  -----
0       8     size = 93 (payload after this field)
8       8     timestamp = 1000
16      1     level = 2
17      7     [padding to align message]
24      8     message.offset = 72 (relative to byte 8)
32      8     message.length = 13
40      64    source = "main.cpp\0..." (null-terminated, zero-padded)
--- Variable section (offset 8 + 72 = 80 from start) ---
104     13    message data: "Hello, World!"
------
Total: 117 bytes
```

### Optional Types

ZMEM supports optional values via the `opt<T>` type:

```
opt<T>    # Optional value of type T
```

**Layout**:
```
┌─────────┬─────────────┬─────────────────┐
│ present │   padding   │     value       │
│ 1 byte  │ align-1     │   sizeof(T)     │
└─────────┴─────────────┴─────────────────┘
```

- `present`: 1 byte — `0x00` = absent, `0x01` = present (other values invalid)
- `padding`: bytes to align value to `alignof(T)` (must be zero)
- `value`: `sizeof(T)` bytes — valid if present, **must be zero if absent**

**Zero-initialization requirement**: When `present == 0`, the padding and value bytes MUST be zero. This ensures:
- Deterministic serialization (same logical value → same bytes)
- No information leakage (no stale data in unused bytes)
- Byte-by-byte comparison works (`memcmp` of two absent optionals is equal)

```
opt<u32> absent example:
┌─────────┬───────────────┬───────────────────────────┐
│ present │    padding    │          value            │
│  0x00   │ 0x00 0x00 0x00│ 0x00 0x00 0x00 0x00       │
└─────────┴───────────────┴───────────────────────────┘
     0          1-3                  4-7

opt<u32> present example (value = 42):
┌─────────┬───────────────┬───────────────────────────┐
│ present │    padding    │          value            │
│  0x01   │ 0x00 0x00 0x00│ 0x2A 0x00 0x00 0x00       │
└─────────┴───────────────┴───────────────────────────┘
     0          1-3                  4-7
```

**Size**: `alignof(T) + sizeof(T)`
**Alignment**: `alignof(T)`

| Type | Size | Alignment | Layout |
|------|------|-----------|--------|
| `opt<u8>` | 2 | 1 | `[present:1][value:1]` |
| `opt<u16>` | 4 | 2 | `[present:1][pad:1][value:2]` |
| `opt<u32>` | 8 | 4 | `[present:1][pad:3][value:4]` |
| `opt<u64>` | 16 | 8 | `[present:1][pad:7][value:8]` |
| `opt<f32>` | 8 | 4 | `[present:1][pad:3][value:4]` |
| `opt<f64>` | 16 | 8 | `[present:1][pad:7][value:8]` |
| `opt<str[32]>` | 33 | 1 | `[present:1][value:32]` |

**Why not use std::optional directly?**

`std::optional<T>` has implementation-defined layout that varies across compilers. ZMEM defines an explicit layout for cross-platform wire compatibility. Generated code uses `zmem_optional<T>` with guaranteed layout.

**Constraints**:
- `T` must be a fixed-size type (primitive, str[N], fixed array, or fixed struct)
- `opt<[T]>` (optional vector) is not supported — use empty vector instead
- `opt<opt<T>>` (nested optionals) is not supported

### Fixed Arrays

Fixed-size arrays are supported for **fixed-size element types**:

```
T[N]
```

Where:
- `T` is a fixed-size type (see below)
- `N` is a positive integer constant

#### Fixed-Size Element Types

Fixed arrays require elements with compile-time known size. Valid element types:

| Element Type | Valid | Reason |
|--------------|-------|--------|
| Primitives (`f32`, `i64`, etc.) | ✓ | Known size |
| `str[N]` | ✓ | Fixed N bytes |
| `opt<T>` where T is fixed-size | ✓ | Fixed inline size |
| `T[M]` (nested fixed array) | ✓ | Fixed M × sizeof(T) |
| Fixed structs (no vectors/strings) | ✓ | Fixed field layout |
| Enums | ✓ | Fixed tag size |
| Fixed unions (no vector variants) | ✓ | Fixed max inline size |
| `[T]` (vector) | ✗ | Variable size |
| `string` | ✗ | Variable size |
| Variable structs (with vectors) | ✗ | Variable size |
| `opt<[T]>`, `opt<string>` | ✗ | Contains variable type |

**Syntax note**: `[T][N]` is a **syntax error** (see Grammar Disambiguation). To express "vector of fixed arrays", use `[T[N]]`.

**Layout rules**:
- Elements are contiguous (no padding between elements)
- Array alignment equals element alignment
- Total size = `sizeof(T) * N`

#### Multi-dimensional Arrays

Multi-dimensional arrays are expressed by chaining dimensions:

```
T[A][B]       # 2D array: A × B elements
T[A][B][C]    # 3D array: A × B × C elements
```

**Examples**:
```
struct Matrix4x4 {
  data::f32[4][4]           # 4×4 matrix, 64 bytes
}

struct Image {
  pixels::u8[1080][1920][4] # height × width × RGBA, ~7.9 MB
}

struct Tensor {
  values::f32[8][16][32]    # 3D tensor, 16 KB
}
```

#### Memory Order: Row-Major (C Order)

ZMEM uses **row-major order** (same as C, C++, Rust):

- The **rightmost index varies fastest** in memory
- Elements with the same leading indices are contiguous

For `T[A][B][C]`:
- Element `[i][j][k]` is at byte offset: `(i × B × C + j × C + k) × sizeof(T)`
- Index `k` varies fastest, then `j`, then `i`

**Example**: `f32[2][3]` (2 rows, 3 columns)

```
Logical view:          Memory layout:
┌─────┬─────┬─────┐
│[0,0]│[0,1]│[0,2]│    [0,0] [0,1] [0,2] [1,0] [1,1] [1,2]
├─────┼─────┼─────┤    ←─── row 0 ────→ ←─── row 1 ────→
│[1,0]│[1,1]│[1,2]│
└─────┴─────┴─────┘    Offset:  0     4     8    12    16    20
```

**Example**: `u8[2][3][4]` (2 planes, 3 rows, 4 columns)

```
Element [i][j][k] at offset: i×12 + j×4 + k

Plane 0 (i=0):           Plane 1 (i=1):
[0,0,0..3][0,1,0..3]...  [1,0,0..3][1,1,0..3]...
   ↑ k varies fastest
```

#### Why Row-Major?

| Language | Memory Order |
|----------|--------------|
| C / C++ | Row-major |
| Rust | Row-major |
| Python (NumPy default) | Row-major |
| Fortran | Column-major |
| MATLAB | Column-major |
| Julia | Column-major |

Since ZMEM targets C++, Rust, and C, row-major is the natural choice for zero-copy interop.

#### C++ Mapping

```cpp
// f32[4][4] maps to:
float data[4][4];
// or
std::array<std::array<float, 4>, 4> data;

// Access [i][j]:
float value = data[i][j];

// Layout verification
static_assert(sizeof(float[4][4]) == 64);
static_assert(&data[0][3] + 1 == &data[1][0]);  // rows are contiguous
```

#### Type Signature

Multi-dimensional arrays appear with chained brackets:

```
Matrix4x4{data::f32[4][4]}
Image{pixels::u8[1080][1920][4]}
```

### Nested Structs

Structs may contain other ZMEM-conforming structs as fields. Nested structs:
- Must independently satisfy all ZMEM requirements
- Are aligned according to their own alignment requirement
- Contribute their full type signature to the parent's signature (recursively expanded)

### Embedded Vectors

Structs may contain vector fields (`std::vector<T>` in C++, `Vec<T>` in Rust). This creates two categories of structs:

| Category | Description | Wire Format |
|----------|-------------|-------------|
| **Fixed Struct** | No vector fields (trivially copyable) | Direct memcpy, 8-byte header |
| **Variable Struct** | Has vector field(s) | Offset-based, inline + variable sections |

#### Vector Field Wire Representation

Each vector field is stored in the inline section as a **vector reference**:

```cpp
struct alignas(8) zmem_vector_ref {
    uint64_t offset;  // Byte offset to array data (see reference point below)
    uint64_t count;   // Number of elements
};
```

- Size: 16 bytes
- Alignment: 8 bytes (platform-independent, required for 32-bit/64-bit compatibility)

The actual array data is stored in the **variable section** after the inline section.

**Offset Reference Point**: The `offset` is always relative to **byte 8 of the containing struct** (the first byte after the 8-byte size field). This rule applies at all nesting levels:
- Top-level struct: offset is relative to byte 8 of the root serialization
- Nested element in `[VariableStruct]`: offset is relative to byte 8 of **that element**, not the parent

This makes each variable struct self-contained and independently deserializable.

#### Element Type Categories

Vector elements are classified into three categories:

| Category | Element Types | Wire Format |
|----------|---------------|-------------|
| **Fixed** | Primitives, `str[N]`, fixed arrays, fixed structs | Contiguous data |
| **Variable** | Structs with vector fields | Offset table + self-contained elements |
| **Variable** | `[T]` (nested vectors), `string` | Offset table + variable-size data |

#### Vectors of Fixed Elements

For `[T]` where T is fixed (trivially copyable), elements are stored **contiguously**:

```
Variable section:
┌──────────────────────────────────────────────┐
│ element[0] │ element[1] │ ... │ element[n-1] │
│ sizeof(T)  │ sizeof(T)  │     │ sizeof(T)    │
└──────────────────────────────────────────────┘
```

Random access: `element_i = base + i * sizeof(T)`

#### Vectors of Variable Elements

For `[T]` where T is a variable struct (has vector fields), each element is **self-contained** with its own size header, and an **offset table** enables random access:

```
Variable section:
┌─────────────────────────────────────────────────────────────┐
│ Offset table: (count+1) × 8 bytes                           │
│   offsets[0] = 0 (element 0 start)                          │
│   offsets[1] = size of element 0                            │
│   ...                                                       │
│   offsets[count] = total data size (sentinel)               │
├─────────────────────────────────────────────────────────────┤
│ Element 0: [size:8][inline][variable]                       │
├─────────────────────────────────────────────────────────────┤
│ Element 1: [size:8][inline][variable]                       │
├─────────────────────────────────────────────────────────────┤
│ ...                                                         │
└─────────────────────────────────────────────────────────────┘
```

**Element access**:
- Data section starts at: `base + (count+1) * 8`
- Element i starts at: `data_start + offsets[i]`
- Element i size: `offsets[i+1] - offsets[i]`

**Example**: `[Entity]` where Entity has vector fields

```
struct Entity {
  id::u64
  weights::[f32]
}
```

For `[{id: 1, weights: [0.5]}, {id: 2, weights: [0.1, 0.2]}]`:

```
Offset  Size  Field
------  ----  -----
0       8     offsets[0] = 0
8       8     offsets[1] = 36
16      8     offsets[2] = 76  (total data size)
--- Element 0 starts at offset 24 ---
24      8     elem[0].size = 28
32      8     elem[0].id = 1                     ← byte 8 of element 0
40      8     elem[0].weights.offset = 24       ← relative to byte 8 of THIS element
48      8     elem[0].weights.count = 1
56      4     elem[0].weights[0] = 0.5          ← offset 24 from byte 32 = offset 56
--- Element 1 starts at offset 60 ---
60      8     elem[1].size = 32
68      8     elem[1].id = 2                     ← byte 8 of element 1
76      8     elem[1].weights.offset = 24       ← relative to byte 8 of THIS element
84      8     elem[1].weights.count = 2
92      4     elem[1].weights[0] = 0.1          ← offset 24 from byte 68 = offset 92
96      4     elem[1].weights[1] = 0.2
------
Total: 100 bytes
```

**Offset Reference Point (Critical)**: Each variable element is **self-contained**. Vector offsets within an element are relative to **that element's own byte 8** (the start of its inline section), NOT the parent's byte 8. This enables:
- Independent deserialization of each element
- Copy/move of elements without offset fixup
- Consistent offset calculation at any nesting depth

#### Nested Vectors

For `[[T]]` (vector of vectors), the same offset table format applies:

```
Variable section:
┌─────────────────────────────────────────────────────────────┐
│ Offset table: (count+1) × 8 bytes                           │
├─────────────────────────────────────────────────────────────┤
│ Inner vector 0: [count:8][data...]                          │
├─────────────────────────────────────────────────────────────┤
│ Inner vector 1: [count:8][data...]                          │
├─────────────────────────────────────────────────────────────┤
│ ...                                                         │
└─────────────────────────────────────────────────────────────┘
```

Each inner vector is serialized as `[count:8][element_data...]` where elements follow the rules for that element type.

**Example**: `[[i32]]` with `[[1, 2], [3, 4, 5], []]`

```
Offset  Size  Field
------  ----  -----
0       8     offsets[0] = 0
8       8     offsets[1] = 16
16      8     offsets[2] = 36
24      8     offsets[3] = 44  (total data size)
--- Data section starts at offset 32 ---
32      8     inner[0].count = 2
40      4     inner[0].data[0] = 1
44      4     inner[0].data[1] = 2
48      8     inner[1].count = 3
56      4     inner[1].data[0] = 3
60      4     inner[1].data[1] = 4
64      4     inner[1].data[2] = 5
68      8     inner[2].count = 0  (empty vector)
------
Total: 76 bytes
```

#### Vectors of Strings

For `[string]` (vector of variable-length strings), strings are stored as raw bytes:

```
Variable section:
┌─────────────────────────────────────────────────────────────┐
│ Offset table: (count+1) × 8 bytes                           │
├─────────────────────────────────────────────────────────────┤
│ String 0 bytes (length = offsets[1] - offsets[0])           │
├─────────────────────────────────────────────────────────────┤
│ String 1 bytes (length = offsets[2] - offsets[1])           │
├─────────────────────────────────────────────────────────────┤
│ ...                                                         │
└─────────────────────────────────────────────────────────────┘
```

**Example**: `[string]` with `["hello", "world!", ""]`

```
Offset  Size  Field
------  ----  -----
0       8     offsets[0] = 0
8       8     offsets[1] = 5
16      8     offsets[2] = 11
24      8     offsets[3] = 11  (total data size, empty string has 0 length)
--- Data section starts at offset 32 ---
32      5     "hello"
37      6     "world!"
------
Total: 43 bytes
```

**Zero-length element detection**: When `offsets[i] == offsets[i+1]`, element `i` has zero length. In this example, `offsets[2] == offsets[3] == 11` indicates string 2 is empty. This pattern applies to all offset table formats (strings, nested vectors, variable elements).

#### Deeply Nested Vectors

The format supports arbitrary nesting depth. For `[[[T]]]`:

```
Outer offset table → [
  Middle offset table → [
    Inner: [count][data...]
  ]
]
```

Each level uses the same offset table + data pattern recursively.

#### Summary: Vector Wire Format by Element Type

| Element Type | Offset Table? | Element Format |
|--------------|---------------|----------------|
| Fixed (`i32`, `Vec3`, etc.) | No | Raw bytes, `sizeof(T)` each |
| Variable struct | Yes | `[size:8][inline][variable]` |
| Inner vector `[T]` | Yes | `[count:8][data...]` |
| `string` | Yes | Raw bytes (length from offsets) |

### Maps

ZMEM supports flat maps with integer or string keys:

```
map<K, V>    # K = key type, V = value type
```

**Key type constraints**:
- Integer types: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`
- String types: `str[N]` (max key length: N-1 chars)

**Value type**: Any ZMEM type (primitives, structs, fixed arrays, vectors, nested maps)

#### Map Categories

Like structs, maps are categorized by whether values contain vectors:

| Category | Value Type | Entry Layout | Wire Format |
|----------|------------|--------------|-------------|
| **Fixed Map** | Trivially copyable (no vectors) | `{key, value}` | Count + entries |
| **Variable Map** | Contains vectors | `{key, offset, count, ...}` | Size + count + entries + variable data |

#### Fixed Maps

For `map<K, V>` where V is trivially copyable (primitives, fixed structs, fixed arrays):

```cpp
// Equivalent to:
std::vector<std::pair<K, V>>
```

**Entry layout**:
```
Entry = { key: K, [padding], value: V, [padding] }
```

Entry alignment = `max(alignof(K), alignof(V))`

**Examples**:

| Map Type | Entry Layout | Entry Size | Entry Align |
|----------|--------------|------------|-------------|
| `map<u64, f32>` | `[key:8][value:4][pad:4]` | 16 | 8 |
| `map<u32, u32>` | `[key:4][value:4]` | 8 | 4 |
| `map<str[32], i64>` | `[key:32][value:8]` | 40 | 8 |
| `map<u8, u8>` | `[key:1][value:1]` | 2 | 1 |

**Wire format**:
```
┌─────────┬────────────────────────────────────────────┐
│ Count   │ Entries...                                 │
│ 8 bytes │ count × sizeof(Entry)                      │
└─────────┴────────────────────────────────────────────┘
```

#### Variable Maps

For `map<K, V>` where V contains vectors (including `map<K, [T]>`):

**Entry layout** for vector values (`map<K, [T]>`):
```
Entry = { key: K, [padding], offset: u64, count: u64 }
```

The offset/count reference points to data in the variable section, just like embedded vectors in structs.

**Entry layout** for variable struct values:
```
Entry = { key: K, [padding], <inline fields with vector refs> }
```

**Wire format**:
```
┌──────────┬─────────┬─────────────────────────────┬───────────────────┐
│ Size     │ Count   │ Entries (with refs)...      │ Variable section  │
│ 8 bytes  │ 8 bytes │ fixed stride                │ vector data       │
└──────────┴─────────┴─────────────────────────────┴───────────────────┘
```

**Example**: `map<str[32], [f32]>` with 2 entries:
- "alpha" → [1.0, 2.0, 3.0]
- "beta" → [4.0, 5.0]

Entries are sorted by key: "alpha" < "beta" (lexicographic order).

```
Offset  Size  Field
------  ----  -----
0       8     Total size (after this field)
8       8     Entry count = 2
--- Entry 0 (key: "alpha") ---
16      32    Entry 0 key: "alpha\0..." (zero-padded)
48      8     Entry 0 value offset: 0
56      8     Entry 0 value count: 3
--- Entry 1 (key: "beta", sorted after "alpha") ---
64      32    Entry 1 key: "beta\0..." (zero-padded)
96      8     Entry 1 value offset: 12
104     8     Entry 1 value count: 2
--- Variable section (offset 112) ---
112     4     1.0f  (entry 0, element 0)
116     4     2.0f  (entry 0, element 1)
120     4     3.0f  (entry 0, element 2)
124     4     4.0f  (entry 1, element 0)
128     4     5.0f  (entry 1, element 1)
------
Total: 132 bytes
```

**Map Offset Convention**: For maps with vector values (`map<K, [T]>`), the `offset` field in each entry is relative to the **start of the variable section**, NOT byte 8. This differs from struct fields where offsets are relative to byte 8.

Calculating the variable section start: `16 + (count × entry_stride)`

#### Entry Ordering (Mandatory)

**Map entries MUST be sorted by key in ascending order.** This ensures:
- Deterministic serialization (same logical map → same bytes)
- `memcmp` equality comparison works
- Hashing of serialized data is consistent
- Binary search (O(log n) lookup) is always possible

##### Sorting Rules

**Integer keys**: Ascending numeric order

| Key Type | Order |
|----------|-------|
| Unsigned (`u8`, `u16`, `u32`, `u64`) | 0, 1, 2, ... |
| Signed (`i8`, `i16`, `i32`, `i64`) | ..., -2, -1, 0, 1, 2, ... |

**String keys (`str[N]`)**: Lexicographic byte order (equivalent to `memcmp`)

The zero-padding requirement for strings ensures `memcmp` produces correct lexicographic ordering.

| Keys (str[8]) | Sorted Order |
|---------------|--------------|
| "apple", "banana", "cherry" | "apple", "banana", "cherry" |
| "a", "aa", "aaa", "b" | "a", "aa", "aaa", "b" |
| "", "a", "z" | "", "a", "z" |

##### Duplicate Keys

Duplicate keys are **not allowed**. A map with duplicate keys is invalid.

##### Examples

**Integer key ordering** (`map<i32, str[8]>`):
```
Entries must be ordered: -10, -1, 0, 5, 100
NOT: 0, 5, -1, 100, -10  (invalid - not sorted)
```

**String key ordering** (`map<str[32], f64>`):
```
Entries must be ordered: "alpha", "beta", "gamma"
NOT: "gamma", "alpha", "beta"  (invalid - not sorted)
```

##### Binary Search

Sorted order enables O(log n) lookup via binary search:

```cpp
// Binary search on map<str[32], T>
template<typename T>
const T* find_in_map(const Entry<T>* entries, size_t count, const char* key) {
    auto it = std::lower_bound(entries, entries + count, key,
        [](const Entry<T>& entry, const char* k) {
            return std::memcmp(entry.key, k, 32) < 0;
        });
    if (it != entries + count && std::memcmp(it->key, key, 32) == 0) {
        return &it->value;
    }
    return nullptr;
}

// Binary search on map<u64, T>
template<typename T>
const T* find_in_map(const Entry<T>* entries, size_t count, uint64_t key) {
    auto it = std::lower_bound(entries, entries + count, key,
        [](const Entry<T>& entry, uint64_t k) {
            return entry.key < k;
        });
    if (it != entries + count && it->key == key) {
        return &it->value;
    }
    return nullptr;
}
```

##### Validation

Implementations SHOULD validate that map entries are sorted when deserializing from untrusted sources. Unsorted entries indicate malformed data.

##### Rationale

Mandatory sorted order is consistent with ZMEM's determinism philosophy:
- Strings require zero-padding for deterministic `memcmp` comparison
- Optionals require zero-initialization when absent
- Maps require sorted entries for deterministic serialization

For use cases where ordering is irrelevant and sorting overhead is unacceptable, use a vector of key-value structs instead:
```
struct Entry { key::str[32], value::f64 }
entries::[Entry]    # no ordering constraint, O(n) lookup
```

#### C++ Type Mapping

**Fixed maps**:
```cpp
// map<u64, f32> →
struct Entry_u64_f32 {
    uint64_t key;
    float value;

    // Comparison for sorting
    bool operator<(const Entry_u64_f32& other) const {
        return key < other.key;
    }
};
using Map_u64_f32 = std::vector<Entry_u64_f32>;

// map<str[32], Config> (Config is fixed) →
struct Entry_str32_Config {
    char key[32];      // null-terminated, zero-padded
    Config value;

    // Comparison for sorting (lexicographic byte order)
    bool operator<(const Entry_str32_Config& other) const {
        return std::memcmp(key, other.key, 32) < 0;
    }
};
using Map_str32_Config = std::vector<Entry_str32_Config>;
```

**Serialization must sort entries**:
```cpp
// Before serializing, ensure entries are sorted
void serialize_map(Map_u64_f32& map, std::vector<uint8_t>& buffer) {
    // Sort by key (mandatory for ZMEM compliance)
    std::sort(map.begin(), map.end());

    // ... serialize sorted entries ...
}
```

**Variable maps** (for owning deserialized data):
```cpp
// map<str[32], [f32]> →
struct Entry_str32_vecf32 {
    char key[32];
    std::vector<float> values;

    bool operator<(const Entry_str32_vecf32& other) const {
        return std::memcmp(key, other.key, 32) < 0;
    }
};
using Map_str32_vecf32 = std::vector<Entry_str32_vecf32>;
```

**Variable maps** (zero-copy view):
```cpp
// View into serialized buffer
struct EntryView_str32_vecf32 {
    std::string_view key;           // points into buffer
    std::span<const float> values;  // points into buffer
};
```

#### Type Signature

```
map<u64,f32>                    # fixed map, integer key
map<str[32],Config{x::f32}>     # fixed map, string key
map<str[32],[f32]>              # variable map, vector values
map<u64,Entity{id::u64,tags::[str[16]]}>  # variable map, variable struct values
```

---

## Memory Layout

### Byte Order

All multi-byte values use **little-endian** byte order.

Rationale: Little-endian is native to x86, x86-64, ARM (in default mode), and Apple Silicon, enabling zero-copy access without byte swapping on the vast majority of modern systems.

### Alignment Rules

ZMEM uses **natural alignment**:

1. **Primitive alignment**: Each primitive type is aligned to its size
   - `i8`, `u8`, `bool`: 1-byte alignment
   - `i16`, `u16`, `f16`, `bf16`: 2-byte alignment
   - `i32`, `u32`, `f32`: 4-byte alignment
   - `i64`, `u64`, `f64`: 8-byte alignment
   - `i128`, `u128`: 16-byte alignment

2. **Struct alignment**: Maximum alignment of any member field

3. **Padding**: Inserted as needed to satisfy alignment constraints
   - Between fields to align subsequent fields
   - At end of struct to ensure `sizeof(struct)` is a multiple of struct alignment

4. **Array alignment**: Same as element alignment (no additional padding)

5. **Reference type alignment**: Vector references (`offset` + `count`) and string references (`offset` + `length`) are **always 8-byte aligned**, regardless of platform pointer size. This ensures wire format compatibility between 32-bit and 64-bit systems.

**Unaligned access note**: ZMEM assumes buffers are properly aligned for direct memory access. On platforms without hardware unaligned access support (some embedded ARM, older MIPS, etc.), implementations should either:
- Ensure input buffers are aligned to the struct's alignment requirement
- Use byte-by-byte reads/writes for deserialization/serialization
- Use `memcpy` to a properly aligned local variable before access

Most modern platforms (x86, x86-64, ARMv7+, Apple Silicon) support unaligned access with minimal or no performance penalty.

### Layout Algorithm

Given a struct with fields `f1, f2, ..., fn`:

```
offset = 0
for each field fi with alignment ai and size si:
    if offset % ai != 0:
        padding = ai - (offset % ai)
        offset += padding
    fi.offset = offset
    offset += si

struct_alignment = max(a1, a2, ..., an)
if offset % struct_alignment != 0:
    tail_padding = struct_alignment - (offset % struct_alignment)
    offset += tail_padding
struct_size = offset
```

### Layout Examples

#### Example 1: Fixed Struct

```cpp
struct Point {
    f32 x;  // offset 0, size 4
    f32 y;  // offset 4, size 4
};
// alignment: 4, size: 8
```

```
┌───────────┬───────────┐
│     x     │     y     │
│  4 bytes  │  4 bytes  │
└───────────┴───────────┘
```

#### Example 2: Mixed Types with Padding

```cpp
struct Mixed {
    u8  a;    // offset 0, size 1
    // 3 bytes padding
    u32 b;    // offset 4, size 4
    u8  c;    // offset 8, size 1
    // 1 byte padding
    u16 d;    // offset 10, size 2
};
// alignment: 4, size: 12
```

```
┌─────┬─────────────┬───────────┬─────┬─────┬───────┐
│  a  │   padding   │     b     │  c  │ pad │   d   │
│  1  │      3      │     4     │  1  │  1  │   2   │
└─────┴─────────────┴───────────┴─────┴─────┴───────┘
Total: 12 bytes
```

#### Example 3: Nested Struct

```cpp
struct Vec3 {
    f32 x;  // offset 0
    f32 y;  // offset 4
    f32 z;  // offset 8
};
// alignment: 4, size: 12

struct Transform {
    Vec3    position;  // offset 0, size 12
    f32 rotation;  // offset 12, size 4
};
// alignment: 4, size: 16
```

#### Example 4: Array Field

```cpp
struct Matrix2x2 {
    f32 data[4];  // offset 0, size 16
};
// alignment: 4, size: 16

struct Particle {
    u64     id;      // offset 0, size 8
    f32     pos[3];  // offset 8, size 12
    // 4 bytes padding
    u64     flags;   // offset 24, size 8
};
// alignment: 8, size: 32
```

---

## Wire Format

ZMEM supports three message types: **Fixed Struct**, **Variable Struct** (with embedded vectors), and **Array** messages.

### Message Types

| Message Type | Header | Overhead | Use Case |
|--------------|--------|----------|----------|
| Fixed Struct | **None** | 0 bytes | Single trivially-copyable struct |
| Variable Struct | Size (8B) | 8 bytes | Struct with vector field(s) |
| Array | Count (8B) | 8 bytes | Sequence of elements |

The receiver must know the expected type at compile time. There is no type identification in the wire format.

---

### Fixed Struct Message

For serializing a trivially-copyable struct (no vector fields):

```
┌─────────────────────────┐
│      Struct Bytes       │
│      sizeof(T)          │
└─────────────────────────┘
```

| Field | Size | Description |
|-------|------|-------------|
| Payload | sizeof(T) | Raw struct bytes, directly memcpy-able |

**Total message size**: `sizeof(T)` — **zero overhead**

This enables maximum performance: serialize with `memcpy(&buf, &struct, sizeof(T))`, deserialize with `memcpy(&struct, buf, sizeof(T))` or direct pointer cast.

---

### Variable Struct Message

For serializing a struct with vector field(s):

```
┌─────────────────────────┬─────────────────────────┬─────────────────────────┐
│      Total Size         │    Inline Section       │   Variable Section      │
│       8 bytes           │     (fixed size)        │   (variable size)       │
└─────────────────────────┴─────────────────────────┴─────────────────────────┘
```

| Field | Size | Description |
|-------|------|-------------|
| Total Size | 8 | Total bytes after this field (inline + variable) |
| Inline Section | fixed | Scalar fields + vector refs (offset, count) |
| Variable Section | varies | Array data for all vector fields |

**Total message size**: `8 + inline_size + variable_size`

The size field enables:
- Stream framing (know how many bytes to read)
- Skipping messages without fully parsing
- Buffer pre-allocation

#### Maximum Message Size

The 8-byte size field (u64) supports a theoretical maximum of **2^64 - 1 bytes** (~18 exabytes). Practical limits are much lower:

| Constraint | Typical Limit | Notes |
|------------|---------------|-------|
| System memory | 8GB - 1TB | Depends on available RAM |
| 32-bit systems | ~4GB | `size_t` is 32-bit |
| File systems | 16TB (ext4, NTFS) | Varies by file system |
| Network messages | Application-defined | Often 64MB - 1GB |

**Implementation requirements**:
- Implementations MUST validate that size fits in platform's `size_t` before allocation
- Implementations SHOULD reject messages exceeding available memory
- Implementations MAY impose application-specific size limits

**Overflow behavior**: The size field stores the actual byte count; overflow is not possible within the wire format. Attempting to serialize data exceeding 2^64 - 1 bytes is an application error.

**Recommendation**: Define application-specific maximum message sizes based on your use case. For network protocols, document the maximum in your protocol specification.

#### Inline Section Layout

The inline section has a fixed size determined by the struct definition:
- Scalar fields: stored at their natural size/alignment
- Fixed arrays: stored inline (element_size × N)
- Vector fields: stored as `{offset::u64, count::u64}` (16 bytes, 8-byte aligned)
- Nested fixed structs: stored inline
- Nested variable structs: stored inline (their vector refs point into the parent's variable section)

#### Variable Section Layout

The variable section contains array data for all vector fields, concatenated with proper alignment:

**For vectors of fixed types** (trivially copyable):
```
[element₀][element₁]...[elementₙ₋₁]
```
Contiguous, no overhead.

**For vectors of variable types** (structs with vectors):
```
┌─────────────────────────────────────────┬────────────────────────────────────┐
│  Offset Table (count × 8 bytes)         │  Serialized Elements               │
│  [off₀][off₁]...[offₙ₋₁]                │  [elem₀][elem₁]...[elemₙ₋₁]        │
└─────────────────────────────────────────┴────────────────────────────────────┘
```
Each offset points to that element's inline section start. Elements include their own inline + variable sections.

#### Offset Semantics

All offsets are **relative to byte 8** (start of inline section, after the size field).

This means:
- `offset = 0` points to the first byte of the inline section
- `offset = inline_size` points to the first byte of the variable section
- For nested variable elements (in `[VariableStruct]`), each element is self-contained with offsets relative to **that element's own byte 8**, not the parent's byte 8

#### Complete Example

```cpp
struct Entity {
    uint64_t id;
    std::vector<float> weights;
};

struct Scene {
    std::vector<Entity> entities;
    float scale;
};
```

Scene with:
- entities = [{id:1, weights:[1.0, 2.0]}, {id:2, weights:[3.0, 4.0, 5.0]}]
- scale = 1.0

**Type signatures** (for compile-time validation, not in wire format):
- Entity: `Entity{id::u64,weights::[f32]}`
- Scene: `Scene{entities::[Entity{id::u64,weights::[f32]}],scale::f32}`

**Wire layout** (offsets relative to byte 8):

```
Wire    Data
Offset  Offset  Size  Content
------  ------  ----  -------
0       -       8     Total Size = 100 (bytes after this field)
8       0       8     entities.offset = 24
16      8       8     entities.count = 2
24      16      4     scale = 1.0f
28      20      4     [padding to 8-byte align]
        ─────── Inline section: 24 bytes ───────
32      24      8     offset_table[0] = 40 → Entity 0
40      32      8     offset_table[1] = 72 → Entity 1
        ─────── Entity 0 (inline + variable) ───────
48      40      8     Entity 0: id = 1
56      48      8     Entity 0: weights.offset = 64
64      56      8     Entity 0: weights.count = 2
72      64      4     weights[0] = 1.0f
76      68      4     weights[1] = 2.0f
        ─────── Entity 1 (inline + variable) ───────
80      72      8     Entity 1: id = 2
88      80      8     Entity 1: weights.offset = 96
96      88      8     Entity 1: weights.count = 3
104     96      4     weights[0] = 3.0f
108     100     4     weights[1] = 4.0f
112     104     4     weights[2] = 5.0f
────────────────────────────────────────────────
Total: 116 bytes (8 size + 108 data)
```

---

### Array Message

For serializing a contiguous sequence of elements (`std::vector<T>`, `std::array<T, N>`, raw arrays):

```
┌─────────────────────────┬─────────────────────────┐
│        Count            │        Elements         │
│       8 bytes           │   count * sizeof(T)     │
└─────────────────────────┴─────────────────────────┘
```

| Field | Size | Description |
|-------|------|-------------|
| Count | 8 | Number of elements (u64, little-endian) |
| Elements | varies | Raw element bytes, contiguous |

**Total message size**: `8 + count * sizeof(T)` — **8-byte overhead only**

**Key properties**:
- Elements are stored contiguously with no padding between them
- The 8-byte count header means elements start at 8-byte aligned offset
- Empty arrays are valid: count = 0, total size = 8 bytes
- Element type `T` must be ZMEM-conforming (primitive, fixed struct, or variable struct)

#### Supported Containers

Any container with contiguous storage can be serialized as a ZMEM Array:

| Language | Source Types | Deserialization Target |
|----------|--------------|------------------------|
| C++ | `std::vector<T>`, `std::array<T,N>`, `T[]`, `std::span<T>` | `std::vector<T>`, `std::span<const T>` (zero-copy view) |
| Rust | `Vec<T>`, `[T; N]`, `&[T]` | `Vec<T>`, `&[T]` (zero-copy view) |
| C | `T[]`, `T*` with count | `T*` with count |

#### What Arrays Can Contain

| Element Type | Supported | Storage Mode |
|--------------|-----------|--------------|
| Primitives (`f32`, `i32`, etc.) | Yes | Contiguous |
| Fixed structs (no vectors) | Yes | Contiguous |
| Variable structs (with vectors) | Yes | Offset table + elements |
| Nested vectors (`vector<vector<T>>`) | No | Not supported |

**Note**: Non-contiguous containers (`std::deque`, `std::list`) cannot be serialized directly - convert to `std::vector` first.

---

### Type Signatures (Compile-Time Validation)

Type signatures provide a canonical string representation of each type. Use these for **compile-time validation** to ensure sender and receiver agree on struct layout.

**Purpose**: Detect schema mismatches at compile/link time by comparing signatures between sender and receiver code.

#### Type Signature Format

The type signature is a canonical string representation.

**Struct signature**:
```
StructName{field1::type1,field2::type2,...}
```

**Enum signature**:
```
EnumName:underlying{Variant1=value1,Variant2=value2,...}
```

**Union signature**:
```
UnionName:tag_type|Variant1=value1|Variant2=value2{field1::type1,...}|...
```

Unit variants: `Name=value`
Struct variants: `Name=value{field::type,...}`
Type reference variants: `Name=value::TypeSignature`

**Array message signature** (for top-level ZMEM Array messages):
```
[ElementSignature]
```

**Embedded vector field** (inside structs):
```
fieldname::[elementtype]
```

Rules:
1. Struct name followed by `{` and `}`
2. Fields listed in declaration order
3. Each field: `name::type` separated by `,`
4. No spaces
5. Nested structs expand recursively inline
6. Fixed arrays (inside structs) written as `type[N]`
7. Vector fields (inside structs) written as `[type]`
8. Top-level array messages wrap element signature in `[]`
9. Optional fields written as `opt<type>`
10. Enum fields expand to full enum signature: `EnumName:underlying{Variant=value,...}`
11. Union fields expand to full union signature: `UnionName:tag|Variant=value{...}|...`
12. **Type aliases are always expanded** to their underlying type (aliases never appear in signatures)
13. **Default values are NOT included** in type signatures (they have no wire format impact)
14. **Constants are expanded to literal values** in type signatures (e.g., `T[SIZE]` becomes `T[64]`)
15. **Variable-length strings** written as `string`
16. **Nested vectors** written recursively: `[[type]]` for vector of vectors

**Character set**: Type signatures are ASCII-only (since identifiers are restricted to `[a-zA-Z_][a-zA-Z0-9_]*`). Signatures are compared byte-wise as strings. This avoids Unicode normalization issues.

#### Distinguishing Type Syntax

| Syntax | Meaning | Example |
|--------|---------|---------|
| `type[N]` | Fixed array of N elements (inline) | `data::f32[4]` |
| `[type]` | Vector field (offset + count) | `data::[f32]` |
| `[[type]]` | Nested vector (offset table format) | `rows::[[f32]]` |
| `string` | Variable-length string | `message::string` |
| `opt<type>` | Optional value (present + value) | `id::opt<u64>` |
| `Enum:type{...}` | Enum with variants | `status::Status:u8{...}` |
| `Union:type\|...\|` | Union with variants | `result::Result:u32\|Ok=0{...}\|Err=1{...}\|` |

#### Variable Struct Examples

```cpp
struct Fixed {
    f32 x;
    f32 y;
};
// Signature: Fixed{x::f32,y::f32}
// Category: Fixed (trivially copyable)

struct WithVector {
    u64 id;
    std::vector<float> weights;
};
// Signature: WithVector{id::u64,weights::[f32]}
// Category: Variable (has vector)

struct Nested {
    std::vector<WithVector> items;
    u32 count;
};
// Signature: Nested{items::[WithVector{id::u64,weights::[f32]}],count::u32}
// Category: Variable (has vector of variable)

struct Config {
    u64 id;
    zmem_optional<f64> timeout;
    zmem_optional<str[64]> name;
};
// Signature: Config{id::u64,timeout::opt<f64>,name::opt<str[64]>}
// Category: Fixed (optionals are trivially copyable)

enum Status : u8 {
    Pending = 0,
    Active = 1,
    Completed = 2,
    Failed = 3
};
// Signature: Status:u8{Pending=0,Active=1,Completed=2,Failed=3}

struct Task {
    u64 id;
    Status status;
    Status history[4];   // fixed array of enums
};
// Signature: Task{id::u64,status::Status:u8{Pending=0,Active=1,Completed=2,Failed=3},history::Status:u8{Pending=0,Active=1,Completed=2,Failed=3}[4]}
// Category: Fixed (enums are trivially copyable)

union Result : u32 {
    Ok = 0 { value::u64 }
    Err = 1 { code::i32, msg::str[64] }
};
// Signature: Result:u32|Ok=0{value::u64}|Err=1{code::i32,msg::str[64]}|
// Category: Fixed (all variants trivially copyable)

struct Response {
    u64 id;
    Result result;       // union field
};
// Signature: Response{id::u64,result::Result:u32|Ok=0{value::u64}|Err=1{code::i32,msg::str[64]}|}
// Category: Fixed (unions are trivially copyable when all variants are)

struct LogEntry {
    u64 timestamp;
    u8 level;
    zmem_string_ref message;  // variable-length string
};
// Signature: LogEntry{timestamp::u64,level::u8,message::string}
// Category: Variable (has variable-length string)

struct Document {
    zmem_string_ref title;
    zmem_string_ref content;
    zmem_vector_ref tags;     // vector of strings
};
// Signature: Document{title::string,content::string,tags::[string]}
// Category: Variable (has strings and vector)

struct Matrix {
    zmem_vector_ref rows;     // vector of vectors
};
// Signature: Matrix{rows::[[f32]]}
// Category: Variable (has nested vector)

struct Scene {
    zmem_string_ref name;
    zmem_vector_ref meshes;   // vector of variable type
};
// Signature: Scene{name::string,meshes::[Mesh{...}]}
// Category: Variable (has vector of variable structs)
```

#### Primitive Type Names in Signatures

Use the canonical ZMEM type names:
- `bool`, `i8`, `i16`, `i32`, `i64`, `i128`
- `u8`, `u16`, `u32`, `u64`, `u128`
- `f16`, `f32`, `f64`, `bf16`

#### Examples

**Fixed struct**:
```cpp
struct Point {
    f32 x;
    f32 y;
};
```
Signature: `Point{x::f32,y::f32}`

**With array**:
```cpp
struct Color {
    u8 rgba[4];
};
```
Signature: `Color{rgba::u8[4]}`

**Nested struct**:
```cpp
struct Vec3 {
    f32 x;
    f32 y;
    f32 z;
};

struct Bounds {
    Vec3 min;
    Vec3 max;
};
```
Signature for `Bounds`:
```
Bounds{min::Vec3{x::f32,y::f32,z::f32},max::Vec3{x::f32,y::f32,z::f32}}
```

**Deeply nested**:
```cpp
struct Inner {
    i32 value;
};

struct Middle {
    Inner data;
    u16 flags;
};

struct Outer {
    Middle m;
    bool active;
};
```
Signature for `Outer`:
```
Outer{m::Middle{data::Inner{value::i32},flags::u16},active::bool}
```

---

## Reading and Writing

### Writing a Fixed Struct Message

1. Write struct bytes directly: `memcpy(buf, &value, sizeof(T))`

That's it. Zero overhead.

### Reading a Fixed Struct Message

1. Read struct bytes directly: `memcpy(&value, buf, sizeof(T))`
2. Or cast pointer: `T* value = reinterpret_cast<T*>(buf)`

### Writing a Variable Struct Message

1. Compute total size: inline_size + variable_size
2. Write size (8 bytes, little-endian)
3. First pass: write inline section with placeholder offsets
4. Second pass: for each vector field:
   - Record current position as the offset
   - If vector contains fixed types: write elements contiguously
   - If vector contains variable types: write offset table, then recursively serialize each element
5. Patch offset values in inline section

### Reading a Variable Struct Message

1. Read 8-byte size (for buffer validation/allocation)
2. Read inline section (fixed size based on type)
3. For each vector field:
   - Read offset and count from inline section
   - Seek to offset position
   - If fixed elements: read count elements contiguously
   - If variable elements: read offset table, then recursively deserialize each element

### Zero-Copy Views

For read-only access without copying, create a **view type** that holds spans/pointers into the buffer:

```cpp
// Native struct (owns data)
struct Entity {
    uint64_t id;
    std::vector<float> weights;
};

// View struct (references buffer)
struct EntityView {
    uint64_t id;
    std::span<const float> weights;  // Points into buffer
};
```

Views enable true zero-copy access - no allocations, no memcpy for vector data.

### Writing an Array Message

1. Write count (8 bytes, little-endian)
2. Write element bytes directly: `memcpy(buf + 8, vec.data(), count * sizeof(T))`

### Reading an Array Message

1. Read 8-byte count
2. Validate: buffer size >= 8 + count * sizeof(T)
3. Either:
   - **Copy**: `memcpy(vec.data(), buf + 8, count * sizeof(T))`
   - **Zero-copy view**: `std::span<const T>(buf + 8, count)`

### Validation

**Fixed struct messages**:
- Buffer size >= sizeof(T)

**Variable struct messages**:
- Buffer size >= 8 (for size field)
- Buffer size >= 8 + size_value

**Array messages**:
- Buffer size >= 8 (for count field)
- Buffer size >= 8 + count * sizeof(T)

---

## Reference Implementation (C++)

#### Constants

ZMEM constants map directly to C++ `constexpr` values:

```cpp
#include <cstdint>
#include <array>
#include <string_view>

// Size constants (for array dimensions)
constexpr size_t MAX_NAME_LENGTH = 64;
constexpr size_t MAX_PLAYERS = 32;
constexpr size_t MATRIX_DIM = 4;

// Typed numeric constants
constexpr uint16_t DEFAULT_PORT = 8080;
constexpr uint64_t DEFAULT_TIMEOUT = 30000;
constexpr double PI = 3.14159265358979;

// Array constants
constexpr std::array<float, 3> ORIGIN = {0.0f, 0.0f, 0.0f};
constexpr std::array<uint8_t, 4> WHITE = {255, 255, 255, 255};

// String constants
constexpr std::string_view DEFAULT_HOST = "localhost";
constexpr std::string_view VERSION = "1.0.0";

// Boolean constants
constexpr bool DEBUG_MODE = false;
```

**Usage in structs**:

```cpp
// Constants used for array sizes
struct Player {
    char name[MAX_NAME_LENGTH];      // char[64]
    float position[3];
};

struct Game {
    Player players[MAX_PLAYERS];     // Player[32]
};

struct Matrix {
    float data[MATRIX_DIM][MATRIX_DIM];  // float[4][4]
};

// Constants used as defaults
struct ServerConfig {
    char host[MAX_NAME_LENGTH] = "localhost";
    uint16_t port = DEFAULT_PORT;        // = 8080
    uint64_t timeout = DEFAULT_TIMEOUT;  // = 30000
};

// Array constants as defaults
struct Particle {
    uint64_t id;
    std::array<float, 3> position = ORIGIN;
    std::array<uint8_t, 4> color = WHITE;
};
```

**Compile-time validation**:

```cpp
// Verify constants match schema expectations
static_assert(MAX_NAME_LENGTH == 64);
static_assert(MAX_PLAYERS == 32);
static_assert(DEFAULT_PORT == 8080);

// Verify struct sizes with constants
static_assert(sizeof(Player) == MAX_NAME_LENGTH + 12);  // name + position
```

#### Type Aliases

ZMEM type aliases map directly to C++ `using` declarations:

```cpp
#include <cstdint>
#include <array>

// Primitive aliases
using EntityId = uint64_t;
using Timestamp = int64_t;
using Score = double;

// Fixed array aliases
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Color = std::array<uint8_t, 4>;
using Mat4 = std::array<float, 16>;

// String aliases
using Name = char[64];
using Path = char[256];

// Composite aliases
using MaybeScore = zmem_optional<float>;
```

**Type alias traits** (optional, for reflection):

```cpp
template<typename T>
struct zmem_alias {
    static constexpr bool is_alias = false;
};

template<>
struct zmem_alias<EntityId> {
    static constexpr bool is_alias = true;
    using underlying = uint64_t;
    static constexpr std::string_view name = "EntityId";
};

template<>
struct zmem_alias<Vec3> {
    static constexpr bool is_alias = true;
    using underlying = std::array<float, 3>;
    static constexpr std::string_view name = "Vec3";
};

template<typename T>
inline constexpr bool is_zmem_alias_v = zmem_alias<T>::is_alias;
```

**Usage in structs**:

```cpp
struct Entity {
    EntityId id;      // underlying: uint64_t
    Name name;        // underlying: char[64]
    Vec3 position;    // underlying: std::array<float, 3>
    Timestamp created; // underlying: int64_t
};

// Aliases don't affect layout - this is identical to:
struct Entity_Expanded {
    uint64_t id;
    char name[64];
    std::array<float, 3> position;
    int64_t created;
};

static_assert(sizeof(Entity) == sizeof(Entity_Expanded));
```

#### Multi-dimensional Arrays

Multi-dimensional arrays map directly to C/C++ nested arrays:

```cpp
#include <array>
#include <cstdint>

// f32[4][4] - 4x4 matrix
struct Matrix4x4 {
    float data[4][4];  // or std::array<std::array<float, 4>, 4>
};

static_assert(sizeof(Matrix4x4) == 64);
static_assert(sizeof(float[4][4]) == 64);

// u8[1080][1920][4] - HD image with RGBA
struct Image {
    uint8_t pixels[1080][1920][4];
};

static_assert(sizeof(Image) == 1080 * 1920 * 4);  // 8,294,400 bytes
```

**Row-major memory order verification**:

```cpp
float mat[2][3] = {
    {1.0f, 2.0f, 3.0f},  // row 0
    {4.0f, 5.0f, 6.0f}   // row 1
};

// Memory layout: 1, 2, 3, 4, 5, 6 (row-major)
float* ptr = &mat[0][0];
// ptr[0] = 1, ptr[1] = 2, ptr[2] = 3, ptr[3] = 4, ptr[4] = 5, ptr[5] = 6

// Row 0 is contiguous with row 1
static_assert(&mat[0][2] + 1 == &mat[1][0]);
```

**Indexing formula**:

```cpp
// For T[A][B][C], element [i][j][k] is at:
// offset = (i * B * C + j * C + k) * sizeof(T)

template<typename T, size_t A, size_t B, size_t C>
T& access_3d(T (&arr)[A][B][C], size_t i, size_t j, size_t k) {
    // Compiler generates: arr[i][j][k]
    // Equivalent to: *(&arr[0][0][0] + i*B*C + j*C + k)
    return arr[i][j][k];
}
```

**Using std::array**:

```cpp
// Equivalent representations:
using Matrix4x4_CStyle = float[4][4];
using Matrix4x4_StdArray = std::array<std::array<float, 4>, 4>;

static_assert(sizeof(Matrix4x4_CStyle) == sizeof(Matrix4x4_StdArray));
static_assert(std::is_trivially_copyable_v<Matrix4x4_StdArray>);

// Both have identical memory layout
Matrix4x4_StdArray mat = {{
    {{1, 0, 0, 0}},
    {{0, 1, 0, 0}},
    {{0, 0, 1, 0}},
    {{0, 0, 0, 1}}
}};
```

#### Struct Requirements

**Fixed structs** (no vectors) must be trivially copyable:

```cpp
#include <type_traits>

template<typename T>
constexpr bool is_fixed_zmem_struct() {
    return std::is_trivially_copyable_v<T> &&
           std::is_standard_layout_v<T>;
}

// Usage for fixed structs
static_assert(is_fixed_zmem_struct<Point>());
```

**`std::is_trivially_copyable`** ensures:
- Trivial copy/move constructors and assignment operators
- Trivial destructor
- No virtual functions or virtual bases

**`std::is_standard_layout`** ensures:
- All non-static members have same access control
- No virtual functions or virtual base classes
- All non-static members (including in base classes) are in one class
- First non-static member is not a base class type
- No base classes of same type as first non-static member

**Variable structs** (with vectors) are NOT trivially copyable but still ZMEM-conforming:

```cpp
// Variable struct - has vector, cannot use memcpy directly
struct Entity {
    uint64_t id;
    std::vector<float> weights;  // Makes this variable
};

// Variable structs use offset-based serialization instead of memcpy
static_assert(!std::is_trivially_copyable_v<Entity>);  // Expected!
```

#### Enum Types

ZMEM enums map directly to C++ scoped enums (`enum class`):

```cpp
#include <type_traits>
#include <cstdint>

// Generated from: enum Status : u8 { Pending=0, Active=1, Completed=2, Failed=3 }
enum class Status : uint8_t {
    Pending = 0,
    Active = 1,
    Completed = 2,
    Failed = 3
};

// Compile-time validation
static_assert(std::is_trivially_copyable_v<Status>);
static_assert(std::is_standard_layout_v<Status>);
static_assert(sizeof(Status) == sizeof(uint8_t));
static_assert(alignof(Status) == alignof(uint8_t));

// Enums can be used in is_fixed_zmem_struct check
static_assert(std::is_enum_v<Status>);
```

**Enum traits helper**:

```cpp
// Type trait to check if T is a ZMEM enum
template<typename T>
struct is_zmem_enum : std::false_type {};

template<>
struct is_zmem_enum<Status> : std::true_type {
    using underlying = uint8_t;
    static constexpr std::string_view signature =
        "Status:u8{Pending=0,Active=1,Completed=2,Failed=3}";
};

template<typename T>
inline constexpr bool is_zmem_enum_v = is_zmem_enum<T>::value;

// Usage
static_assert(is_zmem_enum_v<Status>);
static_assert(!is_zmem_enum_v<int>);
```

**Optional enum validation** (when performance allows):

```cpp
#include <optional>

template<typename E>
constexpr bool is_valid_enum(E value);

// Specialize for each enum
template<>
constexpr bool is_valid_enum<Status>(Status value) {
    switch (value) {
        case Status::Pending:
        case Status::Active:
        case Status::Completed:
        case Status::Failed:
            return true;
        default:
            return false;
    }
}

// Validated deserialization (optional, adds overhead)
template<typename E>
std::optional<E> deserialize_enum_validated(const uint8_t* data) {
    static_assert(std::is_enum_v<E>);
    E value;
    std::memcpy(&value, data, sizeof(E));
    if (is_valid_enum(value)) {
        return value;
    }
    return std::nullopt;
}
```

**Struct with enum fields**:

```cpp
struct Task {
    uint64_t id;
    Status status;
    // 7 bytes padding (alignment to 8)
};

static_assert(sizeof(Task) == 16);
static_assert(offsetof(Task, id) == 0);
static_assert(offsetof(Task, status) == 8);
static_assert(std::is_trivially_copyable_v<Task>);

// Serialize/deserialize like any fixed struct
Task task{42, Status::Active};
uint8_t buffer[sizeof(Task)];
std::memcpy(buffer, &task, sizeof(Task));
```

#### Union Types

ZMEM unions map to C++ structs with a tag enum and anonymous union:

```cpp
#include <type_traits>
#include <cstdint>
#include <cstring>
#include <cassert>

// Generated from: union Result : u32 { Ok = 0 { value::u64 }, Err = 1 { code::i32, msg::str[64] } }

struct Result {
    enum class Tag : uint32_t {
        Ok = 0,
        Err = 1
    };

    Tag tag;

private:
    union Data {
        struct { uint64_t value; } ok;
        struct { int32_t code; char msg[64]; } err;
    } data_;

public:
    // Tag checks
    bool is_ok() const { return tag == Tag::Ok; }
    bool is_err() const { return tag == Tag::Err; }

    // Accessors (with assertions for safety)
    uint64_t& as_ok() {
        assert(is_ok());
        return data_.ok.value;
    }
    const uint64_t& as_ok() const {
        assert(is_ok());
        return data_.ok.value;
    }

    auto& as_err() {
        assert(is_err());
        return data_.err;
    }
    const auto& as_err() const {
        assert(is_err());
        return data_.err;
    }

    // Factory methods
    static Result make_ok(uint64_t value) {
        Result r{};
        r.tag = Tag::Ok;
        r.data_.ok.value = value;
        return r;
    }

    static Result make_err(int32_t code, const char* msg) {
        Result r{};
        r.tag = Tag::Err;
        r.data_.err.code = code;
        std::strncpy(r.data_.err.msg, msg, 63);
        r.data_.err.msg[63] = '\0';
        return r;
    }
};

// Layout validation
static_assert(sizeof(Result) == 76);   // 4 tag + 4 padding + 68 max variant
static_assert(alignof(Result) == 8);   // max alignment of variants
static_assert(std::is_trivially_copyable_v<Result>);
static_assert(std::is_standard_layout_v<Result>);
static_assert(offsetof(Result, tag) == 0);
```

**Union type traits helper**:

```cpp
template<typename T>
struct is_zmem_union : std::false_type {};

template<>
struct is_zmem_union<Result> : std::true_type {
    using tag_type = uint32_t;
    static constexpr std::string_view signature =
        "Result:u32|Ok=0{value::u64}|Err=1{code::i32,msg::str[64]}|";
};

template<typename T>
inline constexpr bool is_zmem_union_v = is_zmem_union<T>::value;

// Usage
static_assert(is_zmem_union_v<Result>);
static_assert(!is_zmem_union_v<int>);
```

**Serialization (fixed union)**:

```cpp
// Serialize - just memcpy (zero overhead)
Result result = Result::make_ok(42);
uint8_t buffer[sizeof(Result)];
std::memcpy(buffer, &result, sizeof(Result));

// Deserialize
Result restored;
std::memcpy(&restored, buffer, sizeof(Result));

if (restored.is_ok()) {
    std::cout << "Ok: " << restored.as_ok() << "\n";
} else {
    std::cout << "Err: " << restored.as_err().code << "\n";
}

// Zero-copy view (if buffer is aligned)
const Result& view = *reinterpret_cast<const Result*>(buffer);
```

**Pattern matching helper (C++17)**:

```cpp
#include <variant>

// Convert ZMEM union to std::variant for pattern matching
template<typename... Fs>
struct overloaded : Fs... { using Fs::operator()...; };
template<typename... Fs> overloaded(Fs...) -> overloaded<Fs...>;

// Usage with visitor pattern
void process(const Result& r) {
    if (r.is_ok()) {
        std::cout << "Success: " << r.as_ok() << "\n";
    } else {
        const auto& err = r.as_err();
        std::cout << "Error " << err.code << ": " << err.msg << "\n";
    }
}
```

#### Default Values

Default values are codegen hints with **no wire format impact**. Generated code uses C++ default member initializers:

```cpp
// Schema:
// struct ServerConfig {
//   host::str[64] = "localhost"
//   port::u16 = 8080
//   timeout_ms::u64 = 30000
//   max_connections::u32 = 100
// }

struct ServerConfig {
    char host[64] = "localhost";
    uint16_t port = 8080;
    uint64_t timeout_ms = 30000;
    uint32_t max_connections = 100;
};

// Layout is identical to struct without defaults
static_assert(sizeof(ServerConfig) == 88);
static_assert(std::is_trivially_copyable_v<ServerConfig>);
```

**Array defaults**:

```cpp
// Schema:
// struct Color {
//   rgba::u8[4] = [255, 255, 255, 255]
// }

struct Color {
    uint8_t rgba[4] = {255, 255, 255, 255};
};

// Multi-dimensional array defaults
// struct Transform {
//   matrix::f32[4][4] = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]
// }

struct Transform {
    float matrix[4][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f}
    };
};
```

**Optional defaults**:

```cpp
// Schema:
// struct Connection {
//   host::str[64]
//   port::opt<u16> = 8080           # optional with default
//   timeout::opt<u64>               # optional, no default (absent)
// }

struct Connection {
    char host[64];
    zmem_optional<uint16_t> port = zmem_optional<uint16_t>::some(8080);
    zmem_optional<uint64_t> timeout = zmem_optional<uint64_t>::none();

    // Accessor with default fallback
    uint16_t port_or_default() const {
        return port.has_value() ? *port : 8080;
    }
};
```

**Enum defaults**:

```cpp
// Schema:
// enum ConnectionState : u8 {
//   Disconnected = 0 default     # marked as default
//   Connecting = 1
//   Connected = 2
//   Error = 3
// }

enum class ConnectionState : uint8_t {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Error = 3
};

// Trait provides default value
template<>
struct zmem_enum_default<ConnectionState> {
    static constexpr ConnectionState value = ConnectionState::Disconnected;
};

// Usage in struct
struct Client {
    uint64_t id;
    ConnectionState state = ConnectionState::Disconnected;  // default applied
};
```

**Important**: Defaults are purely for initialization convenience. Wire format contains actual values, not defaults:

```cpp
// If sender writes:
ServerConfig config{};  // all defaults applied
config.port = 9000;     // override one field
serialize(config);      // ALL fields written to wire, including defaults

// Receiver reads exact values sent, NOT defaults
ServerConfig received = deserialize<ServerConfig>(buffer);
// received.port == 9000 (from wire)
// received.host == "localhost" (from wire, happened to match default)
```

#### Optional Type

ZMEM defines an explicit `zmem_optional<T>` with guaranteed cross-platform layout:

```cpp
#include <cstdint>
#include <cstring>
#include <optional>

template<typename T>
struct zmem_optional {
    uint8_t present;                    // 0 = absent, 1 = present
    alignas(alignof(T)) uint8_t _pad[alignof(T) - 1];  // explicit padding (must be zero)
    T value;                            // must be zero when absent

    // Accessors
    bool has_value() const noexcept { return present != 0; }

    T& operator*() noexcept { return value; }
    const T& operator*() const noexcept { return value; }

    T* operator->() noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }

    // Conversion to/from std::optional
    operator std::optional<T>() const {
        return present ? std::optional<T>(value) : std::nullopt;
    }

    zmem_optional& operator=(const std::optional<T>& opt) {
        if (opt.has_value()) {
            present = 1;
            std::memset(_pad, 0, sizeof(_pad));  // zero padding
            value = *opt;
        } else {
            // Zero entire struct when absent (deterministic serialization)
            std::memset(this, 0, sizeof(*this));
        }
        return *this;
    }

    // Factory functions
    static zmem_optional none() noexcept {
        zmem_optional result{};  // zero-initialized
        return result;
    }

    static zmem_optional some(const T& val) noexcept {
        zmem_optional result{};  // zero-initialized (including padding)
        result.present = 1;
        result.value = val;
        return result;
    }
};

// Layout validation
static_assert(sizeof(zmem_optional<uint8_t>) == 2);
static_assert(sizeof(zmem_optional<uint16_t>) == 4);
static_assert(sizeof(zmem_optional<uint32_t>) == 8);
static_assert(sizeof(zmem_optional<uint64_t>) == 16);
static_assert(sizeof(zmem_optional<float>) == 8);
static_assert(sizeof(zmem_optional<double>) == 16);

static_assert(alignof(zmem_optional<uint64_t>) == 8);
static_assert(alignof(zmem_optional<uint32_t>) == 4);

// Usage
zmem_optional<uint64_t> opt_id = zmem_optional<uint64_t>::some(42);
if (opt_id.has_value()) {
    uint64_t id = *opt_id;
}

// Convert from std::optional
std::optional<float> std_opt = 3.14f;
zmem_optional<float> zmem_opt;
zmem_opt = std_opt;

// Convert to std::optional
std::optional<float> back = zmem_opt;
```

**Why not use `std::optional` directly?**

`std::optional<T>` has implementation-defined layout:
- MSVC, GCC, and Clang may use different padding/ordering
- The discriminant (has_value flag) position varies
- Size may exceed `sizeof(T) + 1` due to alignment choices

`zmem_optional<T>` guarantees:
- Discriminant at offset 0 (1 byte)
- Padding to align value (alignof(T) - 1 bytes)
- Value at offset alignof(T)
- Total size: alignof(T) + sizeof(T)

This ensures identical wire representation across all platforms.

#### Variable-Length Strings

Variable-length strings use a reference type for the inline section:

```cpp
#include <cstdint>
#include <string>
#include <string_view>

// Inline representation of a variable-length string
// alignas(8) ensures consistent layout on both 32-bit and 64-bit platforms
struct alignas(8) zmem_string_ref {
    uint64_t offset;    // byte offset to string data (relative to byte 8 of containing struct)
    uint64_t length;    // byte length (NOT null-terminated)
};

static_assert(sizeof(zmem_string_ref) == 16);
static_assert(alignof(zmem_string_ref) == 8);

// Helper to read a string from a buffer
std::string_view read_string(const uint8_t* inline_base, const zmem_string_ref& ref) {
    const char* data = reinterpret_cast<const char*>(inline_base + ref.offset);
    return std::string_view(data, ref.length);
}

// Example struct with variable-length string
struct LogEntry {
    uint64_t timestamp;
    uint8_t level;
    uint8_t _pad[7];           // padding to align zmem_string_ref
    zmem_string_ref message;  // variable-length string
    char source[64];           // fixed-length string
};

static_assert(sizeof(LogEntry) == 96);  // 8 + 1 + 7 + 16 + 64
```

**Serialization**:

```cpp
#include <vector>
#include <cstring>

std::vector<uint8_t> serialize_log_entry(
    uint64_t timestamp, uint8_t level,
    std::string_view message, const char* source
) {
    // Calculate sizes
    size_t inline_size = sizeof(LogEntry);
    size_t variable_size = message.size();
    size_t total_payload = inline_size + variable_size;

    std::vector<uint8_t> buffer(8 + total_payload);  // size header + payload
    uint8_t* ptr = buffer.data();

    // Write size header
    uint64_t size = total_payload;
    std::memcpy(ptr, &size, 8);
    ptr += 8;

    // Write inline section
    LogEntry entry{};
    entry.timestamp = timestamp;
    entry.level = level;
    entry.message.offset = inline_size;  // relative to inline start
    entry.message.length = message.size();
    std::strncpy(entry.source, source, 63);
    entry.source[63] = '\0';
    std::memcpy(ptr, &entry, sizeof(entry));
    ptr += sizeof(entry);

    // Write variable section (string data)
    std::memcpy(ptr, message.data(), message.size());

    return buffer;
}
```

#### Nested Vectors

For vectors of variable-size elements, use an offset table:

```cpp
#include <vector>
#include <cstdint>
#include <cstring>

// Serialize vector<vector<int32_t>>
std::vector<uint8_t> serialize_nested_vector(
    const std::vector<std::vector<int32_t>>& data
) {
    size_t count = data.size();

    // Calculate offset table size
    size_t offset_table_size = (count + 1) * sizeof(uint64_t);

    // Calculate each inner vector's serialized size
    std::vector<size_t> element_sizes;
    size_t total_data_size = 0;
    for (const auto& inner : data) {
        // Inner vector: [count:8][data...]
        size_t elem_size = 8 + inner.size() * sizeof(int32_t);
        element_sizes.push_back(elem_size);
        total_data_size += elem_size;
    }

    // Allocate buffer
    std::vector<uint8_t> buffer(offset_table_size + total_data_size);
    uint8_t* ptr = buffer.data();

    // Write offset table
    uint64_t current_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(ptr + i * 8, &current_offset, 8);
        current_offset += element_sizes[i];
    }
    // Write sentinel (total data size)
    std::memcpy(ptr + count * 8, &current_offset, 8);

    // Write data section
    uint8_t* data_ptr = ptr + offset_table_size;
    for (const auto& inner : data) {
        // Write inner vector count
        uint64_t inner_count = inner.size();
        std::memcpy(data_ptr, &inner_count, 8);
        data_ptr += 8;

        // Write inner vector data
        if (!inner.empty()) {
            std::memcpy(data_ptr, inner.data(), inner.size() * sizeof(int32_t));
            data_ptr += inner.size() * sizeof(int32_t);
        }
    }

    return buffer;
}

// Deserialize vector<vector<int32_t>>
std::vector<std::vector<int32_t>> deserialize_nested_vector(
    const uint8_t* buffer, size_t count
) {
    std::vector<std::vector<int32_t>> result;
    result.reserve(count);

    // Read offset table
    const uint64_t* offsets = reinterpret_cast<const uint64_t*>(buffer);
    const uint8_t* data_start = buffer + (count + 1) * 8;

    for (size_t i = 0; i < count; ++i) {
        const uint8_t* elem_ptr = data_start + offsets[i];

        // Read inner vector count
        uint64_t inner_count;
        std::memcpy(&inner_count, elem_ptr, 8);
        elem_ptr += 8;

        // Read inner vector data
        std::vector<int32_t> inner(inner_count);
        if (inner_count > 0) {
            std::memcpy(inner.data(), elem_ptr, inner_count * sizeof(int32_t));
        }
        result.push_back(std::move(inner));
    }

    return result;
}
```

#### Vectors of Variable Structs

For vectors where elements are variable structs (have their own variable sections):

```cpp
// Each element is self-contained: [size:8][inline][variable]

struct Entity {
    uint64_t id;
    zmem_vector_ref weights;  // vector field makes this variable
};

// Serialize a single Entity
std::vector<uint8_t> serialize_entity(uint64_t id, const std::vector<float>& weights) {
    size_t inline_size = sizeof(Entity);
    size_t variable_size = weights.size() * sizeof(float);
    size_t payload_size = inline_size + variable_size;

    std::vector<uint8_t> buffer(8 + payload_size);  // size + payload
    uint8_t* ptr = buffer.data();

    // Size header
    uint64_t size = payload_size;
    std::memcpy(ptr, &size, 8);
    ptr += 8;

    // Inline section
    Entity entity{};
    entity.id = id;
    entity.weights.offset = inline_size;
    entity.weights.count = weights.size();
    std::memcpy(ptr, &entity, sizeof(entity));
    ptr += sizeof(entity);

    // Variable section
    if (!weights.empty()) {
        std::memcpy(ptr, weights.data(), variable_size);
    }

    return buffer;
}

// Serialize vector<Entity>
std::vector<uint8_t> serialize_entity_vector(
    const std::vector<std::pair<uint64_t, std::vector<float>>>& entities
) {
    size_t count = entities.size();

    // Serialize each entity first
    std::vector<std::vector<uint8_t>> serialized_entities;
    for (const auto& [id, weights] : entities) {
        serialized_entities.push_back(serialize_entity(id, weights));
    }

    // Calculate offset table
    size_t offset_table_size = (count + 1) * sizeof(uint64_t);
    size_t total_data_size = 0;
    for (const auto& se : serialized_entities) {
        total_data_size += se.size();
    }

    // Build buffer
    std::vector<uint8_t> buffer(offset_table_size + total_data_size);
    uint8_t* ptr = buffer.data();

    // Write offset table
    uint64_t current_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(ptr + i * 8, &current_offset, 8);
        current_offset += serialized_entities[i].size();
    }
    std::memcpy(ptr + count * 8, &current_offset, 8);  // sentinel

    // Write serialized entities
    uint8_t* data_ptr = ptr + offset_table_size;
    for (const auto& se : serialized_entities) {
        std::memcpy(data_ptr, se.data(), se.size());
        data_ptr += se.size();
    }

    return buffer;
}
```

#### Vector of Strings

```cpp
// Serialize vector<string>
std::vector<uint8_t> serialize_string_vector(const std::vector<std::string>& strings) {
    size_t count = strings.size();
    size_t offset_table_size = (count + 1) * sizeof(uint64_t);

    // Calculate total string data size
    size_t total_string_size = 0;
    for (const auto& s : strings) {
        total_string_size += s.size();
    }

    std::vector<uint8_t> buffer(offset_table_size + total_string_size);
    uint8_t* ptr = buffer.data();

    // Write offset table
    uint64_t current_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(ptr + i * 8, &current_offset, 8);
        current_offset += strings[i].size();
    }
    std::memcpy(ptr + count * 8, &current_offset, 8);  // sentinel

    // Write string data
    uint8_t* data_ptr = ptr + offset_table_size;
    for (const auto& s : strings) {
        std::memcpy(data_ptr, s.data(), s.size());
        data_ptr += s.size();
    }

    return buffer;
}

// Deserialize vector<string>
std::vector<std::string> deserialize_string_vector(const uint8_t* buffer, size_t count) {
    std::vector<std::string> result;
    result.reserve(count);

    const uint64_t* offsets = reinterpret_cast<const uint64_t*>(buffer);
    const char* data_start = reinterpret_cast<const char*>(buffer + (count + 1) * 8);

    for (size_t i = 0; i < count; ++i) {
        size_t start = offsets[i];
        size_t length = offsets[i + 1] - offsets[i];
        result.emplace_back(data_start + start, length);
    }

    return result;
}
```

#### Type Signature Validation

```cpp
#include <string_view>

// Type signature traits (user-defined per struct)
template<typename T>
struct zmem_signature;

template<>
struct zmem_signature<Point> {
    static constexpr std::string_view value = "Point{x::f32,y::f32}";
};

// Sender and receiver both define the same signature
// Linker error if signatures don't match across translation units
```

#### Field Offset Validation

```cpp
#include <cstddef>

struct Point {
    float x;
    float y;
};

static_assert(offsetof(Point, x) == 0);
static_assert(offsetof(Point, y) == 4);
static_assert(sizeof(Point) == 8);
static_assert(alignof(Point) == 4);
```

#### Fixed Struct Serialization

```cpp
#include <cstring>
#include <vector>
#include <cstdint>

// Serialize fixed struct - ZERO overhead
template<typename T>
void serialize(const T& value, uint8_t* buf) {
    static_assert(is_fixed_zmem_struct<T>());
    std::memcpy(buf, &value, sizeof(T));
}

// Deserialize fixed struct
template<typename T>
void deserialize(const uint8_t* buf, T& out) {
    static_assert(is_fixed_zmem_struct<T>());
    std::memcpy(&out, buf, sizeof(T));
}

// Zero-copy access (if buffer is properly aligned)
template<typename T>
const T& view(const uint8_t* buf) {
    static_assert(is_fixed_zmem_struct<T>());
    return *reinterpret_cast<const T*>(buf);
}

// Usage
Point p{1.0f, 2.0f};
uint8_t buffer[sizeof(Point)];
serialize(p, buffer);  // Just memcpy, nothing else

Point p2;
deserialize(buffer, p2);  // Just memcpy

const Point& p3 = view<Point>(buffer);  // Zero-copy!
```

#### Array Serialization

```cpp
#include <cstring>
#include <vector>
#include <span>
#include <cstdint>

// Serialize std::vector<T> - only 8-byte overhead (count)
template<typename T>
std::vector<uint8_t> serialize_array(const std::vector<T>& vec) {
    static_assert(is_fixed_zmem_struct<T>());

    const uint64_t count = vec.size();
    std::vector<uint8_t> buffer(8 + count * sizeof(T));

    // Write count
    std::memcpy(buffer.data(), &count, 8);

    // Write elements
    if (count > 0) {
        std::memcpy(buffer.data() + 8, vec.data(), count * sizeof(T));
    }

    return buffer;
}

// Deserialize into std::vector<T>
template<typename T>
bool deserialize_array(const uint8_t* data, size_t size, std::vector<T>& out) {
    static_assert(is_fixed_zmem_struct<T>());

    if (size < 8) {
        return false;
    }

    uint64_t count;
    std::memcpy(&count, data, 8);

    if (size < 8 + count * sizeof(T)) {
        return false;
    }

    out.resize(count);
    if (count > 0) {
        std::memcpy(out.data(), data + 8, count * sizeof(T));
    }

    return true;
}

// Zero-copy view
template<typename T>
std::span<const T> view_array(const uint8_t* data, size_t size) {
    static_assert(is_fixed_zmem_struct<T>());

    if (size < 8) {
        return {};
    }

    uint64_t count;
    std::memcpy(&count, data, 8);

    if (size < 8 + count * sizeof(T)) {
        return {};
    }

    return std::span<const T>(
        reinterpret_cast<const T*>(data + 8),
        count
    );
}
```

#### Variable Struct Serialization

For structs with embedded vectors:

```cpp
#include <cstring>
#include <vector>
#include <span>
#include <optional>
#include <cstdint>

// Example variable struct
struct Entity {
    uint64_t id;
    std::vector<float> weights;
};

// Inline size: id (8) + weights ref (16) = 24 bytes
constexpr size_t ENTITY_INLINE_SIZE = 24;

// Serialize a variable struct (8-byte size header)
std::vector<uint8_t> serialize_entity(const Entity& entity) {
    size_t variable_size = entity.weights.size() * sizeof(float);
    size_t payload_size = ENTITY_INLINE_SIZE + variable_size;
    size_t total_size = 8 + payload_size;

    std::vector<uint8_t> buffer(total_size);
    uint8_t* ptr = buffer.data();

    // Write size (payload only, not including size field)
    uint64_t size_field = payload_size;
    std::memcpy(ptr, &size_field, 8);
    ptr += 8;

    // Write inline section
    std::memcpy(ptr, &entity.id, 8);
    ptr += 8;

    // Write vector ref (offset relative to byte 8)
    uint64_t weights_offset = ENTITY_INLINE_SIZE;
    uint64_t weights_count = entity.weights.size();
    std::memcpy(ptr, &weights_offset, 8);
    ptr += 8;
    std::memcpy(ptr, &weights_count, 8);
    ptr += 8;

    // Write variable section
    if (!entity.weights.empty()) {
        std::memcpy(ptr, entity.weights.data(), variable_size);
    }

    return buffer;
}

// Deserialize into Entity (with copy)
bool deserialize_entity(const uint8_t* data, size_t size, Entity& out) {
    if (size < 8) return false;

    uint64_t payload_size;
    std::memcpy(&payload_size, data, 8);

    if (size < 8 + payload_size) return false;

    const uint8_t* ptr = data + 8;

    // Read inline section
    std::memcpy(&out.id, ptr, 8);
    ptr += 8;

    uint64_t weights_offset, weights_count;
    std::memcpy(&weights_offset, ptr, 8);
    ptr += 8;
    std::memcpy(&weights_count, ptr, 8);

    // Read variable section
    out.weights.resize(weights_count);
    if (weights_count > 0) {
        std::memcpy(out.weights.data(), data + 8 + weights_offset,
                    weights_count * sizeof(float));
    }

    return true;
}

// Zero-copy view type
struct EntityView {
    uint64_t id;
    std::span<const float> weights;
};

// Create view without copying
std::optional<EntityView> view_entity(const uint8_t* data, size_t size) {
    if (size < 8 + ENTITY_INLINE_SIZE) return std::nullopt;

    const uint8_t* ptr = data + 8;

    EntityView view;
    std::memcpy(&view.id, ptr, 8);
    ptr += 8;

    uint64_t weights_offset, weights_count;
    std::memcpy(&weights_offset, ptr, 8);
    ptr += 8;
    std::memcpy(&weights_count, ptr, 8);

    // Point directly into buffer - no copy!
    view.weights = std::span<const float>(
        reinterpret_cast<const float*>(data + 8 + weights_offset),
        weights_count
    );

    return view;
}
```

---

## Appendix A: Compile-Time Type Validation

Type signatures are used for **compile-time validation** to ensure sender and receiver agree on struct definitions. The signature does NOT appear in the wire format.

### Purpose

```cpp
// Define type signature as a compile-time constant
template<>
struct zmem_signature<Point> {
    static constexpr std::string_view value = "Point{x::f32,y::f32}";
};

// Both sender and receiver include the same signature definition
// Mismatches cause linker errors or static_assert failures
```

If struct definitions differ between sender and receiver codebases, validation fails at compile/link time, preventing silent data corruption.

---

## Appendix B: Complete Example

### Schema Definition

```cpp
// vec3.h
struct Vec3 {
    float x;
    float y;
    float z;
};

// Type signature: "Vec3{x::f32,y::f32,z::f32}"

static_assert(std::is_trivially_copyable_v<Vec3>);
static_assert(std::is_standard_layout_v<Vec3>);
static_assert(sizeof(Vec3) == 12);
static_assert(alignof(Vec3) == 4);
static_assert(offsetof(Vec3, x) == 0);
static_assert(offsetof(Vec3, y) == 4);
static_assert(offsetof(Vec3, z) == 8);
```

```cpp
// particle.h
struct Particle {
    uint64_t id;
    Vec3 position;
    Vec3 velocity;
    float mass;
};

// Type signature:
// "Particle{id::u64,position::Vec3{x::f32,y::f32,z::f32},velocity::Vec3{x::f32,y::f32,z::f32},mass::f32}"

static_assert(std::is_trivially_copyable_v<Particle>);
static_assert(std::is_standard_layout_v<Particle>);
static_assert(sizeof(Particle) == 40);
static_assert(alignof(Particle) == 8);
static_assert(offsetof(Particle, id) == 0);
static_assert(offsetof(Particle, position) == 8);
static_assert(offsetof(Particle, velocity) == 20);
static_assert(offsetof(Particle, mass) == 32);
```

### Wire Representation

A serialized `Particle`:

```
Offset  Size  Field
------  ----  -----
0       8     id (u64)
8       4     position.x (f32)
12      4     position.y (f32)
16      4     position.z (f32)
20      4     velocity.x (f32)
24      4     velocity.y (f32)
28      4     velocity.z (f32)
32      4     mass (f32)
36      4     [tail padding]
------
Total: 40 bytes (no header for fixed structs)
```

### Array Example

A serialized `std::vector<Vec3>` with 3 elements:

```
Type signature (compile-time only): "[Vec3{x::f32,y::f32,z::f32}]"

Offset  Size  Field
------  ----  -----
0       8     Count = 3
8       4     elements[0].x (f32)
12      4     elements[0].y (f32)
16      4     elements[0].z (f32)
20      4     elements[1].x (f32)
24      4     elements[1].y (f32)
28      4     elements[1].z (f32)
32      4     elements[2].x (f32)
36      4     elements[2].y (f32)
40      4     elements[2].z (f32)
------
Total: 44 bytes (8 count + 36 payload)
```

**C++ usage**:
```cpp
std::vector<Vec3> positions = {
    {1.0f, 2.0f, 3.0f},
    {4.0f, 5.0f, 6.0f},
    {7.0f, 8.0f, 9.0f}
};

// Serialize
auto buffer = serialize_array(positions);

// Deserialize (with copy)
std::vector<Vec3> restored;
deserialize_array(buffer.data(), buffer.size(), restored);

// Zero-copy view (no allocation!)
auto view = view_array<Vec3>(buffer.data(), buffer.size());
for (const auto& v : view) {
    // Access directly from buffer
}
```

