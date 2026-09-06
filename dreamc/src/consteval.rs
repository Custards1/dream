//! Compile-time evaluation for `comp`.
//!
//! `comp e` evaluates `e` while compiling and bakes the result into the image
//! as a literal. The evaluator is deliberately a separate, small interpreter
//! over the syntax tree rather than a call into the VM: the compiler must not
//! depend on the runtime, and restricting `comp` to a pure, terminating subset
//! is easier to enforce here than to police afterwards.
//!
//! What it will not do is the point of it. Effects, `try!`, `spawn!` and host
//! modules are rejected, because a value baked into a program cannot have
//! observably happened at run time. Recursion is allowed but budgeted, so a
//! non-terminating `comp` is a compile error rather than a hung build.

use std::collections::HashMap;
use std::sync::Arc;

use crate::ast::*;
use crate::diag::Diag;
use crate::lexer::Span;

#[derive(Debug, Clone)]
pub enum CValue {
    Int(i64),
    Float(f64),
    Bool(bool),
    Char(char),
    Str(String),
    Atom(String),
    Unit,
    List(Vec<CValue>),
    Array(Vec<CValue>),
    Map(Vec<(CValue, CValue)>),
    Fn(Arc<CClosure>),
}

#[derive(Debug, Clone)]
pub struct CClosure {
    params: Vec<String>,
    body: Expr,
    env: Env,
    /// The module the body was written in. A function called from another
    /// module still resolves its own names -- including its recursive call --
    /// against the module it came from.
    module: usize,
    /// Arguments already applied, for currying.
    applied: Vec<CValue>,
}

type Env = Vec<(String, CValue)>;

/// Structural equality, matching the runtime's. Functions compare by identity,
/// because there is no useful structural answer for them.
impl PartialEq for CValue {
    fn eq(&self, other: &CValue) -> bool {
        use CValue::*;
        match (self, other) {
            (Int(a), Int(b)) => a == b,
            (Float(a), Float(b)) => a == b,
            (Int(a), Float(b)) | (Float(b), Int(a)) => (*a as f64) == *b,
            (Bool(a), Bool(b)) => a == b,
            (Char(a), Char(b)) => a == b,
            (Str(a), Str(b)) => a == b,
            (Atom(a), Atom(b)) => a == b,
            (Unit, Unit) => true,
            (List(a), List(b)) | (Array(a), Array(b)) => a == b,
            (Map(a), Map(b)) => {
                a.len() == b.len()
                    && a.iter().all(|(k, v)| {
                        b.iter().any(|(k2, v2)| k == k2 && v == v2)
                    })
            }
            (Fn(a), Fn(b)) => Arc::ptr_eq(a, b),
            _ => false,
        }
    }
}

/// What the evaluator can see: every module's top-level functions, and the
/// current module's aliases, so `comp list.length xs` works across modules.
pub struct CompCtx<'a> {
    pub modules: Vec<HashMap<String, &'a LetDecl>>,
    pub aliases: HashMap<String, usize>,
    /// Source file index per module, so an error points at the file the code
    /// is actually in rather than at whoever wrote `comp`.
    pub sources: Vec<usize>,
    pub current: usize,
}

pub struct ConstEvaluator<'a> {
    ctx: &'a CompCtx<'a>,
    budget: i64,
    depth: u32,
}

/// Enough to fold a real table, small enough that a runaway `comp` fails fast.
const DEFAULT_BUDGET: i64 = 2_000_000;

/// Recursion here runs on a host stack, so the step budget alone is not enough:
/// two million steps of a self-call would overflow long before the budget ran
/// out. Evaluation therefore runs on its own generously sized stack (see
/// [`evaluate`]) and this bounds it well short of that.
/// Sized against the stack in [`evaluate`], leaving generous room for the
/// largest frame an unoptimized build produces.
const MAX_DEPTH: u32 = 16_384;

/// Evaluate `e`, on a stack big enough for genuinely recursive compile-time
/// code. A tree-walking evaluator uses one host frame per nesting level, and a
/// test thread's default stack runs out at a depth that ordinary code reaches;
/// borrowing a large stack is cheaper than restricting what `comp` may compute.
///
/// The stack and [`MAX_DEPTH`] are set together: the depth limit has to be
/// reached before the stack is, or a runaway `comp` aborts the compiler
/// instead of reporting an error.
pub fn evaluate(ctx: &CompCtx<'_>, e: &Expr) -> Result<CValue, Diag> {
    const STACK: usize = 256 * 1024 * 1024;
    std::thread::scope(|scope| {
        match std::thread::Builder::new()
            .stack_size(STACK)
            .spawn_scoped(scope, || ConstEvaluator::new(ctx).eval_top(e))
        {
            Ok(handle) => handle.join().unwrap_or_else(|_| {
                Err(Diag::error(e.span, "`comp` evaluation aborted unexpectedly"))
            }),
            // If a thread cannot be started, fall back to this one rather than
            // failing the build outright.
            Err(_) => ConstEvaluator::new(ctx).eval_top(e),
        }
    })
}

impl<'a> ConstEvaluator<'a> {
    pub fn new(ctx: &'a CompCtx<'a>) -> ConstEvaluator<'a> {
        ConstEvaluator { ctx, budget: DEFAULT_BUDGET, depth: 0 }
    }

    pub fn eval_top(&mut self, e: &Expr) -> Result<CValue, Diag> {
        let env: Env = Vec::new();
        self.eval(e, &env, self.ctx.current)
    }

    fn spend(&mut self, span: Span) -> Result<(), Diag> {
        self.budget -= 1;
        if self.budget <= 0 {
            return Err(Diag::error(span, "`comp` ran out of evaluation budget")
                .with_note("the expression may not terminate; `comp` must finish while compiling"));
        }
        Ok(())
    }

    fn source_of(&self, module: usize) -> usize {
        self.ctx.sources.get(module).copied().unwrap_or(0)
    }

    fn eval(&mut self, e: &Expr, env: &Env, module: usize) -> Result<CValue, Diag> {
        self.spend(e.span).map_err(|d| d.in_source(self.source_of(module)))?;

        self.depth += 1;
        if self.depth > MAX_DEPTH {
            self.depth -= 1;
            return Err(Diag::error(e.span, "`comp` recursed too deeply")
                .with_note(
                    "the expression may not terminate. If it is genuinely this recursive, \
                     use `comp!`, which evaluates on the VM and is not bounded by the \
                     compiler's stack",
                )
                .in_source(self.source_of(module)));
        }
        let result = self.eval_inner(e, env, module);
        self.depth -= 1;

        result.map_err(|d| {
            // Only the innermost frame knows which file it was in; anything
            // that has already been tagged keeps its own.
            if d.source == 0 { d.in_source(self.source_of(module)) } else { d }
        })
    }

    fn eval_inner(&mut self, e: &Expr, env: &Env, module: usize) -> Result<CValue, Diag> {
        match &e.kind {
            ExprKind::Int(v) => Ok(CValue::Int(*v)),
            ExprKind::Float(v) => Ok(CValue::Float(*v)),
            ExprKind::Bool(v) => Ok(CValue::Bool(*v)),
            ExprKind::Char(v) => Ok(CValue::Char(*v)),
            ExprKind::Str(v) => Ok(CValue::Str(v.clone())),
            ExprKind::Atom(v) => Ok(CValue::Atom(v.clone())),
            ExprKind::Unit => Ok(CValue::Unit),

            ExprKind::Name(name) => self.eval_name(name, e.span, env, module),

            ExprKind::Field(obj, field) => {
                let ExprKind::Name(alias) = &obj.kind else {
                    return Err(self.reject(e.span, "a member lookup on a runtime value"));
                };
                let Some(&target) = self.ctx.aliases.get(alias) else {
                    return Err(Diag::error(
                        e.span,
                        format!("`{alias}` is not a Dream module, so `{alias}.{field}` is not \
                                 available at compile time"),
                    )
                    .with_note("host modules perform effects; `comp` cannot run them"));
                };
                self.eval_name(field, e.span, &Vec::new(), target)
            }

            ExprKind::Apply(f, args) => {
                let callee = self.eval(f, env, module)?;
                let mut vals = Vec::with_capacity(args.len());
                for a in args {
                    vals.push(self.eval(a, env, module)?);
                }
                self.apply(callee, vals, e.span)
            }

            ExprKind::Pipe(lhs, rhs) => {
                let value = self.eval(lhs, env, module)?;
                match &rhs.kind {
                    ExprKind::Apply(f, args) => {
                        let callee = self.eval(f, env, module)?;
                        let mut vals = Vec::with_capacity(args.len() + 1);
                        for a in args {
                            vals.push(self.eval(a, env, module)?);
                        }
                        vals.push(value);
                        self.apply(callee, vals, e.span)
                    }
                    _ => {
                        let callee = self.eval(rhs, env, module)?;
                        self.apply(callee, vec![value], e.span)
                    }
                }
            }

            ExprKind::Unary(op, inner) => {
                let v = self.eval(inner, env, module)?;
                match (op, v) {
                    (UnOp::Neg, CValue::Int(n)) => Ok(CValue::Int(-n)),
                    (UnOp::Neg, CValue::Float(f)) => Ok(CValue::Float(-f)),
                    (UnOp::Not, CValue::Bool(b)) => Ok(CValue::Bool(!b)),
                    (_, other) => Err(self.type_err(e.span, &other, "a number or bool")),
                }
            }

            ExprKind::Binary(op, l, r) => {
                // `&&` and `||` short-circuit here just as they do at run time.
                if matches!(op, BinOp::And | BinOp::Or) {
                    let lhs = self.eval(l, env, module)?;
                    let CValue::Bool(b) = lhs else {
                        return Err(self.type_err(l.span, &lhs, "a bool"));
                    };
                    if (*op == BinOp::And && !b) || (*op == BinOp::Or && b) {
                        return Ok(CValue::Bool(b));
                    }
                    let rhs = self.eval(r, env, module)?;
                    return match rhs {
                        CValue::Bool(_) => Ok(rhs),
                        other => Err(self.type_err(r.span, &other, "a bool")),
                    };
                }
                let a = self.eval(l, env, module)?;
                let b = self.eval(r, env, module)?;
                self.binop(*op, a, b, e.span)
            }

            ExprKind::If(c, t, f) => {
                let cond = self.eval(c, env, module)?;
                let CValue::Bool(b) = cond else {
                    return Err(self.type_err(c.span, &cond, "a bool"));
                };
                if b {
                    self.eval(t, env, module)
                } else if let Some(f) = f {
                    self.eval(f, env, module)
                } else {
                    Ok(CValue::Unit)
                }
            }

            ExprKind::Block(stmts) => {
                let mut local = env.clone();
                let mut last = CValue::Unit;
                for stmt in stmts {
                    match stmt {
                        Stmt::Let(d) => {
                            if d.impure {
                                return Err(self.reject(d.span, "an impure binding"));
                            }
                            let v = if d.params.is_empty() {
                                self.eval(&d.body, &local, module)?
                            } else {
                                CValue::Fn(Arc::new(CClosure {
                                    params: d.params.iter().map(|p| p.name.clone()).collect(),
                                    body: d.body.clone(),
                                    env: local.clone(),
                                    module,
                                    applied: Vec::new(),
                                }))
                            };
                            // `let rec` needs the binding visible in its own
                            // body; naming it before evaluating handles that
                            // for functions, which is the only shape that can
                            // observe it.
                            local.push((d.name.clone(), v));
                            last = CValue::Unit;
                        }
                        Stmt::Expr(x) => last = self.eval(x, &local, module)?,
                    }
                }
                Ok(last)
            }

            ExprKind::Lambda { params, body } => Ok(CValue::Fn(Arc::new(CClosure {
                params: params.iter().map(|p| p.name.clone()).collect(),
                body: (**body).clone(),
                env: env.clone(),
                module,
                applied: Vec::new(),
            }))),

            ExprKind::List(items) => {
                let mut out = Vec::with_capacity(items.len());
                for i in items {
                    out.push(self.eval(i, env, module)?);
                }
                Ok(CValue::List(out))
            }
            ExprKind::Array(items) => {
                let mut out = Vec::with_capacity(items.len());
                for i in items {
                    out.push(self.eval(i, env, module)?);
                }
                Ok(CValue::Array(out))
            }
            ExprKind::Map(pairs) => {
                let mut out = Vec::with_capacity(pairs.len());
                for (k, v) in pairs {
                    out.push((self.eval(k, env, module)?, self.eval(v, env, module)?));
                }
                Ok(CValue::Map(out))
            }

            // A nested `comp` is simply more of the same evaluation. A nested
            // `comp!` is not: this evaluator is the pure one.
            ExprKind::Comp { impure: false, expr } => self.eval(expr, env, module),
            ExprKind::Comp { impure: true, .. } => Err(self.reject(e.span, "`comp!`")),

            ExprKind::Thunk(_) => Err(self.reject(e.span, "a thunk")),
            ExprKind::Try { .. } => Err(self.reject(e.span, "`try!`")),
        }
    }

    fn eval_name(
        &mut self,
        name: &str,
        span: Span,
        env: &Env,
        module: usize,
    ) -> Result<CValue, Diag> {
        if let Some((_, v)) = env.iter().rev().find(|(n, _)| n == name) {
            return Ok(v.clone());
        }
        // Compile-time builtins: the pure ones, which is all `comp` can use.
        match name {
            "len" | "to_string" | "type_of" => {
                return Ok(CValue::Fn(Arc::new(CClosure {
                    params: vec![format!("$builtin:{name}")],
                    body: Expr { kind: ExprKind::Unit, span },
                    env: Vec::new(),
                    module,
                    applied: Vec::new(),
                })));
            }
            _ => {}
        }
        if name.ends_with('!') {
            return Err(self.reject(span, &format!("the impure function `{name}`")));
        }

        let Some(decl) = self.ctx.modules.get(module).and_then(|m| m.get(name)).copied() else {
            return Err(Diag::error(span, format!("cannot find `{name}` at compile time"))
                .with_note("`comp` sees module-level functions and its own bindings"));
        };
        if decl.impure {
            return Err(self.reject(span, &format!("the impure function `{name}`")));
        }
        if decl.params.is_empty() {
            return self.eval(&decl.body, &Vec::new(), module);
        }
        Ok(CValue::Fn(Arc::new(CClosure {
            params: decl.params.iter().map(|p| p.name.clone()).collect(),
            body: decl.body.clone(),
            env: Vec::new(),
            module,
            applied: Vec::new(),
        })))
    }

    fn apply(&mut self, callee: CValue, mut args: Vec<CValue>, span: Span) -> Result<CValue, Diag> {
        self.spend(span)?;
        let CValue::Fn(cl) = callee else {
            return Err(self.type_err(span, &callee, "a function"));
        };

        // The pure builtins are encoded as a one-parameter marker closure.
        if let Some(rest) = cl.params.first().and_then(|p| p.strip_prefix("$builtin:")) {
            if args.len() != 1 {
                return Err(Diag::error(span, format!("`{rest}` takes one argument")));
            }
            return self.builtin(rest, args.remove(0), span);
        }

        let mut applied = cl.applied.clone();
        applied.extend(args);
        if applied.len() < cl.params.len() {
            // Under-applied: keep currying, as at run time.
            return Ok(CValue::Fn(Arc::new(CClosure {
                params: cl.params.clone(),
                body: cl.body.clone(),
                env: cl.env.clone(),
                module: cl.module,
                applied,
            })));
        }
        let extra = applied.split_off(cl.params.len());

        let mut env = cl.env.clone();
        for (p, v) in cl.params.iter().zip(applied.into_iter()) {
            if p != "_" {
                env.push((p.clone(), v));
            }
        }
        // Resolve the body against the module it was written in, so a
        // recursive call inside an imported function finds itself.
        let result = self.eval(&cl.body, &env, cl.module)?;
        if extra.is_empty() {
            Ok(result)
        } else {
            self.apply(result, extra, span)
        }
    }

    fn builtin(&mut self, name: &str, arg: CValue, span: Span) -> Result<CValue, Diag> {
        match name {
            "len" => match &arg {
                CValue::Str(s) => Ok(CValue::Int(s.chars().count() as i64)),
                CValue::List(v) | CValue::Array(v) => Ok(CValue::Int(v.len() as i64)),
                CValue::Map(v) => Ok(CValue::Int(v.len() as i64)),
                other => Err(self.type_err(span, other, "something with a length")),
            },
            "to_string" => Ok(CValue::Str(render(&arg))),
            "type_of" => Ok(CValue::Atom(
                match arg {
                    CValue::Int(_) => "integer",
                    CValue::Float(_) => "float",
                    CValue::Bool(_) => "bool",
                    CValue::Char(_) => "char",
                    CValue::Str(_) => "string",
                    CValue::Atom(_) => "atom",
                    CValue::Unit => "unit",
                    CValue::List(_) => "list",
                    CValue::Array(_) => "array",
                    CValue::Map(_) => "map",
                    CValue::Fn(_) => "pure_fn",
                }
                .to_string(),
            )),
            _ => Err(Diag::error(span, format!("`{name}` is not available at compile time"))),
        }
    }

    fn binop(&mut self, op: BinOp, a: CValue, b: CValue, span: Span) -> Result<CValue, Diag> {
        use BinOp::*;
        use CValue::*;
        let num = |x: &CValue| match x {
            Int(n) => Some(*n as f64),
            Float(f) => Some(*f),
            _ => None,
        };
        match (op, &a, &b) {
            (Add, Str(x), Str(y)) => return Ok(Str(format!("{x}{y}"))),
            (Add, List(x), List(y)) => {
                let mut v = x.clone();
                v.extend(y.clone());
                return Ok(List(v));
            }
            (Eq, _, _) => return Ok(Bool(a == b)),
            (Ne, _, _) => return Ok(Bool(a != b)),
            _ => {}
        }

        if let (Int(x), Int(y)) = (&a, &b) {
            let (x, y) = (*x, *y);
            return match op {
                Add => x.checked_add(y).map(Int).ok_or_else(|| overflow(span)),
                Sub => x.checked_sub(y).map(Int).ok_or_else(|| overflow(span)),
                Mul => x.checked_mul(y).map(Int).ok_or_else(|| overflow(span)),
                Div => {
                    if y == 0 {
                        Err(Diag::error(span, "division by zero while evaluating `comp`"))
                    } else {
                        Ok(Int(x / y))
                    }
                }
                Mod => {
                    if y == 0 {
                        Err(Diag::error(span, "remainder by zero while evaluating `comp`"))
                    } else {
                        Ok(Int(x % y))
                    }
                }
                Lt => Ok(Bool(x < y)),
                Le => Ok(Bool(x <= y)),
                Gt => Ok(Bool(x > y)),
                Ge => Ok(Bool(x >= y)),
                _ => Err(self.type_err(span, &a, "operands this operator accepts")),
            };
        }

        if let (Some(x), Some(y)) = (num(&a), num(&b)) {
            return match op {
                Add => Ok(Float(x + y)),
                Sub => Ok(Float(x - y)),
                Mul => Ok(Float(x * y)),
                Div => Ok(Float(x / y)),
                Mod => Ok(Float(x % y)),
                Lt => Ok(Bool(x < y)),
                Le => Ok(Bool(x <= y)),
                Gt => Ok(Bool(x > y)),
                Ge => Ok(Bool(x >= y)),
                _ => Err(self.type_err(span, &a, "operands this operator accepts")),
            };
        }

        if let (Str(x), Str(y)) = (&a, &b) {
            return match op {
                Lt => Ok(Bool(x < y)),
                Le => Ok(Bool(x <= y)),
                Gt => Ok(Bool(x > y)),
                Ge => Ok(Bool(x >= y)),
                _ => Err(self.type_err(span, &a, "operands this operator accepts")),
            };
        }
        Err(self.type_err(span, &a, "operands this operator accepts"))
    }

    fn reject(&self, span: Span, what: &str) -> Diag {
        Diag::error(span, format!("`comp` cannot evaluate {what}"))
            .with_note("a compile-time value has to be computable without running the program")
    }

    fn type_err(&self, span: Span, got: &CValue, want: &str) -> Diag {
        Diag::error(span, format!("`comp` expected {want}, got {}", describe(got)))
    }
}

fn overflow(span: Span) -> Diag {
    Diag::error(span, "integer overflow while evaluating `comp`")
}

fn describe(v: &CValue) -> String {
    match v {
        CValue::Int(n) => format!("the integer {n}"),
        CValue::Float(f) => format!("the float {f}"),
        CValue::Bool(b) => format!("{b}"),
        CValue::Char(_) => "a char".into(),
        CValue::Str(_) => "a string".into(),
        CValue::Atom(a) => format!(":{a}"),
        CValue::Unit => "unit".into(),
        CValue::List(_) => "a list".into(),
        CValue::Array(_) => "an array".into(),
        CValue::Map(_) => "a map".into(),
        CValue::Fn(_) => "a function".into(),
    }
}

/// The same rendering the runtime's `to_string` produces, so a value folded at
/// compile time prints identically to one computed at run time.
pub fn render(v: &CValue) -> String {
    match v {
        CValue::Int(n) => n.to_string(),
        CValue::Float(f) => {
            let mut s = format!("{f}");
            if !s.contains('.') && !s.contains('e') && !s.contains("inf") && !s.contains("NaN") {
                s.push_str("");
            }
            s
        }
        CValue::Bool(b) => b.to_string(),
        CValue::Char(c) => c.to_string(),
        CValue::Str(s) => s.clone(),
        CValue::Atom(a) => format!(":{a}"),
        CValue::Unit => "()".into(),
        CValue::List(items) => format!(
            "[{}]",
            items.iter().map(quoted).collect::<Vec<_>>().join(", ")
        ),
        CValue::Array(items) => format!(
            "#[{}]",
            items.iter().map(quoted).collect::<Vec<_>>().join(", ")
        ),
        CValue::Map(pairs) => format!(
            "%{{{}}}",
            pairs
                .iter()
                .map(|(k, v)| format!("{} => {}", quoted(k), quoted(v)))
                .collect::<Vec<_>>()
                .join(", ")
        ),
        CValue::Fn(_) => "<fn>".into(),
    }
}

fn quoted(v: &CValue) -> String {
    match v {
        CValue::Str(s) => format!("{s:?}"),
        CValue::Char(c) => format!("{c:?}"),
        other => render(other),
    }
}
