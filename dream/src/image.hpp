// Loader for `.dream` bytecode images.
//
// The container was designed so that loading is a slice, not a parse: every
// section is an array of fixed-size little-endian records and every edge is an
// index. So we mmap the file and point at it. The only work done up front is
// validation -- an image may come from anywhere, and the interpreter indexes
// these arrays without further checks, so every index has to be proven in
// range exactly once, here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dream {

// Must match the compiler's `ir::Op` in dreams/ir.dr.
// See docs/bytecode-format.md for the container.
enum class Op : uint8_t {
    Nop = 0,
    ConstInt = 1,
    ConstFloat = 2,
    ConstStr = 3,
    ConstChar = 4,
    ConstBool = 5,
    ConstAtom = 6,
    Unit = 7,
    Local = 8,
    Capture = 9,
    Global = 10,
    Builtin = 11,
    Field = 12,
    Apply = 13,
    If = 14,
    Block = 15,
    Bind = 16,
    MakeClosure = 17,
    MakeThunk = 18,
    Try = 19,
    Force = 20,
    Add = 21, Sub = 22, Mul = 23, Div = 24, Mod = 25,
    Eq = 26, Ne = 27, Lt = 28, Le = 29, Gt = 30, Ge = 31,
    And = 32, Or = 33,
    Neg = 34, Not = 35,
    MakeList = 36, MakeArray = 37, MakeMap = 38,
    Count
};

const char* op_name(Op op);

inline constexpr uint32_t NO_NODE = 0xFFFFFFFFu;

// Node flags, set by the compiler.
inline constexpr uint8_t F_STRICT = 1 << 0;
inline constexpr uint8_t F_IMPURE = 1 << 1;
inline constexpr uint8_t F_TAIL = 1 << 2;

// Function record flags.
inline constexpr uint16_t FN_IMPURE = 1 << 0;
inline constexpr uint16_t FN_REC = 1 << 1;
inline constexpr uint16_t FN_THUNK = 1 << 2;
inline constexpr uint16_t FN_GLOBAL_VALUE = 1 << 3;

// Global record flags.
inline constexpr uint32_t G_EXPORTED = 1 << 0;
inline constexpr uint32_t G_IMPURE = 1 << 1;

// Capture descriptors.
inline constexpr uint32_t CAP_FROM_CAPTURE = 0x80000000u;

#pragma pack(push, 1)
struct Node {
    uint8_t op;
    uint8_t flags;
    uint16_t aux;
    uint32_t a;
    uint32_t b;
    uint32_t c;
};
static_assert(sizeof(Node) == 16, "node records are 16 bytes on disk");

struct FuncRec {
    uint32_t name;
    uint32_t body;
    uint16_t arity;
    uint16_t flags;
    uint16_t slots;
    uint16_t n_captures;
    uint32_t captures_off;
    uint32_t span_start;
    uint32_t span_end;
    uint32_t reserved;
};
static_assert(sizeof(FuncRec) == 32, "function records are 32 bytes on disk");

struct GlobalRec {
    uint32_t name;
    uint32_t kind;  // 0 = function, 1 = module
    uint32_t target;
    uint32_t flags;
};
static_assert(sizeof(GlobalRec) == 16, "global records are 16 bytes on disk");

struct ImportRec {
    uint32_t path;
    uint32_t alias;
};

struct StrRec {
    uint32_t offset;
    uint32_t length;
};

struct SpanRec {
    uint32_t start;
    uint32_t end;
};

/// One module of a whole-program image. Compilation is whole-program, so an
/// image carries every module it needs and records which globals belong to
/// which -- for diagnostics, tooling, and reflection.
struct ModuleRec {
    uint32_t name;
    uint32_t source;
    uint32_t globals_start;
    uint32_t globals_count;
    uint32_t flags;
    uint32_t derives;
};
static_assert(sizeof(ModuleRec) == 24, "module records are 24 bytes on disk");
#pragma pack(pop)

/// Provided by the host rather than compiled from Dream source.
inline constexpr uint32_t M_NATIVE = 1 << 0;
/// Declares at least one virtual, so it is meant to be derived.
inline constexpr uint32_t M_ABSTRACT = 1 << 1;

enum GlobalKind : uint32_t { GLOBAL_FUNCTION = 0, GLOBAL_MODULE = 1 };

struct StringRef {
    const char* data;
    uint32_t len;
    std::string str() const { return std::string(data, len); }
    bool equals(const char* s) const;
};

class Image {
public:
    Image() = default;
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    /// Map a file and validate it. On failure returns false and fills `error`.
    bool load_file(const std::string& path, std::string& error);
    /// Validate an in-memory image. The bytes must outlive the Image.
    bool load_bytes(const uint8_t* data, size_t size, std::string& error);

    uint16_t version_major() const { return version_major_; }
    uint16_t version_minor() const { return version_minor_; }
    bool has_debug_info() const { return (flags_ & 1) != 0; }
    uint32_t entry() const { return entry_; }

    StringRef module_name() const { return str(module_name_); }
    StringRef source_name() const { return str(source_name_); }

    int64_t integer(uint32_t i) const { return ints_[i]; }
    double real(uint32_t i) const { return floats_[i]; }
    StringRef str(uint32_t i) const {
        return StringRef{blob_ + strs_[i].offset, strs_[i].length};
    }
    /// Atoms are interned separately from strings so they compare by index.
    StringRef atom_name(uint32_t i) const { return str(atoms_[i]); }
    uint32_t atom_string_index(uint32_t i) const { return atoms_[i]; }

    const Node& node(uint32_t i) const { return nodes_[i]; }
    uint32_t kid(uint32_t i) const { return kids_[i]; }
    const uint32_t* kids_at(uint32_t off) const { return kids_ + off; }
    const FuncRec& func(uint32_t i) const { return funcs_[i]; }
    const GlobalRec& global(uint32_t i) const { return globals_[i]; }
    const ImportRec& import(uint32_t i) const { return imports_[i]; }
    const ModuleRec& module(uint32_t i) const { return modules_[i]; }
    uint32_t module_count() const { return n_modules_; }
    /// The module a global belongs to, or -1.
    int module_of_global(uint32_t global_index) const;
    /// Source span for a node, or {0,0} when the image carries no debug info.
    SpanRec span(uint32_t node_index) const;

    uint32_t int_count() const { return n_ints_; }
    uint32_t float_count() const { return n_floats_; }
    uint32_t string_count() const { return n_strs_; }
    uint32_t atom_count() const { return n_atoms_; }
    uint32_t node_count() const { return n_nodes_; }
    uint32_t kid_count() const { return n_kids_; }
    uint32_t func_count() const { return n_funcs_; }
    uint32_t global_count() const { return n_globals_; }
    uint32_t import_count() const { return n_imports_; }

    /// Index of the global with this name, or -1.
    int find_global(const char* name) const;

private:
    bool parse(std::string& error);
    bool validate(std::string& error);

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool owns_mapping_ = false;
    /// What to hand back to munmap. A shebang line is skipped by advancing
    /// `data_`, which then no longer points at the start of the mapping -- and
    /// munmap takes the address it was given, page-aligned, or nothing.
    void* mapping_ = nullptr;
    size_t mapping_size_ = 0;

    uint16_t version_major_ = 0, version_minor_ = 0;
    uint32_t flags_ = 0, module_name_ = 0, source_name_ = 0, entry_ = NO_NODE;

    const int64_t* ints_ = nullptr;          uint32_t n_ints_ = 0;
    const double* floats_ = nullptr;         uint32_t n_floats_ = 0;
    const StrRec* strs_ = nullptr;           uint32_t n_strs_ = 0;
    const char* blob_ = nullptr;             uint32_t n_blob_ = 0;
    const uint32_t* atoms_ = nullptr;        uint32_t n_atoms_ = 0;
    const Node* nodes_ = nullptr;            uint32_t n_nodes_ = 0;
    const uint32_t* kids_ = nullptr;         uint32_t n_kids_ = 0;
    const FuncRec* funcs_ = nullptr;         uint32_t n_funcs_ = 0;
    const GlobalRec* globals_ = nullptr;     uint32_t n_globals_ = 0;
    const ImportRec* imports_ = nullptr;     uint32_t n_imports_ = 0;
    const SpanRec* spans_ = nullptr;         uint32_t n_spans_ = 0;
    const ModuleRec* modules_ = nullptr;     uint32_t n_modules_ = 0;
};

}  // namespace dream
