//! Compiler IR: a flat arena of fixed-size execution-tree nodes.
//!
//! The whole point of this representation is load speed. Every node is a
//! 16-byte record and every edge is a `u32` index into the same arena, so the
//! `NODE` section of a bytecode file is loadable by slicing the bytes — there
//! are no pointers to relocate and no tree to rebuild. Variadic children
//! (block statements, call arguments, list elements) live in a side `KIDS`
//! pool addressed by `(offset, count)`.
//!
//! Because the VM evaluates lazily, the tree *is* the program: a node is
//! forced on demand, and the `STRICT` flag marks the points where evaluation
//! order is observable (effects sequenced inside an impure block).

use std::collections::HashMap;

// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Op {
    Nop = 0,

    // Leaves. `a` indexes the matching constant table unless noted.
    ConstInt = 1,
    ConstFloat = 2,
    ConstStr = 3,
    /// `a` is the Unicode scalar value directly.
    ConstChar = 4,
    /// `a` is 0 or 1.
    ConstBool = 5,
    ConstAtom = 6,
    Unit = 7,

    // References. `a` is a slot / capture index / global index / builtin id.
    Local = 8,
    Capture = 9,
    Global = 10,
    Builtin = 11,

    /// `a` = object node, `b` = string constant naming the field.
    Field = 12,
    /// `a` = callee node, `b` = kids offset, `c` = argument count.
    Apply = 13,
    /// `a` = cond, `b` = then, `c` = else (`NO_NODE` when absent).
    If = 14,
    /// `a` = kids offset, `b` = statement count. Value is the last statement.
    Block = 15,
    /// `a` = local slot, `b` = value node. Binds lazily inside a block.
    Bind = 16,
    /// `a` = function index; captures come from that function's descriptor.
    MakeClosure = 17,
    /// `a` = function index of a generated 0-arity thunk.
    MakeThunk = 18,
    /// `a` = body, `b` = handler, `c` = local slot holding the caught error.
    Try = 19,
    /// `a` = node to force to weak head normal form.
    Force = 20,

    Add = 21,
    Sub = 22,
    Mul = 23,
    Div = 24,
    Mod = 25,
    Eq = 26,
    Ne = 27,
    Lt = 28,
    Le = 29,
    Gt = 30,
    Ge = 31,
    /// Short-circuiting; `b` is only forced when needed.
    And = 32,
    Or = 33,
    Neg = 34,
    Not = 35,

    /// `a` = kids offset, `b` = element count.
    MakeList = 36,
    MakeArray = 37,
    /// `a` = kids offset, `b` = pair count; kids are `k0, v0, k1, v1, ...`.
    MakeMap = 38,
}

impl Op {
    pub fn name(self) -> &'static str {
        match self {
            Op::Nop => "nop",
            Op::ConstInt => "int",
            Op::ConstFloat => "float",
            Op::ConstStr => "str",
            Op::ConstChar => "char",
            Op::ConstBool => "bool",
            Op::ConstAtom => "atom",
            Op::Unit => "unit",
            Op::Local => "local",
            Op::Capture => "capture",
            Op::Global => "global",
            Op::Builtin => "builtin",
            Op::Field => "field",
            Op::Apply => "apply",
            Op::If => "if",
            Op::Block => "block",
            Op::Bind => "bind",
            Op::MakeClosure => "closure",
            Op::MakeThunk => "thunk",
            Op::Try => "try",
            Op::Force => "force",
            Op::Add => "add",
            Op::Sub => "sub",
            Op::Mul => "mul",
            Op::Div => "div",
            Op::Mod => "mod",
            Op::Eq => "eq",
            Op::Ne => "ne",
            Op::Lt => "lt",
            Op::Le => "le",
            Op::Gt => "gt",
            Op::Ge => "ge",
            Op::And => "and",
            Op::Or => "or",
            Op::Neg => "neg",
            Op::Not => "not",
            Op::MakeList => "list",
            Op::MakeArray => "array",
            Op::MakeMap => "map",
        }
    }

    /// Decode an opcode byte. This runs over file data that may be untrusted,
    /// so it is a lookup rather than a transmute; `opcode_table_is_dense`
    /// guards the table against drifting out of step with the enum.
    pub fn from_u8(v: u8) -> Option<Op> {
        OPS.get(v as usize).copied()
    }
}

/// Every opcode, indexed by its discriminant.
static OPS: &[Op] = &[
    Op::Nop,
    Op::ConstInt,
    Op::ConstFloat,
    Op::ConstStr,
    Op::ConstChar,
    Op::ConstBool,
    Op::ConstAtom,
    Op::Unit,
    Op::Local,
    Op::Capture,
    Op::Global,
    Op::Builtin,
    Op::Field,
    Op::Apply,
    Op::If,
    Op::Block,
    Op::Bind,
    Op::MakeClosure,
    Op::MakeThunk,
    Op::Try,
    Op::Force,
    Op::Add,
    Op::Sub,
    Op::Mul,
    Op::Div,
    Op::Mod,
    Op::Eq,
    Op::Ne,
    Op::Lt,
    Op::Le,
    Op::Gt,
    Op::Ge,
    Op::And,
    Op::Or,
    Op::Neg,
    Op::Not,
    Op::MakeList,
    Op::MakeArray,
    Op::MakeMap,
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn opcode_table_is_dense() {
        for (i, op) in OPS.iter().enumerate() {
            assert_eq!(*op as usize, i, "`OPS` is out of step with the `Op` discriminants");
        }
        assert_eq!(Op::from_u8(OPS.len() as u8), None);
    }

    #[test]
    fn nodes_round_trip_through_bytes() {
        let n = Node { op: Op::Apply as u8, flags: F_STRICT | F_TAIL, aux: 7, a: 1, b: 2, c: 3 };
        let mut buf = Vec::new();
        n.write(&mut buf);
        assert_eq!(buf.len(), NODE_SIZE);
        assert_eq!(Node::read(&buf), n);
    }

    #[test]
    fn capture_descriptors_round_trip() {
        for from_capture in [false, true] {
            for index in [0u32, 1, 0x7FFF_FFFF] {
                assert_eq!(decode_capture(encode_capture(from_capture, index)), (from_capture, index));
            }
        }
    }
}

/// Sentinel for an absent node edge.
pub const NO_NODE: u32 = u32::MAX;

// ---- node flags ----
/// Must be forced where it appears; its effects are ordered.
pub const F_STRICT: u8 = 1 << 0;
/// Performs or may perform effects.
pub const F_IMPURE: u8 = 1 << 1;
/// In tail position of its enclosing function.
pub const F_TAIL: u8 = 1 << 2;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Node {
    pub op: u8,
    pub flags: u8,
    pub aux: u16,
    pub a: u32,
    pub b: u32,
    pub c: u32,
}

pub const NODE_SIZE: usize = 16;

impl Node {
    pub fn new(op: Op) -> Node {
        Node { op: op as u8, flags: 0, aux: 0, a: NO_NODE, b: NO_NODE, c: NO_NODE }
    }
    pub fn with(op: Op, a: u32, b: u32, c: u32) -> Node {
        Node { op: op as u8, flags: 0, aux: 0, a, b, c }
    }
    pub fn opcode(&self) -> Option<Op> {
        Op::from_u8(self.op)
    }
    pub fn write(&self, out: &mut Vec<u8>) {
        out.push(self.op);
        out.push(self.flags);
        out.extend_from_slice(&self.aux.to_le_bytes());
        out.extend_from_slice(&self.a.to_le_bytes());
        out.extend_from_slice(&self.b.to_le_bytes());
        out.extend_from_slice(&self.c.to_le_bytes());
    }
    pub fn read(b: &[u8]) -> Node {
        Node {
            op: b[0],
            flags: b[1],
            aux: u16::from_le_bytes([b[2], b[3]]),
            a: u32::from_le_bytes([b[4], b[5], b[6], b[7]]),
            b: u32::from_le_bytes([b[8], b[9], b[10], b[11]]),
            c: u32::from_le_bytes([b[12], b[13], b[14], b[15]]),
        }
    }
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/// Function record flags.
pub const FN_IMPURE: u16 = 1 << 0;
pub const FN_REC: u16 = 1 << 1;
/// A generated 0-arity thunk from `$( .. )` or a value-shaped `let`.
pub const FN_THUNK: u16 = 1 << 2;
/// A top-level value binding: force once and memoize.
pub const FN_GLOBAL_VALUE: u16 = 1 << 3;

/// How a closure captures one variable from its defining frame.
pub const CAP_PARENT_LOCAL: u32 = 0;
pub const CAP_PARENT_CAPTURE: u32 = 0x8000_0000;

pub fn encode_capture(from_capture: bool, index: u32) -> u32 {
    if from_capture { CAP_PARENT_CAPTURE | index } else { index }
}
pub fn decode_capture(v: u32) -> (bool, u32) {
    (v & CAP_PARENT_CAPTURE != 0, v & !CAP_PARENT_CAPTURE)
}

#[derive(Debug, Clone)]
pub struct Func {
    pub name: u32,
    pub body: u32,
    pub arity: u16,
    pub flags: u16,
    /// Frame size: parameters plus block-local bindings.
    pub slots: u16,
    pub n_captures: u16,
    /// Offset into the kids pool of `n_captures` capture descriptors.
    pub captures_off: u32,
    pub span_start: u32,
    pub span_end: u32,
}

pub const FUNC_SIZE: usize = 32;

// ---------------------------------------------------------------------------
// Globals and imports
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum GlobalKind {
    Function = 0,
    Module = 1,
}

#[derive(Debug, Clone)]
pub struct Global {
    pub name: u32,
    pub kind: GlobalKind,
    /// Function index, or import index for a module.
    pub target: u32,
    pub flags: u32,
}

pub const GLOBAL_SIZE: usize = 16;

pub const G_EXPORTED: u32 = 1 << 0;
pub const G_IMPURE: u32 = 1 << 1;

/// One module in the program. A Dream program is compiled whole, so an image
/// holds every module it needs; this records which globals belong to which.
#[derive(Debug, Clone)]
pub struct ModuleRec {
    pub name: u32,
    pub source: u32,
    pub globals_start: u32,
    pub globals_count: u32,
    pub flags: u32,
    /// The module this one derives, or `NO_NODE`.
    pub derives: u32,
}

pub const MODULE_SIZE: usize = 24;

/// Provided by the host rather than compiled from Dream source.
pub const M_NATIVE: u32 = 1 << 0;
/// Declares at least one virtual, so it is meant to be derived.
pub const M_ABSTRACT: u32 = 1 << 1;

#[derive(Debug, Clone)]
pub struct ImportEntry {
    /// Dotted path, e.g. `std.console`.
    pub path: u32,
    pub alias: u32,
}

pub const IMPORT_SIZE: usize = 8;

// ---------------------------------------------------------------------------
// Constant pool
// ---------------------------------------------------------------------------

#[derive(Debug, Default)]
pub struct ConstPool {
    pub ints: Vec<i64>,
    pub floats: Vec<u64>,
    pub strings: Vec<String>,
    /// Atom table: each entry is a string-table index. Atoms are interned
    /// separately so the VM can compare them by identity.
    pub atoms: Vec<u32>,

    int_map: HashMap<i64, u32>,
    float_map: HashMap<u64, u32>,
    string_map: HashMap<String, u32>,
    atom_map: HashMap<String, u32>,
}

impl ConstPool {
    pub fn int(&mut self, v: i64) -> u32 {
        intern(&mut self.int_map, &mut self.ints, v)
    }
    pub fn float(&mut self, v: f64) -> u32 {
        // Key on the bit pattern so `-0.0` and `NaN` round-trip exactly.
        intern(&mut self.float_map, &mut self.floats, v.to_bits())
    }
    pub fn string(&mut self, s: &str) -> u32 {
        if let Some(&i) = self.string_map.get(s) {
            return i;
        }
        let i = self.strings.len() as u32;
        self.strings.push(s.to_string());
        self.string_map.insert(s.to_string(), i);
        i
    }
    pub fn atom(&mut self, s: &str) -> u32 {
        if let Some(&i) = self.atom_map.get(s) {
            return i;
        }
        let str_idx = self.string(s);
        let i = self.atoms.len() as u32;
        self.atoms.push(str_idx);
        self.atom_map.insert(s.to_string(), i);
        i
    }
}

fn intern<T: std::hash::Hash + Eq + Copy>(
    map: &mut HashMap<T, u32>,
    vec: &mut Vec<T>,
    v: T,
) -> u32 {
    if let Some(&i) = map.get(&v) {
        return i;
    }
    let i = vec.len() as u32;
    vec.push(v);
    map.insert(v, i);
    i
}

// ---------------------------------------------------------------------------
// Whole program
// ---------------------------------------------------------------------------

#[derive(Debug, Default)]
pub struct Program {
    pub k: ConstPool,
    pub modules: Vec<ModuleRec>,
    pub nodes: Vec<Node>,
    /// Per-node source spans, parallel to `nodes`. Emitted only with debug info.
    pub spans: Vec<(u32, u32)>,
    pub kids: Vec<u32>,
    pub funcs: Vec<Func>,
    pub globals: Vec<Global>,
    pub imports: Vec<ImportEntry>,
    pub module_name: u32,
    pub source_name: u32,
    /// Function index of `main!`, or `NO_NODE`.
    pub entry: u32,
}

impl Program {
    pub fn push_node(&mut self, n: Node, span: (u32, u32)) -> u32 {
        let i = self.nodes.len() as u32;
        self.nodes.push(n);
        self.spans.push(span);
        i
    }
    pub fn push_kids(&mut self, items: &[u32]) -> u32 {
        let off = self.kids.len() as u32;
        self.kids.extend_from_slice(items);
        off
    }
    pub fn set_flag(&mut self, node: u32, flag: u8) {
        if node != NO_NODE {
            self.nodes[node as usize].flags |= flag;
        }
    }
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

/// Names resolved directly to `Op::Builtin` when not shadowed by a binding.
/// Order defines the builtin id, so only append.
pub const BUILTINS: &[&str] = &[
    "spawn!",   // 0: thunk -> thread
    "join!",    // 1: thread -> value
    "send!",    // 2: thread -> value -> unit
    "recv!",    // 3: unit -> value
    "self!",    // 4: unit -> thread
    "raise!",   // 5: value -> never
    "type_of",  // 6: value -> atom
    "to_string",// 7: value -> string
    "len",      // 8: list|array|map|string -> integer
];

pub fn builtin_id(name: &str) -> Option<u32> {
    BUILTINS.iter().position(|b| *b == name).map(|i| i as u32)
}
