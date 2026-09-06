//! Bytecode container writer.
//!
//! Layout:
//!
//! ```text
//!   Header      32 bytes
//!   Section     16 bytes x section_count   (kind, offset, byte_len, count)
//!   Sections    each 8-byte aligned, in table order
//! ```
//!
//! Every section is a plain array of fixed-size little-endian records, and all
//! cross-references are indices rather than offsets. A loader can therefore
//! `mmap` the file, slice `NODE` and `KIDS`, and start forcing the execution
//! tree without a parse or a relocation pass — which is the requirement that
//! drove the whole design.

use crate::ir::*;

/// `DAGNCAAF` as raw ASCII bytes.
pub const MAGIC: [u8; 8] = *b"DAGNCAAF";
pub const VERSION_MAJOR: u16 = 0;
pub const VERSION_MINOR: u16 = 1;

pub const HEADER_SIZE: usize = 32;
pub const SECTION_ENTRY_SIZE: usize = 16;

/// The `SPAN` section is present.
pub const FLAG_DEBUG: u32 = 1 << 0;

pub const fn tag(s: &[u8; 4]) -> u32 {
    u32::from_le_bytes(*s)
}

pub const T_KINT: u32 = tag(b"KINT");
pub const T_KFLT: u32 = tag(b"KFLT");
pub const T_KSTR: u32 = tag(b"KSTR");
pub const T_SBLB: u32 = tag(b"SBLB");
pub const T_KATM: u32 = tag(b"KATM");
pub const T_NODE: u32 = tag(b"NODE");
pub const T_KIDS: u32 = tag(b"KIDS");
pub const T_FUNC: u32 = tag(b"FUNC");
pub const T_GLBL: u32 = tag(b"GLBL");
pub const T_IMPT: u32 = tag(b"IMPT");
pub const T_SPAN: u32 = tag(b"SPAN");
pub const T_MODS: u32 = tag(b"MODS");

pub fn tag_name(t: u32) -> String {
    String::from_utf8_lossy(&t.to_le_bytes()).into_owned()
}

struct Section {
    kind: u32,
    body: Vec<u8>,
    count: u32,
}

pub fn emit(prog: &Program, debug: bool) -> Vec<u8> {
    let mut sections: Vec<Section> = Vec::new();

    // --- constants ---
    let mut b = Vec::with_capacity(prog.k.ints.len() * 8);
    for v in &prog.k.ints {
        b.extend_from_slice(&v.to_le_bytes());
    }
    sections.push(Section { kind: T_KINT, count: prog.k.ints.len() as u32, body: b });

    let mut b = Vec::with_capacity(prog.k.floats.len() * 8);
    for v in &prog.k.floats {
        b.extend_from_slice(&v.to_le_bytes());
    }
    sections.push(Section { kind: T_KFLT, count: prog.k.floats.len() as u32, body: b });

    // Strings are (offset, len) pairs into one contiguous UTF-8 blob.
    let mut table = Vec::with_capacity(prog.k.strings.len() * 8);
    let mut blob: Vec<u8> = Vec::new();
    for s in &prog.k.strings {
        table.extend_from_slice(&(blob.len() as u32).to_le_bytes());
        table.extend_from_slice(&(s.len() as u32).to_le_bytes());
        blob.extend_from_slice(s.as_bytes());
    }
    sections.push(Section { kind: T_KSTR, count: prog.k.strings.len() as u32, body: table });
    sections.push(Section { kind: T_SBLB, count: blob.len() as u32, body: blob });

    let mut b = Vec::with_capacity(prog.k.atoms.len() * 4);
    for v in &prog.k.atoms {
        b.extend_from_slice(&v.to_le_bytes());
    }
    sections.push(Section { kind: T_KATM, count: prog.k.atoms.len() as u32, body: b });

    // --- execution trees ---
    let mut b = Vec::with_capacity(prog.nodes.len() * NODE_SIZE);
    for n in &prog.nodes {
        n.write(&mut b);
    }
    sections.push(Section { kind: T_NODE, count: prog.nodes.len() as u32, body: b });

    let mut b = Vec::with_capacity(prog.kids.len() * 4);
    for v in &prog.kids {
        b.extend_from_slice(&v.to_le_bytes());
    }
    sections.push(Section { kind: T_KIDS, count: prog.kids.len() as u32, body: b });

    // --- functions ---
    let mut b = Vec::with_capacity(prog.funcs.len() * FUNC_SIZE);
    for f in &prog.funcs {
        b.extend_from_slice(&f.name.to_le_bytes());
        b.extend_from_slice(&f.body.to_le_bytes());
        b.extend_from_slice(&f.arity.to_le_bytes());
        b.extend_from_slice(&f.flags.to_le_bytes());
        b.extend_from_slice(&f.slots.to_le_bytes());
        b.extend_from_slice(&f.n_captures.to_le_bytes());
        b.extend_from_slice(&f.captures_off.to_le_bytes());
        b.extend_from_slice(&f.span_start.to_le_bytes());
        b.extend_from_slice(&f.span_end.to_le_bytes());
        b.extend_from_slice(&0u32.to_le_bytes()); // reserved
    }
    sections.push(Section { kind: T_FUNC, count: prog.funcs.len() as u32, body: b });

    // --- linkage ---
    let mut b = Vec::with_capacity(prog.globals.len() * GLOBAL_SIZE);
    for g in &prog.globals {
        b.extend_from_slice(&g.name.to_le_bytes());
        b.extend_from_slice(&(g.kind as u32).to_le_bytes());
        b.extend_from_slice(&g.target.to_le_bytes());
        b.extend_from_slice(&g.flags.to_le_bytes());
    }
    sections.push(Section { kind: T_GLBL, count: prog.globals.len() as u32, body: b });

    let mut b = Vec::with_capacity(prog.imports.len() * IMPORT_SIZE);
    for i in &prog.imports {
        b.extend_from_slice(&i.path.to_le_bytes());
        b.extend_from_slice(&i.alias.to_le_bytes());
    }
    sections.push(Section { kind: T_IMPT, count: prog.imports.len() as u32, body: b });

    let mut b = Vec::with_capacity(prog.modules.len() * MODULE_SIZE);
    for m in &prog.modules {
        b.extend_from_slice(&m.name.to_le_bytes());
        b.extend_from_slice(&m.source.to_le_bytes());
        b.extend_from_slice(&m.globals_start.to_le_bytes());
        b.extend_from_slice(&m.globals_count.to_le_bytes());
        b.extend_from_slice(&m.flags.to_le_bytes());
        b.extend_from_slice(&m.derives.to_le_bytes());
    }
    sections.push(Section { kind: T_MODS, count: prog.modules.len() as u32, body: b });

    if debug {
        let mut b = Vec::with_capacity(prog.spans.len() * 8);
        for (s, e) in &prog.spans {
            b.extend_from_slice(&s.to_le_bytes());
            b.extend_from_slice(&e.to_le_bytes());
        }
        sections.push(Section { kind: T_SPAN, count: prog.spans.len() as u32, body: b });
    }

    // --- lay out ---
    let mut out = Vec::new();
    let flags = if debug { FLAG_DEBUG } else { 0 };
    out.extend_from_slice(&MAGIC);
    out.extend_from_slice(&VERSION_MAJOR.to_le_bytes());
    out.extend_from_slice(&VERSION_MINOR.to_le_bytes());
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(&prog.module_name.to_le_bytes());
    out.extend_from_slice(&prog.source_name.to_le_bytes());
    out.extend_from_slice(&prog.entry.to_le_bytes());
    out.extend_from_slice(&(sections.len() as u32).to_le_bytes());
    debug_assert_eq!(out.len(), HEADER_SIZE);

    // Offsets are absolute, so compute them before writing the table.
    let table_end = HEADER_SIZE + sections.len() * SECTION_ENTRY_SIZE;
    let mut cursor = align8(table_end);
    let mut offsets = Vec::with_capacity(sections.len());
    for s in &sections {
        offsets.push(cursor);
        cursor = align8(cursor + s.body.len());
    }

    for (s, off) in sections.iter().zip(&offsets) {
        out.extend_from_slice(&s.kind.to_le_bytes());
        out.extend_from_slice(&(*off as u32).to_le_bytes());
        out.extend_from_slice(&(s.body.len() as u32).to_le_bytes());
        out.extend_from_slice(&s.count.to_le_bytes());
    }

    for (s, off) in sections.iter().zip(&offsets) {
        pad_to(&mut out, *off);
        out.extend_from_slice(&s.body);
    }
    pad_to(&mut out, cursor);
    out
}

fn align8(n: usize) -> usize {
    (n + 7) & !7
}

fn pad_to(out: &mut Vec<u8>, offset: usize) {
    while out.len() < offset {
        out.push(0);
    }
}
