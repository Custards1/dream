//! Surface syntax tree.

use crate::lexer::Span;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    Neg,
    Not,
}

#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    Float(f64),
    Char(char),
    Bool(bool),
    Str(String),
    Atom(String),
    Unit,

    /// A bare name. A trailing `!` is part of `name` and marks it impure.
    Name(String),
    /// `obj.field` — `field` may itself carry a trailing `!`.
    Field(Box<Expr>, String),

    /// Curried application: `f a b`.
    Apply(Box<Expr>, Vec<Expr>),
    /// `lhs |> f a` desugars to `f a lhs` during lowering.
    Pipe(Box<Expr>, Box<Expr>),

    Unary(UnOp, Box<Expr>),
    Binary(BinOp, Box<Expr>, Box<Expr>),

    If(Box<Expr>, Box<Expr>, Option<Box<Expr>>),
    /// `{ stmt* }` — value is the last statement, or unit if empty.
    Block(Vec<Stmt>),
    /// `$( e )` — a suspended computation (thunk object).
    Thunk(Box<Expr>),
    /// `try! { body } catch name { handler }`
    Try {
        body: Box<Expr>,
        binder: String,
        handler: Box<Expr>,
    },
    /// `fn a b -> e`
    Lambda {
        params: Vec<Param>,
        body: Box<Expr>,
    },
    /// `comp e` evaluates at compile time and bakes the result in. `comp! e`
    /// is the same but may perform effects, so it is evaluated by running it
    /// on the VM rather than by the compiler's own evaluator.
    Comp { impure: bool, expr: Box<Expr> },

    List(Vec<Expr>),
    Array(Vec<Expr>),
    Map(Vec<(Expr, Expr)>),

    /// `match e { p => b, .. }`
    Match {
        scrutinee: Box<Expr>,
        arms: Vec<MatchArm>,
    },
}

/// A pattern, as written in a `match` arm.
///
/// Patterns are matched in source order and force only as much of the value as
/// deciding takes -- `[x, ..rest]` forces the first cell and neither `x` nor
/// `rest`. That is what keeps `match` usable on a lazy or infinite structure.
#[derive(Debug, Clone)]
pub enum Pattern {
    /// `_` -- matches anything, binds nothing, forces nothing.
    Wildcard,
    /// A name -- matches anything and binds it. Forces nothing.
    Bind(String),
    /// A literal, compared with the same equality `==` uses.
    Int(i64),
    Float(f64),
    Char(char),
    Bool(bool),
    Str(String),
    Atom(String),
    Unit,
    /// `[a, b]`, or `[a, ..rest]` where the tail is bound (or `..` alone,
    /// which matches the rest without naming it).
    List(Vec<Pattern>, Option<Option<String>>),
    /// `#[a, b]` and `#[a, ..rest]`.
    Array(Vec<Pattern>, Option<Option<String>>),
    /// `%{ k => p }` -- the keys are expressions, matched against what the map
    /// holds. Keys the pattern does not mention are ignored.
    Map(Vec<(Expr, Pattern)>),
    /// `p as name` -- matches `p` and also binds the whole value.
    As(Box<Pattern>, String),
}

/// One `pattern => body` of a `match`, with an optional guard.
#[derive(Debug, Clone)]
pub struct MatchArm {
    pub pattern: Pattern,
    /// `pattern if cond => ..` -- checked after the pattern matched, so the
    /// guard can use what the pattern bound.
    pub guard: Option<Expr>,
    pub body: Expr,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let(LetDecl),
    Expr(Expr),
}

#[derive(Debug, Clone)]
pub struct LetDecl {
    pub name: String,
    /// Name ends in `!`: this binding is impure.
    pub impure: bool,
    pub rec: bool,
    pub params: Vec<Param>,
    pub body: Expr,
    pub span: Span,
    pub name_span: Span,
}

/// `import std.list;`, `import std.list as l;`, `import std.list.{map, filter};`
/// or `import std;` naming a package rather than a module.
#[derive(Debug, Clone)]
pub struct Import {
    pub path: Vec<String>,
    pub alias: String,
    /// The members named in `.{ .. }`, bound unqualified in the importing
    /// module. Empty for a plain import, which binds the module itself.
    pub only: Vec<ImportName>,
    pub span: Span,
}

/// One name inside `import path.{ .. }`, with the span to point at if it turns
/// out the module does not export it.
#[derive(Debug, Clone)]
pub struct ImportName {
    pub name: String,
    pub alias: String,
    pub span: Span,
}

/// `virtual let area s;` or `virtual let name s = "shape";`
///
/// A virtual declares a hole in a module. A module that derives this one fills
/// it; a default body makes filling it optional.
#[derive(Debug, Clone)]
pub struct VirtualDecl {
    pub name: String,
    pub impure: bool,
    pub params: Vec<Param>,
    pub default: Option<Expr>,
    pub span: Span,
    pub name_span: Span,
}

/// `derive std.shape;` -- take the base module's code, with this module's
/// implementations filling its virtuals.
#[derive(Debug, Clone)]
pub struct Derive {
    pub path: Vec<String>,
    pub alias: String,
    pub span: Span,
}

/// A compile-time condition, as written after `when`.
///
/// The vocabulary is deliberately the language's own: `&&`, `||`, `not` and
/// parentheses mean here what they mean everywhere else, so there is no second
/// expression syntax to learn.
#[derive(Debug, Clone)]
pub enum CfgExpr {
    /// A flag that is either defined or not, like `test`.
    Flag(String),
    /// A setting compared against a value, like `os == "linux"`.
    Equals(String, String),
    Not(Box<CfgExpr>),
    And(Box<CfgExpr>, Box<CfgExpr>),
    Or(Box<CfgExpr>, Box<CfgExpr>),
    Literal(bool),
}

/// `mod util { .. }` -- a module written inside another one.
///
/// It is exactly a module: the loader registers it as `parent.util` and leaves
/// the parent importing it, so a submodule and a file behave the same way.
#[derive(Debug, Clone)]
pub struct ModDecl {
    pub name: String,
    pub items: Vec<Item>,
    pub span: Span,
    pub name_span: Span,
}

#[derive(Debug, Clone)]
pub enum Item {
    Import(Import),
    Mod(ModDecl),
    Derive(Derive),
    Virtual(VirtualDecl),
    Let(LetDecl),
    /// `when <cond> { .. }` -- the items inside exist only when the condition
    /// holds. Resolved before anything is loaded, so an import inside one is
    /// not even followed when it is off.
    When {
        cond: CfgExpr,
        items: Vec<Item>,
        span: Span,
    },
}

#[derive(Debug, Clone)]
pub struct Module {
    pub items: Vec<Item>,
}
