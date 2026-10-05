# Binary layouts

`std.binary` reads and writes formats defined by an application or a protocol.
A layout specifies both directions. It is an ordinary Dream value: compose it,
name it, pass it to a function, or use it under `comp`.

`std.wire` remains the native serializer for Dream values. Its tags, byte format,
API and framing are unchanged. `std.codec` continues to supply framing for wire,
JSON and other derived formats. Use `binary` for a particular byte layout, such
as a file header, network packet, device register or another language's struct.

## A packet

```dream
import std.binary as b;

let packet = b.record [
    b.magic "DP",                  // checked and emitted, absent from the map
    [:version, b.where (fn n -> n == 1) "unsupported version" b.u8],
    [:flags, b.u8],
    [:id, b.u32be],
    [:payload, b.blob b.u16be 4096],
];

let message = %{ :version => 1, :flags => 0, :id => 42, :payload => "hello" };
let encoded = b.encode packet message;  // [:ok, bytes] or [:error, problem]
// Given the bytes from encoded:
// b.decode packet bytes               // [:ok, message]
```

No implicit machine byte order, integer truncation, skipped trailing data or
padding is involved. `decode` requires the layout to consume the entire input.
`decode_prefix` is available when more fields follow. Ordinary strings carry
bytes; there is no new buffer type and no implicit UTF-8 conversion.

## Integers and floats

| Layout | Meaning |
|---|---|
| `uint order width` | Unsigned integer, **width in bytes**, `order` is `:be` or `:le` |
| `sint order width` | Signed two's-complement integer, width in bytes |
| `u8`, `i8` | One-byte integers |
| `u16be`, `u16le`, `i16be`, `i16le` | 16-bit integers |
| `u24be`, `u24le`, `i24be`, `i24le` | 24-bit integers |
| `u32be`, `u32le`, `i32be`, `i32le` | 32-bit integers |
| `u64be`, `u64le`, `i64be`, `i64le` | 64-bit integers |
| `u128be`, `u128le`, `i128be`, `i128le` | 128-bit integers |
| `float16 order`, `float32 order`, `float64 order` | IEEE binary16, binary32, binary64 |
| `f16be`, `f16le`, `f32be`, `f32le`, `f64be`, `f64le` | Float aliases |
| `boolean` | Exactly one byte, 0 or 1 |

Integer widths are 1 through 65,536 bytes. Dream integers retain every bit,
including unsigned 64-bit and larger values. Writers reject floats, negative
unsigned values and values outside the field's range. Intentional wrapping is
provided separately by `std.binary.word`.

Float writers accept integers or floats. Integers first convert to binary64.
Binary16 and binary32 then round to nearest, ties to even, including subnormal
values. Overflow becomes signed infinity, signed zero survives, and NaNs are
written as a canonical quiet NaN with the input sign. Binary64 preserves the
runtime representation. `std.binary.ieee` is implementation support, not an
additional public format API.

## Variable integers

| Layout | Meaning |
|---|---|
| `varuint max_bits` | Unsigned LEB128, bounded by the given bit width |
| `zigzag max_bits` | ZigZag signed mapping followed by unsigned LEB128 |
| `sleb128 max_bits` | Signed LEB128 with sign extension |
| `uvarint` | `varuint 64` |
| `svarint` | `zigzag 64` |

These are different encodings: `-1` is `01` with ZigZag and `7f` with signed
LEB128. All accept widths from 1 through 524,288 bits and reject redundant
encodings and values beyond the chosen range. A missing continuation byte is
`:truncated`; an encoding that cannot fit the width is `:out_of_range`.

## Byte strings and structure

| Layout | Value and behavior |
|---|---|
| `bytes n` | Exactly n raw bytes |
| `rest` | All remaining bytes in the current bounded reader |
| `terminated delimiter limit` | Bytes followed by a delimiter; limit excludes the delimiter |
| `cstring limit` | NUL-terminated bytes |
| `constant codec expected` | Requires/emits expected; its value is unit |
| `magic bytes` | A constant byte sequence |
| `padding count byte` | Checks/emits count copies of byte; its value is unit |
| `tuple layouts` | A heterogeneous list, in layout order |
| `repeat count item` | A fixed-count homogeneous list |
| `record fields` | Ordered `[key, layout]` entries produce a map; bare constant/padding layouts are omitted |
| `vector prefix limit item` | A count prefix followed by that many items |
| `sized prefix limit body` | A byte-length prefix and an exactly consumed body |
| `blob prefix limit` | A bounded, byte-length-prefixed string |
| `tagged prefix cases` | `[tag, payload]`; cases is a map from tags to layouts |
| `dependent header choose` | `[header, payload]`; choose header returns the payload layout |
| `checksummed count digest body` | Body followed by count digest bytes; value is the body value |

Records reject missing keys, extra keys and duplicate layout keys. This catches
misspellings rather than silently losing information. Constants and padding can
be named if the application wants their unit values in the map.

Vectors bound the number of elements; sized sections and blobs bound bytes.
Limits are explicit and checked against a decoded prefix before reading its
body. `vector` also stops inspecting an input list after limit + 1 elements,
including infinite lists. A sized body cannot read into the following field,
and unused bytes inside it are an error. Prefixes should be non-negative integer
layouts such as `u16be` or `uvarint`.

A terminated writer rejects delimiters embedded in the payload, including an
overlap between the payload suffix and delimiter. This preserves round trips
for multi-byte delimiters as well as NUL. Zero and invalid UTF-8 bytes are fine
in all other raw byte fields.

`checksummed` takes a pure bytes-to-bytes function, for example `crypto.sha256`.
The reader compares its result using `crypto.equal`. The body layout must find
its own end: use a fixed-width body or `sized`/`blob`, rather than an unbounded
`rest`, which would consume the digest as well.

```dream
let choices = b.tagged b.u8 %{
    1 => b.u32be,
    2 => b.blob b.u16le 1024,
};
// b.encode choices [2, "hello"]

let bounded = b.sized b.u16be 4096 (b.record [
    [:id, b.u32be],
    [:data, b.rest],
]);
```

## Bit fields and alignment

```dream
let flags = b.record [
    [:version, b.bits :msb 3],
    [:urgent, b.flag],
    [:delta, b.signed_bits :msb 9],
    b.byte_align 0,
    [:length, b.u16be],
];
```

`bits order width` is unsigned; `signed_bits order width` is signed two's
complement. Width is **bits**, from 1 through 524,288. Physical traversal always
runs from bit 7 to bit 0 in each byte, crossing byte boundaries as needed.
`:msb` reads/writes the most significant value bit first; `:lsb` reads/writes the
least significant value bit first. This is separate from integer byte order.
`flag` is one bit represented as a boolean.

Byte-oriented fields reject an unaligned cursor. `byte_align 0` or `byte_align 1`
checks/writes the remaining bits of the current byte. `align boundary byte`
checks/writes bytes up to a multiple of boundary; it first requires byte
alignment. Alignment is relative to the enclosing reader/section start, so a
sized section can be encoded independently. Neither alignment layout consumes
any padding when already aligned. Both take unit as their value and may appear
bare in a record.

Encoding cannot finish partway through a byte. Add explicit `byte_align` rather
than relying on undocumented padding.

## Conversion and composition

| Function | Purpose |
|---|---|
| `where predicate message codec` | Validate in both directions |
| `convert decode_value encode_value codec` | Bidirectional conversion; callbacks return `[:ok, v]` or `[:error, message]` |
| `named name codec` | Add a name or index to error paths |
| `lazy make` | Delay construction of a recursive layout; make receives unit |
| `layout read write` | Construct a custom layout from reader and builder callbacks |

Conversions run only after their underlying reader succeeds. Writer conversion
runs before any underlying bytes are produced. User callbacks remain ordinary
Dream functions: their own exceptions or nontermination are not caught by pure
layout operations.

## Readers, builders and patching

| Function/value | Result |
|---|---|
| `reader data` | Reader over the whole string |
| `reader_at offset count data` | Checked, bounded reader over a byte range |
| `read codec reader` | `[:ok, [value, next_reader]]` or error |
| `decode_prefix codec data` | Read once from a new reader |
| `read_at codec offset data` | Read from a byte offset |
| `position reader` | Absolute bit position |
| `remaining_bits reader`, `remaining_bytes reader` | Remaining bits, or whole bytes |
| `aligned reader` | Whether the cursor is at a byte boundary |
| `builder` | Empty immutable builder |
| `write codec value builder` | Result containing the next builder |
| `written_bits builder`, `written_bytes builder` | Written bits, or complete bytes |
| `finish builder` | Result containing the output string; rejects partial bytes |
| `encode codec value` | Write to a new builder and finish |
| `decode codec data` | Decode exactly one complete input |
| `patch codec offset value data` | Replace encoded bytes without resizing data |
| `reverse_bytes data` | Reverse raw bytes, without interpreting UTF-8 |

A retained reader or builder is a checkpoint and can be reused. These operations
have no hidden cursor or global state. Treat their representations as opaque;
use the public operations rather than manufacturing tuples.

Builders keep complete chunks in reverse order and join them once on finish.
Sized and checksummed bodies are built separately and materialized to obtain
their length or digest. This is not a zero-copy guarantee: byte slices and
nested section outputs may allocate. The implementation uses Dream functions
and the existing VM primitives; it adds no native module or platform dependency.

## Errors

Pure operations return `[:error, problem]`, where problem contains:

- `:kind`: `:truncated`, `:trailing_data`, `:unaligned`, `:out_of_range`,
  `:type_error`, `:invalid_argument`, `:invalid_value`, `:noncanonical`,
  `:unknown_tag`, `:missing_field`, `:extra_field`, or `:checksum_mismatch`.
- `:offset`: zero-based byte position.
- `:bit_offset`: position within that byte, zero through seven.
- `:path`: outermost field first, including sequence indices.
- `:message`: explanation; `describe problem` formats all these fields.

Truncated fixed byte/bit fields also include `:needed_bits` and `:available_bits`.
Decoded positions remain absolute in the original input, including errors inside
bounded sections. Writer positions refer to the current builder; a separately
encoded sized/checksummed body's writer starts at zero. Field paths still name
its enclosing fields.

`:truncated` means a read ran out of input. At the outer stream boundary a
caller can retry with more bytes. Inside an already complete sized section it
means a malformed section: appending to the parent cannot extend that section.
Other errors describe a rejected value or encoding.
User-chosen layouts and custom callbacks are trusted program definitions; this
is validation of bytes and values, not a sandbox for hostile layout definitions.

## Word operations

`std.binary.word` supplies deliberate fixed-width operations. Each returns a
binary-style result. Widths are 1 through 524,288 bits.

| Function | Meaning |
|---|---|
| `unsigned width n` | Low width bits as a non-negative integer |
| `signed width n` | Low width bits as a signed integer |
| `rotate_left width count n`, `rotate_right width count n` | Counts wrap modulo width; negative counts reverse direction |
| `extract offset width n` | A field; offset zero is the least significant bit |
| `replace offset width field n` | Replace a field while retaining every other bit |
| `popcount width n` | Number of set bits |
| `reverse width n` | Reverse the low width bits |
| `leading_zeros width n`, `trailing_zeros width n` | Zero input returns width |

`replace` rejects fields that do not fit, and limits offsets to 524,288 bits.
Other word operations first take the low width bits where appropriate. Negative
integers use the language's infinite two's-complement semantics.

## IO without a second framing system

`std.binary.io` uses callbacks or existing IO handles:

```dream
// source! maximum -> up to maximum bytes; empty string means EOF.
// sink! bytes -> number of bytes written.
bio.read_exact_with! count source!
bio.read_with! layout count source!
bio.write_with! layout value sink!

bio.read! layout count handle
bio.write! layout value handle
```

These return results. Short reads and writes are handled; read chunks are joined
once. Early EOF is `:truncated`. A reader returning more than requested, a writer
making no progress, or a callback raising produces `:io_error`. Encoding is
validated before writing. An IO failure may have written a prefix; the caller
owns recovery. Callers should bound count according to their protocol.

Existing wire framing can be described without changing it:

```dream
let frame_bytes = b.blob b.u32le 1048576;
// b.encode frame_bytes (wire.encode value) has the bytes of wire.frame value.
// The explicit limit is this application's policy.
```

## Tests

`just test-binary` runs the API suite and independent Python reference tests in
interpreter and JIT modes. The reference test includes every finite binary16
representation, signed zeros and infinities, randomized binary32/64 conversions,
and integer widths through 264 bits. API tests cover malformed input, boundaries,
field paths, compile-time use, short IO, checksums and wire interoperability.

The API suite is included in `std.all`, and the reference suite is registered in
both `just test-std` and the workspace's `mind test` checks.
