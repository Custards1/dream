#include "image.hpp"

#include "builtins.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace dream {

namespace {

constexpr uint8_t MAGIC[8] = {'D', 'A', 'G', 'N', 'C', 'A', 'A', 'F'};
constexpr uint16_t SUPPORTED_MAJOR = 0;
constexpr size_t HEADER_SIZE = 32;
constexpr size_t SECTION_ENTRY_SIZE = 16;
// Deliberately not a constant here: the builtin table lives in builtins.cpp,
// and a second copy of its size is a copy that drifts. It did -- a builtin was
// appended and every image using it was rejected as malformed.
inline uint32_t builtin_limit() { return builtin_count(); }

constexpr uint32_t tag(const char s[5]) {
    return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) |
           (uint32_t(uint8_t(s[2])) << 16) | (uint32_t(uint8_t(s[3])) << 24);
}

inline uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

struct SectionInfo {
    uint32_t kind, offset, length, count;
};

/// The node indices a node points at. Shared by the validator's two walks so
/// they cannot disagree about what counts as a child.
void node_children(const Node& n, const uint32_t* kids, std::vector<uint32_t>& out) {
    auto push_kids = [&](uint32_t off, uint64_t count) {
        for (uint64_t i = 0; i < count; ++i) out.push_back(kids[off + i]);
    };
    switch (static_cast<Op>(n.op)) {
        case Op::Field: case Op::Force: case Op::Neg: case Op::Not:
            out.push_back(n.a);
            break;
        case Op::Apply:
            out.push_back(n.a);
            push_kids(n.b, n.c);
            break;
        case Op::If:
            out.push_back(n.a);
            out.push_back(n.b);
            out.push_back(n.c);
            break;
        case Op::Block: case Op::MakeList: case Op::MakeArray:
            push_kids(n.a, n.b);
            break;
        case Op::MakeMap:
            push_kids(n.a, uint64_t(n.b) * 2);
            break;
        case Op::Bind:
            out.push_back(n.b);
            break;
        case Op::Try:
            out.push_back(n.a);
            out.push_back(n.b);
            break;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::And: case Op::Or:
            out.push_back(n.a);
            out.push_back(n.b);
            break;
        default:
            break;
    }
}

}  // namespace

const char* op_name(Op op) {
    switch (op) {
        case Op::Nop: return "nop";
        case Op::ConstInt: return "int";
        case Op::ConstFloat: return "float";
        case Op::ConstStr: return "str";
        case Op::ConstChar: return "char";
        case Op::ConstBool: return "bool";
        case Op::ConstAtom: return "atom";
        case Op::Unit: return "unit";
        case Op::Local: return "local";
        case Op::Capture: return "capture";
        case Op::Global: return "global";
        case Op::Builtin: return "builtin";
        case Op::Field: return "field";
        case Op::Apply: return "apply";
        case Op::If: return "if";
        case Op::Block: return "block";
        case Op::Bind: return "bind";
        case Op::MakeClosure: return "closure";
        case Op::MakeThunk: return "thunk";
        case Op::Try: return "try";
        case Op::Force: return "force";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Mod: return "mod";
        case Op::Eq: return "eq";
        case Op::Ne: return "ne";
        case Op::Lt: return "lt";
        case Op::Le: return "le";
        case Op::Gt: return "gt";
        case Op::Ge: return "ge";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Neg: return "neg";
        case Op::Not: return "not";
        case Op::MakeList: return "list";
        case Op::MakeArray: return "array";
        case Op::MakeMap: return "map";
        default: return "<bad op>";
    }
}

bool StringRef::equals(const char* s) const {
    size_t n = std::strlen(s);
    return n == len && std::memcmp(data, s, n) == 0;
}

Image::~Image() {
    if (owns_mapping_ && data_) {
        ::munmap(const_cast<uint8_t*>(data_), size_);
    }
}

bool Image::load_file(const std::string& path, std::string& error) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        error = "cannot stat " + path;
        ::close(fd);
        return false;
    }
    if (st.st_size < static_cast<off_t>(HEADER_SIZE)) {
        error = path + " is too small to be a bytecode image";
        ::close(fd);
        return false;
    }
    void* p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        error = "cannot map " + path;
        return false;
    }
    data_ = static_cast<const uint8_t*>(p);
    size_ = static_cast<size_t>(st.st_size);
    owns_mapping_ = true;
    if (!parse(error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool Image::load_bytes(const uint8_t* data, size_t size, std::string& error) {
    data_ = data;
    size_ = size;
    owns_mapping_ = false;
    return parse(error);
}

bool Image::parse(std::string& error) {
    if (size_ < HEADER_SIZE) {
        error = "truncated header";
        return false;
    }
    if (std::memcmp(data_, MAGIC, 8) != 0) {
        error = "bad magic number (expected DAGNCAAF)";
        return false;
    }
    version_major_ = rd16(data_ + 8);
    version_minor_ = rd16(data_ + 10);
    if (version_major_ != SUPPORTED_MAJOR) {
        error = "unsupported bytecode version " + std::to_string(version_major_) + "." +
                std::to_string(version_minor_);
        return false;
    }
    flags_ = rd32(data_ + 12);
    module_name_ = rd32(data_ + 16);
    source_name_ = rd32(data_ + 20);
    entry_ = rd32(data_ + 24);
    uint32_t n_sections = rd32(data_ + 28);

    if (n_sections > 4096) {
        error = "implausible section count";
        return false;
    }
    if (HEADER_SIZE + size_t(n_sections) * SECTION_ENTRY_SIZE > size_) {
        error = "section table extends past end of file";
        return false;
    }

    for (uint32_t i = 0; i < n_sections; ++i) {
        const uint8_t* e = data_ + HEADER_SIZE + size_t(i) * SECTION_ENTRY_SIZE;
        SectionInfo s{rd32(e), rd32(e + 4), rd32(e + 8), rd32(e + 12)};
        if (size_t(s.offset) + s.length > size_) {
            error = "section extends past end of file";
            return false;
        }
        const uint8_t* base = data_ + s.offset;

        // A record array must actually hold `count` records.
        auto fits = [&](size_t record_size) {
            return size_t(s.count) * record_size <= s.length;
        };
        if (s.kind == tag("KINT")) {
            if (!fits(8)) { error = "KINT section is short"; return false; }
            ints_ = reinterpret_cast<const int64_t*>(base); n_ints_ = s.count;
        } else if (s.kind == tag("KFLT")) {
            if (!fits(8)) { error = "KFLT section is short"; return false; }
            floats_ = reinterpret_cast<const double*>(base); n_floats_ = s.count;
        } else if (s.kind == tag("KSTR")) {
            if (!fits(8)) { error = "KSTR section is short"; return false; }
            strs_ = reinterpret_cast<const StrRec*>(base); n_strs_ = s.count;
        } else if (s.kind == tag("SBLB")) {
            blob_ = reinterpret_cast<const char*>(base); n_blob_ = s.length;
        } else if (s.kind == tag("KATM")) {
            if (!fits(4)) { error = "KATM section is short"; return false; }
            atoms_ = reinterpret_cast<const uint32_t*>(base); n_atoms_ = s.count;
        } else if (s.kind == tag("NODE")) {
            if (!fits(sizeof(Node))) { error = "NODE section is short"; return false; }
            nodes_ = reinterpret_cast<const Node*>(base); n_nodes_ = s.count;
        } else if (s.kind == tag("KIDS")) {
            if (!fits(4)) { error = "KIDS section is short"; return false; }
            kids_ = reinterpret_cast<const uint32_t*>(base); n_kids_ = s.count;
        } else if (s.kind == tag("FUNC")) {
            if (!fits(sizeof(FuncRec))) { error = "FUNC section is short"; return false; }
            funcs_ = reinterpret_cast<const FuncRec*>(base); n_funcs_ = s.count;
        } else if (s.kind == tag("GLBL")) {
            if (!fits(sizeof(GlobalRec))) { error = "GLBL section is short"; return false; }
            globals_ = reinterpret_cast<const GlobalRec*>(base); n_globals_ = s.count;
        } else if (s.kind == tag("IMPT")) {
            if (!fits(sizeof(ImportRec))) { error = "IMPT section is short"; return false; }
            imports_ = reinterpret_cast<const ImportRec*>(base); n_imports_ = s.count;
        } else if (s.kind == tag("MODS")) {
            if (!fits(sizeof(ModuleRec))) { error = "MODS section is short"; return false; }
            modules_ = reinterpret_cast<const ModuleRec*>(base); n_modules_ = s.count;
        } else if (s.kind == tag("SPAN")) {
            if (!fits(sizeof(SpanRec))) { error = "SPAN section is short"; return false; }
            spans_ = reinterpret_cast<const SpanRec*>(base); n_spans_ = s.count;
        }
        // Unknown sections are skipped, as the format requires.
    }
    return validate(error);
}

int Image::module_of_global(uint32_t global_index) const {
    for (uint32_t i = 0; i < n_modules_; ++i) {
        const ModuleRec& m = modules_[i];
        if (global_index >= m.globals_start && global_index < m.globals_start + m.globals_count) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

SpanRec Image::span(uint32_t node_index) const {
    if (!spans_ || node_index >= n_spans_) return SpanRec{0, 0};
    return spans_[node_index];
}

int Image::find_global(const char* name) const {
    for (uint32_t i = 0; i < n_globals_; ++i) {
        if (str(globals_[i].name).equals(name)) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Validation
//
// The interpreter indexes these arrays with no further bounds checks, so every
// index in the image is proven in range here, once. Anything that fails leaves
// the image unusable rather than trusting it partially.
// ---------------------------------------------------------------------------

bool Image::validate(std::string& error) {
    auto fail = [&](const char* what) {
        error = what;
        return false;
    };

    if (module_name_ >= n_strs_ || source_name_ >= n_strs_) return fail("bad module or source name index");
    if (entry_ != NO_NODE && entry_ >= n_funcs_) return fail("entry point is not a valid function");

    for (uint32_t i = 0; i < n_strs_; ++i) {
        if (size_t(strs_[i].offset) + strs_[i].length > n_blob_) return fail("string extends past the string blob");
    }
    for (uint32_t i = 0; i < n_atoms_; ++i) {
        if (atoms_[i] >= n_strs_) return fail("atom names an out-of-range string");
    }
    for (uint32_t i = 0; i < n_imports_; ++i) {
        if (imports_[i].path >= n_strs_ || imports_[i].alias >= n_strs_) return fail("bad import string index");
    }
    for (uint32_t i = 0; i < n_globals_; ++i) {
        const GlobalRec& g = globals_[i];
        if (g.name >= n_strs_) return fail("bad global name index");
        if (g.kind == GLOBAL_FUNCTION) {
            if (g.target >= n_funcs_) return fail("global names an out-of-range function");
        } else if (g.kind == GLOBAL_MODULE) {
            if (g.target >= n_imports_) return fail("global names an out-of-range import");
        } else {
            return fail("unknown global kind");
        }
    }
    for (uint32_t i = 0; i < n_modules_; ++i) {
        const ModuleRec& m = modules_[i];
        if (m.name >= n_strs_ || m.source >= n_strs_) return fail("bad module name index");
        if (size_t(m.globals_start) + m.globals_count > n_globals_) {
            return fail("module's global range is out of bounds");
        }
        if (m.derives != NO_NODE && m.derives >= n_modules_) {
            return fail("module derives an out-of-range module");
        }
    }
    for (uint32_t i = 0; i < n_funcs_; ++i) {
        const FuncRec& f = funcs_[i];
        if (f.name >= n_strs_) return fail("bad function name index");
        if (f.body >= n_nodes_) return fail("function body is not a valid node");
        if (size_t(f.captures_off) + f.n_captures > n_kids_) return fail("capture list extends past the kids pool");
    }
    for (uint32_t i = 0; i < n_kids_; ++i) {
        (void)i;  // Kid meaning depends on the referring node; checked below.
    }

    // Structural pass: every operand of every node must be in range.
    auto node_ok = [&](uint32_t n) { return n < n_nodes_; };
    auto kids_ok = [&](uint32_t off, uint64_t count) {
        if (uint64_t(off) + count > n_kids_) return false;
        for (uint64_t k = 0; k < count; ++k) {
            if (!node_ok(kids_[off + k])) return false;
        }
        return true;
    };

    for (uint32_t i = 0; i < n_nodes_; ++i) {
        const Node& n = nodes_[i];
        if (n.op >= static_cast<uint8_t>(Op::Count)) return fail("unknown opcode");
        switch (static_cast<Op>(n.op)) {
            case Op::ConstInt: if (n.a >= n_ints_) return fail("bad integer constant index"); break;
            case Op::ConstFloat: if (n.a >= n_floats_) return fail("bad float constant index"); break;
            case Op::ConstStr: if (n.a >= n_strs_) return fail("bad string constant index"); break;
            case Op::ConstAtom: if (n.a >= n_atoms_) return fail("bad atom index"); break;
            case Op::ConstChar: if (n.a > 0x10FFFF) return fail("char constant is not a Unicode scalar value"); break;
            case Op::ConstBool: if (n.a > 1) return fail("bool constant is neither 0 nor 1"); break;
            case Op::Global: if (n.a >= n_globals_) return fail("bad global index"); break;
            case Op::Builtin: if (n.a >= builtin_limit()) return fail("unknown builtin id"); break;
            case Op::MakeClosure:
            case Op::MakeThunk: if (n.a >= n_funcs_) return fail("bad function index"); break;
            case Op::Field:
                if (!node_ok(n.a)) return fail("bad field object node");
                if (n.b >= n_strs_) return fail("bad field name index");
                break;
            case Op::Apply:
                if (!node_ok(n.a)) return fail("bad callee node");
                if (!kids_ok(n.b, n.c)) return fail("bad argument list");
                break;
            case Op::If:
                if (!node_ok(n.a) || !node_ok(n.b)) return fail("bad if branch");
                if (n.c != NO_NODE && !node_ok(n.c)) return fail("bad else branch");
                break;
            case Op::Block:
            case Op::MakeList:
            case Op::MakeArray:
                if (!kids_ok(n.a, n.b)) return fail("bad child list");
                break;
            case Op::MakeMap:
                if (!kids_ok(n.a, uint64_t(n.b) * 2)) return fail("bad map entry list");
                break;
            case Op::Bind: if (!node_ok(n.b)) return fail("bad bind value"); break;
            case Op::Try:
                if (!node_ok(n.a) || !node_ok(n.b)) return fail("bad try body or handler");
                break;
            case Op::Force: case Op::Neg: case Op::Not:
                if (!node_ok(n.a)) return fail("bad unary operand");
                break;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::And: case Op::Or:
                if (!node_ok(n.a) || !node_ok(n.b)) return fail("bad binary operand");
                break;
            default: break;
        }
    }

    // Acyclicity. An execution tree that points back at itself is malformed,
    // and nothing above would notice: every index is in range. The interpreter
    // would merely spin, but the disassembler walks the tree recursively, so a
    // cycle in an untrusted image would overflow the stack.
    {
        enum Mark : uint8_t { White = 0, Grey = 1, Black = 2 };
        std::vector<uint8_t> mark(n_nodes_, White);
        std::vector<uint32_t> kids_of;
        for (uint32_t root = 0; root < n_nodes_; ++root) {
            if (mark[root] != White) continue;
            std::vector<std::pair<uint32_t, uint32_t>> stack;
            stack.push_back({root, 0});
            mark[root] = Grey;
            while (!stack.empty()) {
                auto [ni, child] = stack.back();
                stack.pop_back();
                kids_of.clear();
                node_children(nodes_[ni], kids_, kids_of);
                if (child >= kids_of.size()) {
                    mark[ni] = Black;
                    continue;
                }
                stack.push_back({ni, child + 1});
                uint32_t c = kids_of[child];
                if (c == NO_NODE || c >= n_nodes_) continue;
                if (mark[c] == Grey) return fail("the execution tree contains a cycle");
                if (mark[c] == White) {
                    mark[c] = Grey;
                    stack.push_back({c, 0});
                }
            }
        }
    }

    // Slot pass: `local` and `capture` indices are only meaningful against the
    // function whose body reaches them, so walk each body. A generation stamp
    // avoids reclearing the visited map, and the explicit worklist means a
    // maliciously deep or cyclic image cannot blow the C++ stack.
    std::vector<uint32_t> seen(n_nodes_, 0);
    std::vector<uint32_t> work;
    for (uint32_t fi = 0; fi < n_funcs_; ++fi) {
        const FuncRec& f = funcs_[fi];
        const uint32_t gen = fi + 1;
        work.clear();
        work.push_back(f.body);
        while (!work.empty()) {
            uint32_t ni = work.back();
            work.pop_back();
            if (ni == NO_NODE || ni >= n_nodes_ || seen[ni] == gen) continue;
            seen[ni] = gen;
            const Node& n = nodes_[ni];
            switch (static_cast<Op>(n.op)) {
                case Op::Local:
                    if (n.a >= f.slots) return fail("local slot is outside the function frame");
                    break;
                case Op::Capture:
                    if (n.a >= f.n_captures) return fail("capture index is outside the capture list");
                    break;
                case Op::Bind:
                    if (n.a >= f.slots) return fail("bind slot is outside the function frame");
                    work.push_back(n.b);
                    break;
                case Op::Try:
                    if (n.c >= f.slots) return fail("catch slot is outside the function frame");
                    work.push_back(n.a);
                    work.push_back(n.b);
                    break;
                case Op::Apply:
                    work.push_back(n.a);
                    for (uint32_t k = 0; k < n.c; ++k) work.push_back(kids_[n.b + k]);
                    break;
                case Op::Block:
                case Op::MakeList:
                case Op::MakeArray:
                    for (uint32_t k = 0; k < n.b; ++k) work.push_back(kids_[n.a + k]);
                    break;
                case Op::MakeMap:
                    for (uint32_t k = 0; k < n.b * 2; ++k) work.push_back(kids_[n.a + k]);
                    break;
                case Op::If:
                    work.push_back(n.a);
                    work.push_back(n.b);
                    work.push_back(n.c);
                    break;
                case Op::Field:
                case Op::Force: case Op::Neg: case Op::Not:
                    work.push_back(n.a);
                    break;
                case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
                case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
                case Op::And: case Op::Or:
                    work.push_back(n.a);
                    work.push_back(n.b);
                    break;
                default:
                    break;
            }
        }
        // Capture descriptors are read against the *parent* frame at closure
        // creation, so they are bounds-checked there rather than here.
    }
    return true;
}

}  // namespace dream
