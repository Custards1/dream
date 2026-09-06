//! Resolution and lowering: AST -> execution-tree arena.
//!
//! This pass does three jobs at once:
//!   1. Name resolution — every variable becomes a frame slot, a closure
//!      capture index, a global index, or a builtin id, so the VM never has to
//!      hash a name at run time.
//!   2. Purity checking — a name ending in `!` denotes an impure value.
//!      A pure function may not reach one, which is what makes `pure_fn` and
//!      `impure_fn` genuinely different object types rather than a convention.
//!   3. Emission of nodes into the flat arena.

use std::collections::HashMap;

use crate::ast::*;
use crate::diag::Diag;
use crate::ir::*;
use crate::lexer::Span;
use crate::consteval::{self, CValue, CompCtx};
use crate::modules::ModuleSet;

fn sp(s: Span) -> (u32, u32) {
    (s.start, s.end)
}

fn item_span(item: &Item) -> Span {
    match item {
        Item::Import(i) => i.span,
        Item::Derive(d) => d.span,
        Item::Virtual(v) => v.span,
        Item::Let(l) => l.span,
        // Resolved away by the loader before lowering ever runs.
        Item::When { span, .. } => *span,
    }
}

#[derive(Debug, Clone, Copy)]
enum Res {
    Local(u32),
    Capture(u32),
    Global(u32),
    Builtin(u32),
}

struct Frame {
    func: usize,
    scopes: Vec<HashMap<String, u32>>,
    next_slot: u32,
    max_slots: u32,
    cap_names: Vec<String>,
    caps: Vec<u32>,
    /// The enclosing function was declared impure, so effects are allowed.
    impure_allowed: bool,
    /// An effect was actually performed inside this frame.
    saw_impure: bool,
    display: String,
}

/// Where a module alias points. A Dream module is resolved at compile time --
/// `list.map` becomes a global index -- while a native one keeps a runtime
/// lookup, because the host owns its member table.
#[derive(Debug, Clone, Copy)]
enum ModuleRef {
    Dream(usize),
    /// The global index holding the module value.
    Native(u32),
}

/// Per-module name resolution state.
#[derive(Default)]
struct ModuleCtx<'a> {
    globals: HashMap<String, u32>,
    aliases: HashMap<String, ModuleRef>,
    /// The module's top-level functions, so `comp` can call them.
    funcs: HashMap<String, &'a LetDecl>,
    source: usize,
    derives: Option<usize>,
}

/// A `comp!` waiting to be evaluated on the VM once the whole program has been
/// lowered, because staging it needs a complete image.
pub struct PendingComp {
    /// The placeholder node to overwrite with the result.
    pub node: u32,
    /// A synthetic zero-arity function holding the expression.
    pub func: usize,
    pub span: Span,
    pub source: usize,
}

/// A global whose body still has to be lowered.
struct Pending<'a> {
    func: usize,
    /// The module whose names the body is resolved against. For a derived
    /// copy this is the deriving module, not the one the code was written in --
    /// that is what makes a virtual bind to the implementation.
    ctx: usize,
    /// The module the code was written in, for diagnostics.
    source: usize,
    decl: &'a LetDecl,
}

pub struct Lowerer<'a> {
    pub prog: Program,
    pub diags: Vec<Diag>,
    modules: Vec<ModuleCtx<'a>>,
    current: usize,
    /// The file the code being lowered came from.
    current_source: usize,
    frames: Vec<Frame>,
    set: Option<&'a ModuleSet>,
    pub pending_comp: Vec<PendingComp>,
}

impl<'a> Lowerer<'a> {
    pub fn new(module_name: &str, source_name: &str) -> Lowerer<'a> {
        let mut prog = Program { entry: NO_NODE, ..Default::default() };
        prog.module_name = prog.k.string(module_name);
        prog.source_name = prog.k.string(source_name);
        Lowerer {
            prog,
            diags: Vec::new(),
            modules: vec![ModuleCtx::default()],
            current: 0,
            current_source: 0,
            frames: Vec::new(),
            set: None,
            pending_comp: Vec::new(),
        }
    }

    fn err(&mut self, span: Span, msg: impl Into<String>) {
        let source = self.current_source;
        self.diags.push(Diag::error(span, msg).in_source(source));
    }

    // ---- frames and scopes ------------------------------------------------

    fn frame(&mut self) -> &mut Frame {
        self.frames.last_mut().expect("no active frame")
    }

    fn alloc_slot(&mut self) -> u32 {
        let f = self.frame();
        let slot = f.next_slot;
        f.next_slot += 1;
        f.max_slots = f.max_slots.max(f.next_slot);
        slot
    }

    fn define(&mut self, name: &str, slot: u32) {
        if name == "_" {
            return; // wildcard: occupies a slot but is unreachable
        }
        let f = self.frame();
        f.scopes.last_mut().expect("no active scope").insert(name.to_string(), slot);
    }

    fn lookup_local(&self, fi: usize, name: &str) -> Option<u32> {
        self.frames[fi].scopes.iter().rev().find_map(|s| s.get(name).copied())
    }

    /// Thread `name` down from an enclosing frame, adding a capture to every
    /// frame along the way. Returns the capture index within frame `fi`.
    fn capture(&mut self, fi: usize, name: &str) -> Option<u32> {
        if let Some(pos) = self.frames[fi].cap_names.iter().position(|n| n == name) {
            return Some(pos as u32);
        }
        if fi == 0 {
            return None;
        }
        let desc = if let Some(slot) = self.lookup_local(fi - 1, name) {
            encode_capture(false, slot)
        } else {
            let parent_cap = self.capture(fi - 1, name)?;
            encode_capture(true, parent_cap)
        };
        let f = &mut self.frames[fi];
        let idx = f.caps.len() as u32;
        f.caps.push(desc);
        f.cap_names.push(name.to_string());
        Some(idx)
    }

    fn resolve(&mut self, name: &str) -> Option<Res> {
        if !self.frames.is_empty() {
            let top = self.frames.len() - 1;
            if let Some(slot) = self.lookup_local(top, name) {
                return Some(Res::Local(slot));
            }
            if let Some(ci) = self.capture(top, name) {
                return Some(Res::Capture(ci));
            }
        }
        if let Some(&g) = self.modules[self.current].globals.get(name) {
            return Some(Res::Global(g));
        }
        builtin_id(name).map(Res::Builtin)
    }

    /// Record that an effect happens here, rejecting it in a pure function.
    fn note_effect(&mut self, span: Span, what: &str) {
        let f = self.frames.last().expect("no active frame");
        if !f.impure_allowed {
            let display = f.display.clone();
            let source = self.current_source;
            self.diags.push(
                Diag::error(span, format!("cannot use {what} inside the pure function `{display}`"))
                    .with_note(
                        "mark the function impure by ending its name with `!`, \
                         e.g. `let f! x = ..`",
                    )
                    .in_source(source),
            );
        }
        self.frame().saw_impure = true;
    }

    fn node(&mut self, n: Node, span: Span) -> u32 {
        self.prog.push_node(n, sp(span))
    }

    /// Effects are visible through any enclosing node, so propagate the flag up.
    fn inherit(&mut self, node: u32, kids: &[u32]) {
        let mut flags = 0u8;
        for &k in kids {
            if k != NO_NODE {
                flags |= self.prog.nodes[k as usize].flags & F_IMPURE;
            }
        }
        self.prog.nodes[node as usize].flags |= flags;
    }

    fn is_impure_node(&self, node: u32) -> bool {
        node != NO_NODE && self.prog.nodes[node as usize].flags & F_IMPURE != 0
    }

    // ---- whole programs ----------------------------------------------------

    /// Lower every module of a program into one image.
    pub fn lower_program(&mut self, set: &'a ModuleSet) {
        self.set = Some(set);
        self.modules = (0..set.modules.len()).map(|_| ModuleCtx::default()).collect();
        for (i, m) in set.modules.iter().enumerate() {
            self.modules[i].source = m.source;
        }

        // Phase 1: declare every module's names before lowering any body, so
        // forward references and mutual recursion work within and across
        // modules. Dependencies come first in the list, so a module's aliases
        // always point at something already declared.
        let mut pending: Vec<Pending<'a>> = Vec::new();
        for mi in 0..set.modules.len() {
            self.declare_module(set, mi, &mut pending);
        }

        // Phase 2: lower the bodies.
        for p in pending {
            self.current = p.ctx;
            self.current_source = p.source;
            self.lower_global_body(p.func, p.decl);
        }

        for i in 0..self.prog.funcs.len() {
            let body = self.prog.funcs[i].body;
            self.mark_tail(body);
        }

        // The entry point is `main!` in the root module, which is the last one
        // loaded because its dependencies were resolved first.
        let root = set.modules.len().saturating_sub(1);
        if let Some(&g) = self.modules.get(root).and_then(|m| m.globals.get("main!")) {
            self.prog.entry = self.prog.globals[g as usize].target;
        }
    }

    /// The items a module contributes, with anything it derives coming first so
    /// the deriving module's own definitions override them.
    fn effective_items(
        &self,
        set: &'a ModuleSet,
        mi: usize,
        out: &mut Vec<(usize, &'a Item)>,
        seen: &mut Vec<usize>,
    ) {
        if seen.contains(&mi) {
            return;
        }
        seen.push(mi);
        for item in &set.modules[mi].ast.items {
            if let Item::Derive(d) = item {
                if let Some(base) = set.find(&d.path.join(".")) {
                    self.effective_items(set, base, out, seen);
                }
            }
        }
        for item in &set.modules[mi].ast.items {
            out.push((mi, item));
        }
    }

    fn declare_module(&mut self, set: &'a ModuleSet, mi: usize, pending: &mut Vec<Pending<'a>>) {
        self.current = mi;
        self.current_source = set.modules[mi].source;

        let name_k = self.prog.k.string(&set.modules[mi].name);
        let src_k = self.prog.k.string(&set.files[set.modules[mi].source].path);
        let globals_start = self.prog.globals.len() as u32;

        // Aliases first: imports, then derives (which also bring the base
        // module's own aliases along, so its code still resolves once copied).
        let mut derives: Option<usize> = None;
        for item in &set.modules[mi].ast.items {
            match item {
                Item::Import(imp) => self.declare_import(set, mi, imp),
                Item::Derive(d) => {
                    let path = d.path.join(".");
                    match set.find(&path) {
                        Some(base) => {
                            if derives.is_some() {
                                self.err(d.span, "a module can derive only one other module");
                            } else {
                                derives = Some(base);
                                self.modules[mi].derives = Some(base);
                                // Inherit the base's aliases for anything this
                                // module has not bound itself.
                                let inherited: Vec<(String, ModuleRef)> = self.modules[base]
                                    .aliases
                                    .iter()
                                    .map(|(k, v)| (k.clone(), *v))
                                    .collect();
                                for (k, v) in inherited {
                                    self.modules[mi].aliases.entry(k).or_insert(v);
                                }
                                self.modules[mi]
                                    .aliases
                                    .insert(d.alias.clone(), ModuleRef::Dream(base));
                            }
                        }
                        None => {
                            self.err(
                                d.span,
                                format!("cannot derive `{path}`: it is not a Dream module"),
                            );
                        }
                    }
                }
                _ => {}
            }
        }

        // Collect the items this module contributes, base first.
        let mut items: Vec<(usize, &'a Item)> = Vec::new();
        let mut seen = Vec::new();
        self.effective_items(set, mi, &mut items, &mut seen);

        // A later `let` of the same name overrides an earlier one, which is how
        // a deriving module implements a virtual.
        let mut lets: Vec<(usize, &'a LetDecl)> = Vec::new();
        let mut virtuals: Vec<(usize, &'a VirtualDecl)> = Vec::new();
        for (owner, item) in &items {
            match item {
                Item::Let(d) => {
                    match lets.iter_mut().find(|(_, l)| l.name == d.name) {
                        // Redefining a name from a module we derive is the
                        // point of deriving. Redefining one from this very
                        // module is a mistake.
                        Some(existing) if existing.0 != *owner => *existing = (*owner, d),
                        Some(_) => {
                            self.diags.push(
                                Diag::error(
                                    d.name_span,
                                    format!("`{}` is already bound in this module", d.name),
                                )
                                .in_source(set.modules[*owner].source),
                            );
                        }
                        None => lets.push((*owner, d)),
                    }
                }
                Item::Virtual(v) => {
                    if !virtuals.iter().any(|(_, x)| x.name == v.name) {
                        virtuals.push((*owner, v));
                    }
                }
                _ => {}
            }
        }

        let mut has_virtual = false;
        for (owner, v) in &virtuals {
            let implemented = lets.iter().any(|(_, l)| l.name == v.name);
            if implemented {
                continue;
            }
            if let Some(default) = &v.default {
                // Synthesize a `let` from the default body. It is lowered in
                // this module's context, so it still sees the overrides.
                let decl = self.synth_default(v, default);
                lets.push((*owner, decl));
            } else if *owner != mi {
                self.diags.push(
                    Diag::error(
                        set.modules[mi].ast.items.first().map(item_span).unwrap_or(v.span),
                        format!(
                            "module `{}` derives `{}` but does not implement `{}`",
                            set.modules[mi].name, set.modules[*owner].name, v.name
                        ),
                    )
                    .with_note(format!(
                        "add `let {} {}= ..`, or give the virtual a default body",
                        v.name,
                        v.params
                            .iter()
                            .map(|p| format!("{} ", p.name))
                            .collect::<String>()
                    ))
                    .in_source(set.modules[mi].source),
                );
            } else {
                // The declaring module itself. It stays abstract: calling the
                // virtual here raises, rather than failing to compile a module
                // that was never meant to be run directly.
                has_virtual = true;
                let decl = self.synth_abstract(set, mi, v);
                lets.push((mi, decl));
            }
        }
        if !virtuals.is_empty() {
            has_virtual = true;
        }

        for (owner, decl) in lets {
            self.modules[mi].funcs.insert(decl.name.clone(), decl);
            if let Some(fi) = self.declare_global(mi, decl) {
                pending.push(Pending {
                    func: fi,
                    ctx: mi,
                    source: set.modules[owner].source,
                    decl,
                });
            }
        }

        let globals_count = self.prog.globals.len() as u32 - globals_start;
        self.prog.modules.push(ModuleRec {
            name: name_k,
            source: src_k,
            globals_start,
            globals_count,
            flags: if has_virtual { M_ABSTRACT } else { 0 },
            derives: derives.map(|d| d as u32).unwrap_or(NO_NODE),
        });
    }

    /// Turn a virtual's default body into a `let` this module owns.
    fn synth_default(&mut self, v: &VirtualDecl, default: &Expr) -> &'a LetDecl {
        let decl = LetDecl {
            name: v.name.clone(),
            impure: v.impure,
            rec: false,
            params: v.params.clone(),
            body: default.clone(),
            span: v.span,
            name_span: v.name_span,
        };
        Box::leak(Box::new(decl))
    }

    /// A virtual with no default, in the module that declares it. Calling it
    /// raises, so the abstract module still compiles and still runs its own
    /// tests -- only the unimplemented hole fails.
    fn synth_abstract(&mut self, set: &ModuleSet, mi: usize, v: &VirtualDecl) -> &'a LetDecl {
        let message = format!(
            "`{}` is a virtual of module `{}` with no implementation",
            v.name, set.modules[mi].name
        );
        let body = Expr {
            kind: ExprKind::Apply(
                Box::new(Expr { kind: ExprKind::Name("raise!".into()), span: v.span }),
                vec![Expr { kind: ExprKind::Str(message), span: v.span }],
            ),
            span: v.span,
        };
        let decl = LetDecl {
            name: v.name.clone(),
            // Raising is an effect, so the stub has to be allowed to perform it.
            impure: true,
            rec: false,
            params: v.params.clone(),
            body,
            span: v.span,
            name_span: v.name_span,
        };
        Box::leak(Box::new(decl))
    }

    fn declare_import(&mut self, set: &ModuleSet, mi: usize, imp: &Import) {
        let path = imp.path.join(".");
        if self.modules[mi].aliases.contains_key(&imp.alias) {
            self.err(imp.span, format!("`{}` is already imported here", imp.alias));
            return;
        }
        if let Some(target) = set.find(&path) {
            self.modules[mi].aliases.insert(imp.alias.clone(), ModuleRef::Dream(target));
            return;
        }

        // Host-provided: keep a module value and resolve members at run time.
        let path_k = self.prog.k.string(&path);
        let alias_k = self.prog.k.string(&imp.alias);
        let import_idx = self.prog.imports.len() as u32;
        self.prog.imports.push(ImportEntry { path: path_k, alias: alias_k });

        let g = self.prog.globals.len() as u32;
        self.prog.globals.push(Global {
            name: alias_k,
            kind: GlobalKind::Module,
            target: import_idx,
            flags: 0,
        });
        self.modules[mi].aliases.insert(imp.alias.clone(), ModuleRef::Native(g));
        // A host module is a value as well as a namespace, so a bare mention of
        // the alias yields the module itself.
        self.modules[mi].globals.insert(imp.alias.clone(), g);
    }

    /// Reserve the function and global records; returns the function index.
    fn declare_global(&mut self, mi: usize, decl: &LetDecl) -> Option<usize> {
        if self.modules[mi].globals.contains_key(&decl.name) {
            self.err(decl.name_span, format!("`{}` is already bound in this module", decl.name));
            return None;
        }
        let name_k = self.prog.k.string(&decl.name);
        let mut flags = 0u16;
        if decl.impure {
            flags |= FN_IMPURE;
        }
        if decl.rec {
            flags |= FN_REC;
        }
        // A parameterless binding is a 0-arity function. A pure one is a lazy
        // value the VM forces once and memoizes; an impure one is an action
        // that must re-run on every call.
        if decl.params.is_empty() {
            flags |= FN_THUNK;
            if !decl.impure {
                flags |= FN_GLOBAL_VALUE;
            }
        }

        let fi = self.prog.funcs.len();
        self.prog.funcs.push(Func {
            name: name_k,
            body: NO_NODE,
            arity: decl.params.len() as u16,
            flags,
            slots: 0,
            n_captures: 0,
            captures_off: 0,
            span_start: decl.span.start,
            span_end: decl.span.end,
        });

        let g = self.prog.globals.len() as u32;
        self.prog.globals.push(Global {
            name: name_k,
            kind: GlobalKind::Function,
            target: fi as u32,
            flags: G_EXPORTED | if decl.impure { G_IMPURE } else { 0 },
        });
        self.modules[mi].globals.insert(decl.name.clone(), g);
        Some(fi)
    }

    fn lower_global_body(&mut self, fi: usize, decl: &LetDecl) {
        self.push_frame(fi, &decl.name, decl.impure, &decl.params);
        let body = self.lower_expr(&decl.body);
        self.pop_frame(fi, body);
    }

    fn push_frame(&mut self, func: usize, display: &str, impure: bool, params: &[Param]) {
        self.frames.push(Frame {
            func,
            scopes: vec![HashMap::new()],
            next_slot: 0,
            max_slots: 0,
            cap_names: Vec::new(),
            caps: Vec::new(),
            impure_allowed: impure,
            saw_impure: false,
            display: display.to_string(),
        });
        for p in params {
            let slot = self.alloc_slot();
            self.define(&p.name, slot);
        }
    }

    fn pop_frame(&mut self, fi: usize, body: u32) {
        let f = self.frames.pop().expect("frame underflow");
        debug_assert_eq!(f.func, fi);
        let captures_off = self.prog.push_kids(&f.caps);
        let fun = &mut self.prog.funcs[fi];
        fun.body = body;
        fun.slots = f.max_slots as u16;
        fun.n_captures = f.caps.len() as u16;
        fun.captures_off = captures_off;
        if f.saw_impure {
            fun.flags |= FN_IMPURE;
        }
    }

    // ---- expressions -------------------------------------------------------

    fn lower_expr(&mut self, e: &Expr) -> u32 {
        match &e.kind {
            ExprKind::Int(v) => {
                let k = self.prog.k.int(*v);
                self.node(Node::with(Op::ConstInt, k, NO_NODE, NO_NODE), e.span)
            }
            ExprKind::Float(v) => {
                let k = self.prog.k.float(*v);
                self.node(Node::with(Op::ConstFloat, k, NO_NODE, NO_NODE), e.span)
            }
            ExprKind::Str(s) => {
                let k = self.prog.k.string(s);
                self.node(Node::with(Op::ConstStr, k, NO_NODE, NO_NODE), e.span)
            }
            ExprKind::Atom(s) => {
                let k = self.prog.k.atom(s);
                self.node(Node::with(Op::ConstAtom, k, NO_NODE, NO_NODE), e.span)
            }
            ExprKind::Char(c) => {
                self.node(Node::with(Op::ConstChar, *c as u32, NO_NODE, NO_NODE), e.span)
            }
            ExprKind::Bool(b) => {
                self.node(Node::with(Op::ConstBool, *b as u32, NO_NODE, NO_NODE), e.span)
            }
            ExprKind::Unit => self.node(Node::new(Op::Unit), e.span),

            ExprKind::Name(name) => self.lower_name(name, e.span),

            ExprKind::Field(obj, field) => {
                // `list.map` where `list` names a Dream module is a global, not
                // a lookup: the whole program is compiled together, so the
                // member's index is known now.
                if let ExprKind::Name(alias) = &obj.kind {
                    if self.resolve_local_or_capture(alias).is_none() {
                        if let Some(m) = self.modules[self.current].aliases.get(alias).copied() {
                            return self.lower_module_member(m, alias, field, e.span);
                        }
                    }
                }
                let o = self.lower_expr(obj);
                let k = self.prog.k.string(field);
                let n = self.node(Node::with(Op::Field, o, k, NO_NODE), e.span);
                self.inherit(n, &[o]);
                if field.ends_with('!') {
                    self.note_effect(e.span, &format!("the impure member `{field}`"));
                    self.prog.set_flag(n, F_IMPURE);
                }
                n
            }

            ExprKind::Comp { impure, expr } => {
                if *impure {
                    self.lower_comp_impure(expr, e.span)
                } else {
                    self.lower_comp(expr, e.span)
                }
            }

            ExprKind::Apply(f, args) => {
                let callee = self.lower_expr(f);
                let arg_nodes: Vec<u32> = args.iter().map(|a| self.lower_expr(a)).collect();
                self.emit_apply(callee, &arg_nodes, e.span)
            }

            // `lhs |> f a`  ==>  `f a lhs`: the piped value becomes the last
            // argument, which is what makes partial application read naturally.
            ExprKind::Pipe(lhs, rhs) => {
                let value = self.lower_expr(lhs);
                match &rhs.kind {
                    ExprKind::Apply(f, args) => {
                        let callee = self.lower_expr(f);
                        let mut arg_nodes: Vec<u32> =
                            args.iter().map(|a| self.lower_expr(a)).collect();
                        arg_nodes.push(value);
                        self.emit_apply(callee, &arg_nodes, e.span)
                    }
                    _ => {
                        let callee = self.lower_expr(rhs);
                        self.emit_apply(callee, &[value], e.span)
                    }
                }
            }

            ExprKind::Unary(op, inner) => {
                let a = self.lower_expr(inner);
                let opcode = match op {
                    UnOp::Neg => Op::Neg,
                    UnOp::Not => Op::Not,
                };
                let n = self.node(Node::with(opcode, a, NO_NODE, NO_NODE), e.span);
                self.inherit(n, &[a]);
                n
            }

            ExprKind::Binary(op, l, r) => {
                let a = self.lower_expr(l);
                let b = self.lower_expr(r);
                let opcode = match op {
                    BinOp::Add => Op::Add,
                    BinOp::Sub => Op::Sub,
                    BinOp::Mul => Op::Mul,
                    BinOp::Div => Op::Div,
                    BinOp::Mod => Op::Mod,
                    BinOp::Eq => Op::Eq,
                    BinOp::Ne => Op::Ne,
                    BinOp::Lt => Op::Lt,
                    BinOp::Le => Op::Le,
                    BinOp::Gt => Op::Gt,
                    BinOp::Ge => Op::Ge,
                    BinOp::And => Op::And,
                    BinOp::Or => Op::Or,
                };
                let n = self.node(Node::with(opcode, a, b, NO_NODE), e.span);
                self.inherit(n, &[a, b]);
                n
            }

            ExprKind::If(c, t, f) => {
                let cn = self.lower_expr(c);
                let tn = self.lower_expr(t);
                let fnode = match f {
                    Some(f) => self.lower_expr(f),
                    None => self.node(Node::new(Op::Unit), e.span),
                };
                // The condition drives control flow, so it is always forced.
                self.prog.set_flag(cn, F_STRICT);
                let n = self.node(Node::with(Op::If, cn, tn, fnode), e.span);
                self.inherit(n, &[cn, tn, fnode]);
                n
            }

            ExprKind::Block(stmts) => self.lower_block(stmts, e.span),

            ExprKind::Thunk(inner) => {
                let fi = self.begin_child_fn("<thunk>", FN_THUNK, 0, e.span);
                // Suspending an effect is not performing one, so the body of a
                // `$( .. )` may be impure wherever it is written. Running it
                // still needs an impure context, because that takes `spawn!`
                // or a call the checker can see. This is what lets a pure list
                // hold effectful work -- a list of test cases, a queue of jobs.
                self.push_frame(fi, "<thunk>", true, &[]);
                let body = self.lower_expr(inner);
                let was_impure = self.frames.last().is_some_and(|f| f.saw_impure);
                self.pop_frame(fi, body);
                self.mark_tail(body);
                let n = self.node(Node::with(Op::MakeThunk, fi as u32, NO_NODE, NO_NODE), e.span);
                if was_impure {
                    self.prog.set_flag(n, F_IMPURE);
                }
                n
            }

            ExprKind::Lambda { params, body } => {
                let fi = self.begin_child_fn("<lambda>", 0, params.len() as u16, e.span);
                let impure_ctx = self.frames.last().is_some_and(|f| f.impure_allowed);
                self.push_frame(fi, "<lambda>", impure_ctx, params);
                let b = self.lower_expr(body);
                self.pop_frame(fi, b);
                self.mark_tail(b);
                self.node(Node::with(Op::MakeClosure, fi as u32, NO_NODE, NO_NODE), e.span)
            }

            ExprKind::Try { body, binder, handler } => {
                self.note_effect(e.span, "`try!`");
                let b = self.lower_expr(body);
                // The point of `try!` is to force the body here, where the
                // handler is installed; a lazy result would escape the frame.
                self.prog.set_flag(b, F_STRICT);
                let slot = self.alloc_slot();
                self.frame().scopes.push(HashMap::new());
                self.define(binder, slot);
                let h = self.lower_expr(handler);
                self.frame().scopes.pop();
                let n = self.node(Node::with(Op::Try, b, h, slot), e.span);
                self.prog.set_flag(n, F_IMPURE);
                n
            }

            ExprKind::List(items) => self.lower_seq(Op::MakeList, items, e.span),
            ExprKind::Array(items) => self.lower_seq(Op::MakeArray, items, e.span),
            ExprKind::Map(pairs) => {
                let mut kids = Vec::with_capacity(pairs.len() * 2);
                for (k, v) in pairs {
                    kids.push(self.lower_expr(k));
                    kids.push(self.lower_expr(v));
                }
                let off = self.prog.push_kids(&kids);
                let n = self.node(
                    Node::with(Op::MakeMap, off, pairs.len() as u32, NO_NODE),
                    e.span,
                );
                self.inherit(n, &kids);
                n
            }
        }
    }

    /// Is this name shadowed by something in the current function? A local
    /// binding beats a module alias.
    fn resolve_local_or_capture(&mut self, name: &str) -> Option<u32> {
        if self.frames.is_empty() {
            return None;
        }
        let top = self.frames.len() - 1;
        if let Some(slot) = self.lookup_local(top, name) {
            return Some(slot);
        }
        self.capture(top, name)
    }

    fn lower_module_member(
        &mut self,
        m: ModuleRef,
        alias: &str,
        field: &str,
        span: Span,
    ) -> u32 {
        match m {
            ModuleRef::Dream(target) => {
                let found = self.modules[target].globals.get(field).copied();
                let Some(g) = found else {
                    let mut names: Vec<String> =
                        self.modules[target].globals.keys().cloned().collect();
                    names.sort();
                    let source = self.current_source;
                    self.diags.push(
                        Diag::error(span, format!("module `{alias}` has no member `{field}`"))
                            .with_note(if names.is_empty() {
                                "it exports nothing".to_string()
                            } else {
                                format!("it exports: {}", names.join(", "))
                            })
                            .in_source(source),
                    );
                    return self.node(Node::new(Op::Unit), span);
                };
                let n = self.node(Node::with(Op::Global, g, NO_NODE, NO_NODE), span);
                if field.ends_with('!') {
                    self.note_effect(span, &format!("the impure function `{alias}.{field}`"));
                    self.prog.set_flag(n, F_IMPURE);
                }
                n
            }
            ModuleRef::Native(global) => {
                let o = self.node(Node::with(Op::Global, global, NO_NODE, NO_NODE), span);
                let k = self.prog.k.string(field);
                let n = self.node(Node::with(Op::Field, o, k, NO_NODE), span);
                if field.ends_with('!') {
                    self.note_effect(span, &format!("the impure member `{field}`"));
                    self.prog.set_flag(n, F_IMPURE);
                }
                n
            }
        }
    }

    /// Stage a `comp!` expression: put it in a synthetic zero-arity function
    /// and leave a placeholder node. It cannot be evaluated yet, because
    /// running it needs an image of the whole program.
    fn lower_comp_impure(&mut self, inner: &Expr, span: Span) -> u32 {
        let fi = self.begin_child_fn("<comp!>", FN_IMPURE | FN_THUNK, 0, span);
        // Lower it with an empty frame stack. A compile-time value cannot
        // depend on a run-time one, so a reference to an enclosing parameter
        // must fail to resolve rather than silently capture nothing.
        let saved = std::mem::take(&mut self.frames);
        self.push_frame(fi, "<comp!>", true, &[]);
        let body = self.lower_expr(inner);
        self.pop_frame(fi, body);
        self.mark_tail(body);
        self.frames = saved;

        let node = self.node(Node::new(Op::Unit), span);
        self.pending_comp.push(PendingComp {
            node,
            func: fi,
            span,
            source: self.current_source,
        });
        node
    }

    /// Replace a `comp!` placeholder with the value the VM produced.
    pub fn patch_comp(&mut self, node: u32, value: &CValue, span: Span) {
        let root = self.emit_const(value, span);
        // Node fields are indices, so copying the root's record into the
        // placeholder grafts the whole constant in place.
        self.prog.nodes[node as usize] = self.prog.nodes[root as usize];
    }

    /// Fold a `comp` expression now and emit its value as a literal.
    fn lower_comp(&mut self, inner: &Expr, span: Span) -> u32 {
        let Some(set) = self.set else {
            self.err(span, "`comp` needs a whole program to evaluate against");
            return self.node(Node::new(Op::Unit), span);
        };
        let ctx = CompCtx {
            modules: self.modules.iter().map(|m| m.funcs.clone()).collect(),
            sources: self.modules.iter().map(|m| m.source).collect(),
            aliases: self.modules[self.current]
                .aliases
                .iter()
                .filter_map(|(k, v)| match v {
                    ModuleRef::Dream(i) => Some((k.clone(), *i)),
                    ModuleRef::Native(_) => None,
                })
                .collect(),
            current: self.current,
        };
        let _ = set;
        let value = consteval::evaluate(&ctx, inner);
        match value {
            Ok(v) => self.emit_const(&v, span),
            Err(d) => {
                // The evaluator already tagged the file the failing code is in.
                let source = self.current_source;
                let d = if d.source == 0 { d.in_source(source) } else { d };
                self.diags.push(d);
                self.node(Node::new(Op::Unit), span)
            }
        }
    }

    /// Rebuild a folded value as nodes. Structures become the same construction
    /// the source would have produced, so nothing downstream has to know a
    /// value came from `comp`.
    fn emit_const(&mut self, v: &CValue, span: Span) -> u32 {
        match v {
            CValue::Int(n) => {
                let k = self.prog.k.int(*n);
                self.node(Node::with(Op::ConstInt, k, NO_NODE, NO_NODE), span)
            }
            CValue::Float(f) => {
                let k = self.prog.k.float(*f);
                self.node(Node::with(Op::ConstFloat, k, NO_NODE, NO_NODE), span)
            }
            CValue::Bool(b) => self.node(Node::with(Op::ConstBool, *b as u32, NO_NODE, NO_NODE), span),
            CValue::Char(c) => {
                self.node(Node::with(Op::ConstChar, *c as u32, NO_NODE, NO_NODE), span)
            }
            CValue::Str(s) => {
                let k = self.prog.k.string(s);
                self.node(Node::with(Op::ConstStr, k, NO_NODE, NO_NODE), span)
            }
            CValue::Atom(a) => {
                let k = self.prog.k.atom(a);
                self.node(Node::with(Op::ConstAtom, k, NO_NODE, NO_NODE), span)
            }
            CValue::Unit => self.node(Node::new(Op::Unit), span),
            CValue::List(items) => {
                let kids: Vec<u32> = items.iter().map(|i| self.emit_const(i, span)).collect();
                let off = self.prog.push_kids(&kids);
                self.node(Node::with(Op::MakeList, off, kids.len() as u32, NO_NODE), span)
            }
            CValue::Array(items) => {
                let kids: Vec<u32> = items.iter().map(|i| self.emit_const(i, span)).collect();
                let off = self.prog.push_kids(&kids);
                self.node(Node::with(Op::MakeArray, off, kids.len() as u32, NO_NODE), span)
            }
            CValue::Map(pairs) => {
                let mut kids = Vec::with_capacity(pairs.len() * 2);
                for (k, val) in pairs {
                    kids.push(self.emit_const(k, span));
                    kids.push(self.emit_const(val, span));
                }
                let off = self.prog.push_kids(&kids);
                self.node(Node::with(Op::MakeMap, off, pairs.len() as u32, NO_NODE), span)
            }
            CValue::Fn(_) => {
                self.err(
                    span,
                    "`comp` produced a function, which cannot be baked into the image",
                );
                self.node(Node::new(Op::Unit), span)
            }
        }
    }

    fn lower_seq(&mut self, op: Op, items: &[Expr], span: Span) -> u32 {
        let kids: Vec<u32> = items.iter().map(|i| self.lower_expr(i)).collect();
        let off = self.prog.push_kids(&kids);
        let n = self.node(Node::with(op, off, kids.len() as u32, NO_NODE), span);
        self.inherit(n, &kids);
        n
    }

    fn emit_apply(&mut self, callee: u32, args: &[u32], span: Span) -> u32 {
        let off = self.prog.push_kids(args);
        let n = self.node(Node::with(Op::Apply, callee, off, args.len() as u32), span);
        self.inherit(n, args);
        // Calling an impure value is itself an effect.
        if self.is_impure_node(callee) {
            self.prog.set_flag(n, F_IMPURE);
            self.prog.set_flag(callee, F_STRICT);
        }
        n
    }

    fn lower_name(&mut self, name: &str, span: Span) -> u32 {
        let res = self.resolve(name);
        let node = match res {
            Some(Res::Local(slot)) => {
                self.node(Node::with(Op::Local, slot, NO_NODE, NO_NODE), span)
            }
            Some(Res::Capture(ci)) => {
                self.node(Node::with(Op::Capture, ci, NO_NODE, NO_NODE), span)
            }
            Some(Res::Global(g)) => self.node(Node::with(Op::Global, g, NO_NODE, NO_NODE), span),
            Some(Res::Builtin(b)) => {
                self.node(Node::with(Op::Builtin, b, NO_NODE, NO_NODE), span)
            }
            None => {
                // A Dream module is a compile-time namespace, not a value, so
                // say that rather than claiming the name does not exist.
                if let Some(ModuleRef::Dream(_)) = self.modules[self.current].aliases.get(name) {
                    let source = self.current_source;
                    self.diags.push(
                        Diag::error(span, format!("`{name}` is a module, not a value"))
                            .with_note(format!("use one of its members, as in `{name}.something`"))
                            .in_source(source),
                    );
                } else {
                    self.err(span, format!("cannot find `{name}` in this scope"));
                }
                self.node(Node::new(Op::Unit), span)
            }
        };
        if name.ends_with('!') {
            self.note_effect(span, &format!("the impure function `{name}`"));
            self.prog.set_flag(node, F_IMPURE);
        }
        node
    }

    fn begin_child_fn(&mut self, name: &str, flags: u16, arity: u16, span: Span) -> usize {
        let name_k = self.prog.k.string(name);
        let fi = self.prog.funcs.len();
        self.prog.funcs.push(Func {
            name: name_k,
            body: NO_NODE,
            arity,
            flags,
            slots: 0,
            n_captures: 0,
            captures_off: 0,
            span_start: span.start,
            span_end: span.end,
        });
        fi
    }

    fn lower_block(&mut self, stmts: &[Stmt], span: Span) -> u32 {
        self.frame().scopes.push(HashMap::new());
        let mut kids: Vec<u32> = Vec::with_capacity(stmts.len());
        let mut stmt_spans: Vec<Span> = Vec::with_capacity(stmts.len());

        for stmt in stmts {
            stmt_spans.push(match stmt {
                Stmt::Expr(e) => e.span,
                Stmt::Let(d) => d.span,
            });
            match stmt {
                Stmt::Expr(e) => kids.push(self.lower_expr(e)),
                Stmt::Let(decl) => {
                    let slot = self.alloc_slot();
                    // `let rec` puts the name in scope for its own body so the
                    // closure can capture its own (still empty) cell.
                    if decl.rec {
                        self.define(&decl.name, slot);
                    }
                    let value = self.lower_local_binding(decl);
                    if !decl.rec {
                        self.define(&decl.name, slot);
                    }
                    let bind = self.node(Node::with(Op::Bind, slot, value, NO_NODE), decl.span);
                    self.inherit(bind, &[value]);
                    // Binding an effectful expression sequences it here rather
                    // than at first use, so effects stay in written order.
                    if self.is_impure_node(value) {
                        self.prog.set_flag(bind, F_STRICT);
                        self.prog.set_flag(value, F_STRICT);
                    }
                    kids.push(bind);
                }
            }
        }
        self.frame().scopes.pop();

        // A non-final statement contributes nothing but its effects. Inside an
        // impure function force it, so those effects happen in written order.
        // Inside a pure one there are no effects to have, so it is dead code:
        // forcing it would only risk raising an error the program never asked
        // for.
        let impure_ctx = self.frames.last().is_some_and(|f| f.impure_allowed);
        if kids.len() > 1 {
            for i in 0..kids.len() - 1 {
                let k = kids[i];
                // Bindings stay lazy; `lower_local_binding` already forced the
                // ones whose value is effectful.
                if self.prog.nodes[k as usize].opcode() == Some(Op::Bind) {
                    continue;
                }
                if impure_ctx {
                    self.prog.set_flag(k, F_STRICT);
                } else if !self.is_impure_node(k) {
                    self.diags.push(
                        Diag::warning(
                            stmt_spans[i],
                            "this statement's value is discarded, and a pure \
                             expression has no other effect",
                        )
                        .with_note("only the final expression of a block is used"),
                    );
                }
            }
        }

        let off = self.prog.push_kids(&kids);
        let n = self.node(Node::with(Op::Block, off, kids.len() as u32, NO_NODE), span);
        self.inherit(n, &kids);
        n
    }

    /// A block-local `let`. With parameters it becomes a nested closure.
    fn lower_local_binding(&mut self, decl: &LetDecl) -> u32 {
        if decl.params.is_empty() {
            return self.lower_expr(&decl.body);
        }
        let mut flags = 0u16;
        if decl.impure {
            flags |= FN_IMPURE;
        }
        if decl.rec {
            flags |= FN_REC;
        }
        let fi = self.begin_child_fn(&decl.name, flags, decl.params.len() as u16, decl.span);
        self.push_frame(fi, &decl.name, decl.impure, &decl.params);
        let body = self.lower_expr(&decl.body);
        self.pop_frame(fi, body);
        self.mark_tail(body);
        self.node(Node::with(Op::MakeClosure, fi as u32, NO_NODE, NO_NODE), decl.span)
    }

    // ---- tail marking ------------------------------------------------------

    /// Flag calls in tail position so the VM can reuse the frame instead of
    /// growing the stack — `fac` recursing 100k deep should not blow up.
    fn mark_tail(&mut self, node: u32) {
        if node == NO_NODE {
            return;
        }
        let n = self.prog.nodes[node as usize];
        match n.opcode() {
            Some(Op::Apply) => self.prog.set_flag(node, F_TAIL),
            Some(Op::If) => {
                self.mark_tail(n.b);
                self.mark_tail(n.c);
            }
            Some(Op::Block) => {
                if n.b > 0 {
                    let last = self.prog.kids[(n.a + n.b - 1) as usize];
                    self.mark_tail(last);
                }
            }
            Some(Op::Force) => self.mark_tail(n.a),
            _ => {}
        }
    }
}
