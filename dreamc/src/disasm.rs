//! Reader and textual dump for a bytecode image.
//!
//! This deliberately reads the *file*, not the in-memory `Program`, so a dump
//! also proves the container round-trips.

use std::collections::HashMap;
use std::fmt::Write as _;

use crate::emit::*;
use crate::ir::*;

pub struct Image<'a> {
    data: &'a [u8],
    /// kind -> (offset, byte_len, count)
    sections: HashMap<u32, (usize, usize, u32)>,
    pub version: (u16, u16),
    pub flags: u32,
    pub module_name: u32,
    pub source_name: u32,
    pub entry: u32,
    pub order: Vec<u32>,
}

#[derive(Debug)]
pub enum ImageError {
    TooSmall,
    BadMagic,
    UnsupportedVersion(u16, u16),
    TruncatedSection(String),
}

impl std::fmt::Display for ImageError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ImageError::TooSmall => write!(f, "file is smaller than the header"),
            ImageError::BadMagic => write!(f, "bad magic number (expected DAGNCAAF)"),
            ImageError::UnsupportedVersion(a, b) => write!(f, "unsupported version {a}.{b}"),
            ImageError::TruncatedSection(s) => write!(f, "section {s} extends past end of file"),
        }
    }
}

fn u16_at(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}
fn u32_at(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}
fn u64_at(d: &[u8], o: usize) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&d[o..o + 8]);
    u64::from_le_bytes(b)
}

impl<'a> Image<'a> {
    pub fn load(data: &'a [u8]) -> Result<Image<'a>, ImageError> {
        if data.len() < HEADER_SIZE {
            return Err(ImageError::TooSmall);
        }
        if data[..8] != MAGIC {
            return Err(ImageError::BadMagic);
        }
        let major = u16_at(data, 8);
        let minor = u16_at(data, 10);
        if major != VERSION_MAJOR {
            return Err(ImageError::UnsupportedVersion(major, minor));
        }
        let flags = u32_at(data, 12);
        let module_name = u32_at(data, 16);
        let source_name = u32_at(data, 20);
        let entry = u32_at(data, 24);
        let n = u32_at(data, 28) as usize;

        let mut sections = HashMap::new();
        let mut order = Vec::new();
        for i in 0..n {
            let base = HEADER_SIZE + i * SECTION_ENTRY_SIZE;
            if base + SECTION_ENTRY_SIZE > data.len() {
                return Err(ImageError::TooSmall);
            }
            let kind = u32_at(data, base);
            let off = u32_at(data, base + 4) as usize;
            let len = u32_at(data, base + 8) as usize;
            let count = u32_at(data, base + 12);
            if off + len > data.len() {
                return Err(ImageError::TruncatedSection(tag_name(kind)));
            }
            sections.insert(kind, (off, len, count));
            order.push(kind);
        }
        Ok(Image { data, sections, version: (major, minor), flags, module_name, source_name, entry, order })
    }

    pub fn section(&self, kind: u32) -> Option<(&'a [u8], u32)> {
        let &(off, len, count) = self.sections.get(&kind)?;
        Some((&self.data[off..off + len], count))
    }
    pub fn section_meta(&self, kind: u32) -> Option<(usize, usize, u32)> {
        self.sections.get(&kind).copied()
    }

    pub fn int(&self, i: u32) -> i64 {
        self.section(T_KINT)
            .map(|(b, _)| u64_at(b, i as usize * 8) as i64)
            .unwrap_or(0)
    }
    pub fn float(&self, i: u32) -> f64 {
        self.section(T_KFLT)
            .map(|(b, _)| f64::from_bits(u64_at(b, i as usize * 8)))
            .unwrap_or(0.0)
    }
    pub fn string(&self, i: u32) -> &'a str {
        let Some((table, count)) = self.section(T_KSTR) else { return "<?>" };
        if i >= count {
            return "<?>";
        }
        let off = u32_at(table, i as usize * 8) as usize;
        let len = u32_at(table, i as usize * 8 + 4) as usize;
        let Some((blob, _)) = self.section(T_SBLB) else { return "<?>" };
        std::str::from_utf8(&blob[off..off + len]).unwrap_or("<invalid utf8>")
    }
    pub fn atom(&self, i: u32) -> &'a str {
        let Some((b, count)) = self.section(T_KATM) else { return "<?>" };
        if i >= count {
            return "<?>";
        }
        self.string(u32_at(b, i as usize * 4))
    }
    pub fn node(&self, i: u32) -> Node {
        let Some((b, count)) = self.section(T_NODE) else { return Node::new(Op::Nop) };
        if i >= count {
            return Node::new(Op::Nop);
        }
        Node::read(&b[i as usize * NODE_SIZE..])
    }
    pub fn node_count(&self) -> u32 {
        self.section(T_NODE).map(|(_, c)| c).unwrap_or(0)
    }
    pub fn kid(&self, i: u32) -> u32 {
        self.section(T_KIDS).map(|(b, _)| u32_at(b, i as usize * 4)).unwrap_or(NO_NODE)
    }
    pub fn kids(&self, off: u32, count: u32) -> Vec<u32> {
        (0..count).map(|i| self.kid(off + i)).collect()
    }
    pub fn func(&self, i: u32) -> Option<Func> {
        let (b, count) = self.section(T_FUNC)?;
        if i >= count {
            return None;
        }
        let o = i as usize * FUNC_SIZE;
        Some(Func {
            name: u32_at(b, o),
            body: u32_at(b, o + 4),
            arity: u16_at(b, o + 8),
            flags: u16_at(b, o + 10),
            slots: u16_at(b, o + 12),
            n_captures: u16_at(b, o + 14),
            captures_off: u32_at(b, o + 16),
            span_start: u32_at(b, o + 20),
            span_end: u32_at(b, o + 24),
        })
    }
    pub fn func_count(&self) -> u32 {
        self.section(T_FUNC).map(|(_, c)| c).unwrap_or(0)
    }
    pub fn global(&self, i: u32) -> Option<Global> {
        let (b, count) = self.section(T_GLBL)?;
        if i >= count {
            return None;
        }
        let o = i as usize * GLOBAL_SIZE;
        Some(Global {
            name: u32_at(b, o),
            kind: if u32_at(b, o + 4) == 1 { GlobalKind::Module } else { GlobalKind::Function },
            target: u32_at(b, o + 8),
            flags: u32_at(b, o + 12),
        })
    }
    pub fn global_count(&self) -> u32 {
        self.section(T_GLBL).map(|(_, c)| c).unwrap_or(0)
    }
    pub fn import(&self, i: u32) -> Option<ImportEntry> {
        let (b, count) = self.section(T_IMPT)?;
        if i >= count {
            return None;
        }
        let o = i as usize * IMPORT_SIZE;
        Some(ImportEntry { path: u32_at(b, o), alias: u32_at(b, o + 4) })
    }
    pub fn import_count(&self) -> u32 {
        self.section(T_IMPT).map(|(_, c)| c).unwrap_or(0)
    }

    fn global_name(&self, i: u32) -> &'a str {
        self.global(i).map(|g| self.string(g.name)).unwrap_or("<?>")
    }
    fn func_name(&self, i: u32) -> &'a str {
        self.func(i).map(|f| self.string(f.name)).unwrap_or("<?>")
    }
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

pub fn dump(img: &Image) -> String {
    let mut out = String::new();
    let _ = writeln!(out, "; dream bytecode image");
    let _ = writeln!(
        out,
        "; magic {}  version {}.{}  flags 0x{:x}{}",
        String::from_utf8_lossy(&MAGIC),
        img.version.0,
        img.version.1,
        img.flags,
        if img.flags & FLAG_DEBUG != 0 { " (debug)" } else { "" }
    );
    let _ = writeln!(
        out,
        "; module {:?}  source {:?}",
        img.string(img.module_name),
        img.string(img.source_name)
    );
    if img.entry == NO_NODE {
        let _ = writeln!(out, "; entry <none>");
    } else {
        let _ = writeln!(out, "; entry fn#{} `{}`", img.entry, img.func_name(img.entry));
    }

    let _ = writeln!(out, "\nsections:");
    for kind in &img.order {
        if let Some((off, len, count)) = img.section_meta(*kind) {
            let _ = writeln!(
                out,
                "  {:<4}  offset 0x{:06x}  bytes {:>6}  count {:>5}",
                tag_name(*kind),
                off,
                len,
                count
            );
        }
    }

    let _ = writeln!(out, "\nconstants:");
    if let Some((_, n)) = img.section(T_KINT) {
        for i in 0..n {
            let _ = writeln!(out, "  int  [{i}] = {}", img.int(i));
        }
    }
    if let Some((_, n)) = img.section(T_KFLT) {
        for i in 0..n {
            let _ = writeln!(out, "  float[{i}] = {}", img.float(i));
        }
    }
    if let Some((_, n)) = img.section(T_KSTR) {
        for i in 0..n {
            let _ = writeln!(out, "  str  [{i}] = {:?}", img.string(i));
        }
    }
    if let Some((_, n)) = img.section(T_KATM) {
        for i in 0..n {
            let _ = writeln!(out, "  atom [{i}] = :{}", img.atom(i));
        }
    }

    if img.import_count() > 0 {
        let _ = writeln!(out, "\nimports:");
        for i in 0..img.import_count() {
            let imp = img.import(i).unwrap();
            let _ = writeln!(
                out,
                "  [{i}] {} as {}",
                img.string(imp.path),
                img.string(imp.alias)
            );
        }
    }

    let _ = writeln!(out, "\nglobals:");
    for i in 0..img.global_count() {
        let g = img.global(i).unwrap();
        let target = match g.kind {
            GlobalKind::Function => format!("fn#{}", g.target),
            GlobalKind::Module => format!("module#{}", g.target),
        };
        let mut attrs = Vec::new();
        if g.flags & G_EXPORTED != 0 {
            attrs.push("exported");
        }
        if g.flags & G_IMPURE != 0 {
            attrs.push("impure");
        }
        let _ = writeln!(
            out,
            "  [{i}] {:<16} -> {:<10} {}",
            img.string(g.name),
            target,
            if attrs.is_empty() { String::new() } else { format!("[{}]", attrs.join(" ")) }
        );
    }

    for i in 0..img.func_count() {
        let f = img.func(i).unwrap();
        let mut attrs = Vec::new();
        if f.flags & FN_IMPURE != 0 {
            attrs.push("impure");
        } else {
            attrs.push("pure");
        }
        if f.flags & FN_REC != 0 {
            attrs.push("rec");
        }
        if f.flags & FN_THUNK != 0 {
            attrs.push("thunk");
        }
        if f.flags & FN_GLOBAL_VALUE != 0 {
            attrs.push("memoized");
        }
        let _ = writeln!(
            out,
            "\nfn#{i} {}  arity {}  slots {}  captures {}  [{}]",
            img.string(f.name),
            f.arity,
            f.slots,
            f.n_captures,
            attrs.join(" ")
        );
        for c in 0..f.n_captures as u32 {
            let (from_cap, idx) = decode_capture(img.kid(f.captures_off + c));
            let _ = writeln!(
                out,
                "  capture[{c}] <- parent {} {idx}",
                if from_cap { "capture" } else { "slot" }
            );
        }
        print_node(img, f.body, 1, &mut out);
    }
    out
}

fn flag_str(flags: u8) -> String {
    let mut v = Vec::new();
    if flags & F_STRICT != 0 {
        v.push("strict");
    }
    if flags & F_IMPURE != 0 {
        v.push("impure");
    }
    if flags & F_TAIL != 0 {
        v.push("tail");
    }
    if v.is_empty() { String::new() } else { format!("  ; {}", v.join(",")) }
}

fn print_node(img: &Image, idx: u32, depth: usize, out: &mut String) {
    let pad = "  ".repeat(depth);
    if idx == NO_NODE {
        let _ = writeln!(out, "{pad}<none>");
        return;
    }
    let n = img.node(idx);
    let Some(op) = n.opcode() else {
        let _ = writeln!(out, "{pad}<bad opcode {}>", n.op);
        return;
    };
    let f = flag_str(n.flags);
    let head = |out: &mut String, text: String| {
        let _ = writeln!(out, "{pad}%{idx} {text}{f}");
    };

    match op {
        Op::ConstInt => head(out, format!("int {}", img.int(n.a))),
        Op::ConstFloat => head(out, format!("float {}", img.float(n.a))),
        Op::ConstStr => head(out, format!("str {:?}", img.string(n.a))),
        Op::ConstChar => head(
            out,
            format!("char {:?}", char::from_u32(n.a).unwrap_or('\u{fffd}')),
        ),
        Op::ConstBool => head(out, format!("bool {}", n.a != 0)),
        Op::ConstAtom => head(out, format!("atom :{}", img.atom(n.a))),
        Op::Unit => head(out, "unit".into()),
        Op::Local => head(out, format!("local ${}", n.a)),
        Op::Capture => head(out, format!("capture ^{}", n.a)),
        Op::Global => head(out, format!("global @{} `{}`", n.a, img.global_name(n.a))),
        Op::Builtin => head(
            out,
            format!("builtin #{} `{}`", n.a, BUILTINS.get(n.a as usize).copied().unwrap_or("?")),
        ),
        Op::Field => {
            head(out, format!("field .{}", img.string(n.b)));
            print_node(img, n.a, depth + 1, out);
        }
        Op::Apply => {
            head(out, format!("apply/{}", n.c));
            print_node(img, n.a, depth + 1, out);
            for k in img.kids(n.b, n.c) {
                print_node(img, k, depth + 1, out);
            }
        }
        Op::If => {
            head(out, "if".into());
            print_node(img, n.a, depth + 1, out);
            let _ = writeln!(out, "{pad}  then:");
            print_node(img, n.b, depth + 2, out);
            let _ = writeln!(out, "{pad}  else:");
            print_node(img, n.c, depth + 2, out);
        }
        Op::Block => {
            head(out, format!("block/{}", n.b));
            for k in img.kids(n.a, n.b) {
                print_node(img, k, depth + 1, out);
            }
        }
        Op::Bind => {
            head(out, format!("bind ${}", n.a));
            print_node(img, n.b, depth + 1, out);
        }
        Op::MakeClosure => head(out, format!("closure fn#{} `{}`", n.a, img.func_name(n.a))),
        Op::MakeThunk => head(out, format!("thunk fn#{}", n.a)),
        Op::Try => {
            head(out, format!("try (error -> ${})", n.c));
            let _ = writeln!(out, "{pad}  body:");
            print_node(img, n.a, depth + 2, out);
            let _ = writeln!(out, "{pad}  catch:");
            print_node(img, n.b, depth + 2, out);
        }
        Op::Force | Op::Neg | Op::Not => {
            head(out, op.name().into());
            print_node(img, n.a, depth + 1, out);
        }
        Op::MakeList | Op::MakeArray => {
            head(out, format!("{}/{}", op.name(), n.b));
            for k in img.kids(n.a, n.b) {
                print_node(img, k, depth + 1, out);
            }
        }
        Op::MakeMap => {
            head(out, format!("map/{}", n.b));
            for pair in img.kids(n.a, n.b * 2).chunks(2) {
                print_node(img, pair[0], depth + 1, out);
                print_node(img, pair[1], depth + 2, out);
            }
        }
        Op::Nop => head(out, "nop".into()),
        _ => {
            // Binary arithmetic, comparison and logic.
            head(out, op.name().into());
            print_node(img, n.a, depth + 1, out);
            print_node(img, n.b, depth + 1, out);
        }
    }
}
