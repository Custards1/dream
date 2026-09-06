//! Verifies a lowered program before it is emitted.
//!
//! The VM validates images it loads, but by then a compiler bug has already
//! turned into a corrupt file, and the message points at the wrong component.
//! This checks the same invariants one step earlier, where the answer is "the
//! compiler is wrong" and the report can say which function.
//!
//! Every check here corresponds to something the VM assumes without checking
//! at run time. `derive` re-lowers a module's code against another module's
//! frame, and `comp` rewrites nodes in place after the fact; both are exactly
//! the kind of thing that produces an index that is subtly out of range.

use std::fmt::Write as _;

use crate::ir::*;

#[derive(Debug)]
pub struct Problem {
    pub where_: String,
    pub what: String,
}

impl std::fmt::Display for Problem {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.where_, self.what)
    }
}

/// Check a program. An empty result means it is well formed.
pub fn verify(p: &Program) -> Vec<Problem> {
    let mut v = Verifier { p, problems: Vec::new(), context: String::new() };
    v.run();
    v.problems
}

/// Verify and panic on failure. Used by tests and debug builds, where a
/// malformed program should stop the world rather than be written out.
pub fn verify_or_panic(p: &Program) {
    let problems = verify(p);
    if problems.is_empty() {
        return;
    }
    let mut msg = String::from("the compiler produced a malformed program:\n");
    for pr in &problems {
        let _ = writeln!(msg, "  {pr}");
    }
    panic!("{msg}");
}

struct Verifier<'a> {
    p: &'a Program,
    problems: Vec<Problem>,
    context: String,
}

impl<'a> Verifier<'a> {
    fn bad(&mut self, what: impl Into<String>) {
        let where_ = if self.context.is_empty() { "program".into() } else { self.context.clone() };
        self.problems.push(Problem { where_, what: what.into() });
    }

    fn check(&mut self, ok: bool, what: impl Into<String>) {
        if !ok {
            self.bad(what);
        }
    }

    fn func_name(&self, fi: usize) -> String {
        self.p
            .funcs
            .get(fi)
            .and_then(|f| self.p.k.strings.get(f.name as usize))
            .cloned()
            .unwrap_or_else(|| format!("fn#{fi}"))
    }

    fn run(&mut self) {
        self.check_constants();
        self.check_modules();
        self.check_globals();
        self.check_functions();
        self.check_nodes();
        self.check_acyclic();
    }

    fn check_constants(&mut self) {
        let n_str = self.p.k.strings.len();
        for (i, &s) in self.p.k.atoms.iter().enumerate() {
            self.check(
                (s as usize) < n_str,
                format!("atom {i} names string {s}, but there are only {n_str}"),
            );
        }
        self.check(
            (self.p.module_name as usize) < n_str && (self.p.source_name as usize) < n_str,
            "the program's module or source name is out of range",
        );
    }

    fn check_modules(&mut self) {
        let n_globals = self.p.globals.len() as u32;
        let n_modules = self.p.modules.len() as u32;
        let mut covered = 0u32;
        for (i, m) in self.p.modules.iter().enumerate() {
            self.context = format!("module #{i}");
            self.check(
                (m.name as usize) < self.p.k.strings.len(),
                "module name is out of range",
            );
            self.check(
                m.globals_start + m.globals_count <= n_globals,
                format!(
                    "globals {}..{} extend past the {n_globals} globals that exist",
                    m.globals_start,
                    m.globals_start + m.globals_count
                ),
            );
            // Ranges are assigned in order as modules are declared, so they
            // must tile the global list exactly. A gap means a global was
            // created outside any module and nothing owns it.
            self.check(
                m.globals_start == covered,
                format!("globals start at {} but the previous module ended at {covered}", m.globals_start),
            );
            covered = m.globals_start + m.globals_count;
            self.check(
                m.derives == NO_NODE || m.derives < n_modules,
                format!("derives module {}, which does not exist", m.derives),
            );
            self.check(m.derives != i as u32, "a module cannot derive itself");
        }
        self.context.clear();
        if !self.p.modules.is_empty() {
            self.check(
                covered == n_globals,
                format!("modules account for {covered} globals but there are {n_globals}"),
            );
        }
    }

    fn check_globals(&mut self) {
        for (i, g) in self.p.globals.iter().enumerate() {
            self.context = format!("global #{i}");
            self.check(
                (g.name as usize) < self.p.k.strings.len(),
                "name is out of range",
            );
            match g.kind {
                GlobalKind::Function => self.check(
                    (g.target as usize) < self.p.funcs.len(),
                    format!("names function {}, which does not exist", g.target),
                ),
                GlobalKind::Module => self.check(
                    (g.target as usize) < self.p.imports.len(),
                    format!("names import {}, which does not exist", g.target),
                ),
            }
        }
        self.context.clear();
    }

    fn check_functions(&mut self) {
        let n_nodes = self.p.nodes.len() as u32;
        let n_kids = self.p.kids.len() as u32;
        for fi in 0..self.p.funcs.len() {
            let f = self.p.funcs[fi].clone();
            self.context = format!("fn#{fi} `{}`", self.func_name(fi));
            self.check(
                (f.name as usize) < self.p.k.strings.len(),
                "name is out of range",
            );
            self.check(f.body < n_nodes, format!("body node {} does not exist", f.body));
            self.check(
                f.slots >= f.arity,
                format!("has {} slots but takes {} parameters", f.slots, f.arity),
            );
            self.check(
                f.captures_off + u32::from(f.n_captures) <= n_kids,
                "capture list extends past the child pool",
            );
            for c in 0..u32::from(f.n_captures) {
                let desc = self.p.kids[(f.captures_off + c) as usize];
                let (from_capture, idx) = decode_capture(desc);
                // The parent frame is not known here, but an index this large
                // is a bug in any frame.
                self.check(
                    idx < 0x10000,
                    format!("capture {c} refers to {} {idx}, which is implausible",
                            if from_capture { "parent capture" } else { "parent slot" }),
                );
            }
            if f.body < n_nodes {
                self.check_slots_reachable(fi, &f);
            }
        }
        self.context.clear();

        if self.p.entry != NO_NODE {
            let e = self.p.entry as usize;
            self.check(e < self.p.funcs.len(), "the entry point is not a function");
            if e < self.p.funcs.len() {
                self.check(
                    self.p.funcs[e].arity == 0,
                    "the entry point takes parameters, but nothing can supply them",
                );
            }
        }
    }

    /// Walk everything a function's body reaches, checking that its slot and
    /// capture indices fit the frame it will actually run in.
    fn check_slots_reachable(&mut self, fi: usize, f: &Func) {
        let mut seen = vec![false; self.p.nodes.len()];
        let mut work = vec![f.body];
        while let Some(ni) = work.pop() {
            if ni == NO_NODE || ni as usize >= self.p.nodes.len() || seen[ni as usize] {
                continue;
            }
            seen[ni as usize] = true;
            let n = self.p.nodes[ni as usize];
            match n.opcode() {
                Some(Op::Local) => self.check(
                    n.a < u32::from(f.slots),
                    format!("node %{ni} reads slot {} of a {}-slot frame", n.a, f.slots),
                ),
                Some(Op::Bind) => {
                    self.check(
                        n.a < u32::from(f.slots),
                        format!("node %{ni} binds slot {} of a {}-slot frame", n.a, f.slots),
                    );
                    work.push(n.b);
                }
                Some(Op::Capture) => self.check(
                    n.a < u32::from(f.n_captures),
                    format!(
                        "node %{ni} reads capture {} of a closure with {}",
                        n.a, f.n_captures
                    ),
                ),
                Some(Op::Try) => {
                    self.check(
                        n.c < u32::from(f.slots),
                        format!("node %{ni} catches into slot {} of a {}-slot frame", n.c, f.slots),
                    );
                    work.push(n.a);
                    work.push(n.b);
                }
                _ => {
                    for child in self.children(&n) {
                        work.push(child);
                    }
                }
            }
            let _ = fi;
        }
    }

    /// The node indices a node points at. Constants and references have none.
    fn children(&self, n: &Node) -> Vec<u32> {
        let mut out = Vec::new();
        let kid = |off: u32, count: u32, out: &mut Vec<u32>| {
            for i in 0..count {
                if let Some(&k) = self.p.kids.get((off + i) as usize) {
                    out.push(k);
                }
            }
        };
        match n.opcode() {
            Some(Op::Field) | Some(Op::Force) | Some(Op::Neg) | Some(Op::Not) => out.push(n.a),
            Some(Op::Apply) => {
                out.push(n.a);
                kid(n.b, n.c, &mut out);
            }
            Some(Op::If) => {
                out.push(n.a);
                out.push(n.b);
                out.push(n.c);
            }
            Some(Op::Block) | Some(Op::MakeList) | Some(Op::MakeArray) => kid(n.a, n.b, &mut out),
            Some(Op::MakeMap) => kid(n.a, n.b * 2, &mut out),
            Some(Op::Bind) => out.push(n.b),
            Some(Op::Try) => {
                out.push(n.a);
                out.push(n.b);
            }
            Some(
                Op::Add | Op::Sub | Op::Mul | Op::Div | Op::Mod | Op::Eq | Op::Ne | Op::Lt
                | Op::Le | Op::Gt | Op::Ge | Op::And | Op::Or,
            ) => {
                out.push(n.a);
                out.push(n.b);
            }
            _ => {}
        }
        out.retain(|c| *c != NO_NODE);
        out
    }

    fn check_nodes(&mut self) {
        let n_nodes = self.p.nodes.len() as u32;
        let n_kids = self.p.kids.len() as u32;
        for i in 0..self.p.nodes.len() {
            let n = self.p.nodes[i];
            self.context = format!("node %{i}");
            let Some(op) = n.opcode() else {
                self.bad(format!("has unknown opcode {}", n.op));
                continue;
            };
            match op {
                Op::ConstInt => self.check(
                    (n.a as usize) < self.p.k.ints.len(),
                    format!("integer constant {} does not exist", n.a),
                ),
                Op::ConstFloat => self.check(
                    (n.a as usize) < self.p.k.floats.len(),
                    format!("float constant {} does not exist", n.a),
                ),
                Op::ConstStr => self.check(
                    (n.a as usize) < self.p.k.strings.len(),
                    format!("string constant {} does not exist", n.a),
                ),
                Op::ConstAtom => self.check(
                    (n.a as usize) < self.p.k.atoms.len(),
                    format!("atom {} does not exist", n.a),
                ),
                Op::ConstChar => self.check(
                    char::from_u32(n.a).is_some(),
                    format!("{} is not a Unicode scalar value", n.a),
                ),
                Op::ConstBool => self.check(n.a <= 1, "bool constant is neither 0 nor 1"),
                Op::Global => self.check(
                    (n.a as usize) < self.p.globals.len(),
                    format!("global {} does not exist", n.a),
                ),
                Op::Builtin => self.check(
                    (n.a as usize) < BUILTINS.len(),
                    format!("builtin {} does not exist", n.a),
                ),
                Op::MakeClosure | Op::MakeThunk => self.check(
                    (n.a as usize) < self.p.funcs.len(),
                    format!("function {} does not exist", n.a),
                ),
                Op::Field => {
                    self.check(n.a < n_nodes, "object node is out of range");
                    self.check(
                        (n.b as usize) < self.p.k.strings.len(),
                        "field name is out of range",
                    );
                }
                Op::Apply => {
                    self.check(n.a < n_nodes, "callee node is out of range");
                    self.check(n.b + n.c <= n_kids, "argument list extends past the child pool");
                }
                Op::If => {
                    self.check(n.a < n_nodes, "condition node is out of range");
                    self.check(n.b < n_nodes, "then branch is out of range");
                    self.check(n.c == NO_NODE || n.c < n_nodes, "else branch is out of range");
                }
                Op::Block | Op::MakeList | Op::MakeArray => {
                    self.check(n.a + n.b <= n_kids, "child list extends past the child pool");
                }
                Op::MakeMap => {
                    self.check(
                        n.a + n.b.saturating_mul(2) <= n_kids,
                        "map entries extend past the child pool",
                    );
                }
                Op::Bind => self.check(n.b < n_nodes, "bound value node is out of range"),
                Op::Try => {
                    self.check(n.a < n_nodes, "try body is out of range");
                    self.check(n.b < n_nodes, "catch handler is out of range");
                }
                Op::Force | Op::Neg | Op::Not => {
                    self.check(n.a < n_nodes, "operand node is out of range")
                }
                Op::Add | Op::Sub | Op::Mul | Op::Div | Op::Mod | Op::Eq | Op::Ne | Op::Lt
                | Op::Le | Op::Gt | Op::Ge | Op::And | Op::Or => {
                    self.check(n.a < n_nodes, "left operand is out of range");
                    self.check(n.b < n_nodes, "right operand is out of range");
                }
                Op::Local | Op::Capture | Op::Unit | Op::Nop => {}
            }

            // Every child of every node must exist, whatever the opcode.
            let kids = self.children(&n);
            for c in kids {
                self.check(c < n_nodes, format!("points at node %{c}, which does not exist"));
            }
        }
        self.context.clear();

        if !self.p.spans.is_empty() {
            self.check(
                self.p.spans.len() == self.p.nodes.len(),
                format!(
                    "{} spans for {} nodes; debug info must be parallel to the arena",
                    self.p.spans.len(),
                    self.p.nodes.len()
                ),
            );
        }
    }

    /// The execution tree has to be acyclic. A cycle is not a malformed index,
    /// so nothing else here would catch it -- but it would hang the VM.
    fn check_acyclic(&mut self) {
        #[derive(Clone, Copy, PartialEq)]
        enum Mark {
            White,
            Grey,
            Black,
        }
        let mut mark = vec![Mark::White; self.p.nodes.len()];
        // Iterative DFS: the arena can be deeper than the host stack.
        for root in 0..self.p.nodes.len() {
            if mark[root] != Mark::White {
                continue;
            }
            let mut stack: Vec<(u32, usize)> = vec![(root as u32, 0)];
            mark[root] = Mark::Grey;
            while let Some((ni, child_index)) = stack.pop() {
                let kids = self.children(&self.p.nodes[ni as usize]);
                if child_index >= kids.len() {
                    mark[ni as usize] = Mark::Black;
                    continue;
                }
                stack.push((ni, child_index + 1));
                let c = kids[child_index];
                if (c as usize) >= self.p.nodes.len() {
                    continue; // already reported by check_nodes
                }
                match mark[c as usize] {
                    Mark::Grey => {
                        self.context = format!("node %{ni}");
                        self.bad(format!("is part of a cycle through %{c}"));
                        self.context.clear();
                        // Treat it as visited so one cycle is reported once.
                        mark[c as usize] = Mark::Black;
                    }
                    Mark::White => {
                        mark[c as usize] = Mark::Grey;
                        stack.push((c, 0));
                    }
                    Mark::Black => {}
                }
            }
        }
    }
}
