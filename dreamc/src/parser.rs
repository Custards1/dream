//! Recursive-descent parser with precedence climbing.
//!
//! Two pieces of context steer the grammar:
//!
//! * `nl_sensitive` — inside blocks and at top level a line break terminates a
//!   statement. It is cleared inside `(`, `[`, `$(`, `#[`, `%{` groups. A line
//!   *starting* with an infix operator, `.`, `else` or `catch` always continues
//!   the previous expression, since none of those can begin a statement.
//! * `no_brace` — while parsing an `if` condition, `{` starts the branch body
//!   rather than a block passed as an argument.

use crate::ast::*;
use crate::diag::Diag;
use crate::lexer::{Span, Tok, Token};

const KEYWORDS: &[&str] = &[
    "let", "rec", "if", "else", "import", "as", "catch", "true", "false", "not", "try!", "fn",
    "virtual", "derive", "comp", "comp!", "when",
];

/// Keywords that can never begin an expression, so they end an application.
const NON_STARTERS: &[&str] =
    &["let", "rec", "else", "import", "as", "catch", "virtual", "derive", "when"];

pub struct Parser<'a> {
    toks: &'a [Token],
    pos: usize,
    end: Span,
    nl_sensitive: bool,
    no_brace: bool,
    pub diags: Vec<Diag>,
}

type PResult<T> = Result<T, Diag>;

impl<'a> Parser<'a> {
    pub fn new(toks: &'a [Token], src_len: usize) -> Parser<'a> {
        Parser {
            toks,
            pos: 0,
            end: Span::new(src_len, src_len),
            nl_sensitive: true,
            no_brace: false,
            diags: Vec::new(),
        }
    }

    // ---- token access -----------------------------------------------------

    fn peek(&self) -> Option<&'a Token> {
        self.toks.get(self.pos)
    }
    fn peek_at(&self, n: usize) -> Option<&'a Token> {
        self.toks.get(self.pos + n)
    }
    fn span(&self) -> Span {
        self.peek().map(|t| t.span).unwrap_or(self.end)
    }
    fn prev_span(&self) -> Span {
        if self.pos == 0 { self.end } else { self.toks[self.pos - 1].span }
    }
    fn bump(&mut self) -> Option<&'a Token> {
        let t = self.toks.get(self.pos);
        if t.is_some() {
            self.pos += 1;
        }
        t
    }
    fn at(&self, t: &Tok) -> bool {
        self.peek()
            .is_some_and(|tk| std::mem::discriminant(&tk.tok) == std::mem::discriminant(t))
    }
    fn eat(&mut self, t: &Tok) -> bool {
        if self.at(t) {
            self.pos += 1;
            true
        } else {
            false
        }
    }
    fn expect(&mut self, t: &Tok, what: &str) -> PResult<Span> {
        if self.at(t) {
            let s = self.span();
            self.pos += 1;
            Ok(s)
        } else {
            Err(Diag::error(self.span(), format!("expected {what}, found {}", self.describe())))
        }
    }
    fn describe(&self) -> String {
        match self.peek() {
            None => "end of file".to_string(),
            Some(t) => match &t.tok {
                Tok::Ident(s) => format!("`{s}`"),
                Tok::Atom(s) => format!("atom `:{s}`"),
                Tok::Int(v) => format!("integer `{v}`"),
                Tok::Float(v) => format!("float `{v}`"),
                Tok::Str(_) => "a string literal".to_string(),
                Tok::Char(_) => "a char literal".to_string(),
                other => format!("`{}`", tok_text(other)),
            },
        }
    }

    fn ident_str(&self) -> Option<&'a str> {
        match self.peek().map(|t| &t.tok) {
            Some(Tok::Ident(s)) => Some(s.as_str()),
            _ => None,
        }
    }
    fn is_kw(&self, kw: &str) -> bool {
        self.ident_str() == Some(kw)
    }
    fn eat_kw(&mut self, kw: &str) -> bool {
        if self.is_kw(kw) {
            self.pos += 1;
            true
        } else {
            false
        }
    }
    fn expect_ident(&mut self, what: &str) -> PResult<(String, Span)> {
        match self.peek() {
            Some(Token { tok: Tok::Ident(s), span, .. }) if !KEYWORDS.contains(&s.as_str()) => {
                self.pos += 1;
                Ok((s.clone(), *span))
            }
            _ => Err(Diag::error(self.span(), format!("expected {what}, found {}", self.describe()))),
        }
    }

    /// May the current token continue the expression under construction?
    fn continues(&self) -> bool {
        match self.peek() {
            None => false,
            Some(t) => !(self.nl_sensitive && t.starts_line),
        }
    }

    // ---- items ------------------------------------------------------------

    pub fn parse_module(&mut self) -> Module {
        let mut items = Vec::new();
        while self.peek().is_some() {
            while self.eat(&Tok::Semi) {}
            if self.peek().is_none() {
                break;
            }
            let result = if self.is_kw("import") {
                self.parse_import().map(Item::Import)
            } else if self.is_kw("derive") {
                self.parse_derive().map(Item::Derive)
            } else if self.is_kw("virtual") {
                self.parse_virtual().map(Item::Virtual)
            } else if self.is_kw("when") {
                self.parse_when()
            } else if self.is_kw("let") {
                self.parse_let_decl().map(Item::Let)
            } else {
                Err(Diag::error(
                    self.span(),
                    format!(
                        "expected `import`, `derive`, `virtual` or `let` at top level, found {}",
                        self.describe()
                    ),
                ))
            };
            match result {
                Ok(item) => {
                    items.push(item);
                    self.eat(&Tok::Semi);
                }
                Err(d) => {
                    self.diags.push(d);
                    self.sync();
                }
            }
        }
        Module { items }
    }

    /// Skip forward to the next plausible top-level item so one syntax error
    /// does not swallow the rest of the file.
    fn sync(&mut self) {
        loop {
            match self.peek() {
                None => return,
                Some(t) => {
                    let at_item = t.starts_line
                        && matches!(&t.tok, Tok::Ident(s) if s == "let" || s == "import"
                                    || s == "derive" || s == "virtual" || s == "when");
                    if at_item {
                        return;
                    }
                    self.pos += 1;
                }
            }
        }
    }

    fn parse_import(&mut self) -> PResult<Import> {
        let start = self.span();
        self.pos += 1; // `import`
        let mut path = vec![self.expect_ident("a module name")?.0];
        while self.eat(&Tok::Dot) {
            path.push(self.expect_ident("a module path segment")?.0);
        }
        let alias = if self.eat_kw("as") {
            self.expect_ident("an alias name")?.0
        } else {
            path.last().cloned().unwrap_or_default()
        };
        Ok(Import { path, alias, span: start.to(self.prev_span()) })
    }

    /// `when <cond> { items }`, or `when <cond> <item>` for a single one.
    fn parse_when(&mut self) -> PResult<Item> {
        let start = self.span();
        self.pos += 1; // `when`
        // The condition is an expression-shaped thing, so parsing it must not
        // mistake the opening brace of the body for part of it.
        let saved = self.no_brace;
        self.no_brace = true;
        let cond = self.parse_cfg_or();
        self.no_brace = saved;
        let cond = cond?;

        let mut items = Vec::new();
        if self.eat(&Tok::LBrace) {
            let (nl, nb) = (self.nl_sensitive, self.no_brace);
            self.nl_sensitive = true;
            self.no_brace = false;
            while !self.at(&Tok::RBrace) && self.peek().is_some() {
                while self.eat(&Tok::Semi) {}
                if self.at(&Tok::RBrace) {
                    break;
                }
                items.push(self.parse_conditional_item()?);
                self.eat(&Tok::Semi);
            }
            self.nl_sensitive = nl;
            self.no_brace = nb;
            self.expect(&Tok::RBrace, "`}` closing the `when` block")?;
        } else {
            items.push(self.parse_conditional_item()?);
            self.eat(&Tok::Semi);
        }
        Ok(Item::When { cond, items, span: start.to(self.prev_span()) })
    }

    /// The items a `when` may contain -- which is any item, including another
    /// `when`, so conditions nest.
    fn parse_conditional_item(&mut self) -> PResult<Item> {
        if self.is_kw("import") {
            self.parse_import().map(Item::Import)
        } else if self.is_kw("derive") {
            self.parse_derive().map(Item::Derive)
        } else if self.is_kw("virtual") {
            self.parse_virtual().map(Item::Virtual)
        } else if self.is_kw("when") {
            self.parse_when()
        } else if self.is_kw("let") {
            self.parse_let_decl().map(Item::Let)
        } else {
            Err(Diag::error(
                self.span(),
                format!("expected a declaration inside `when`, found {}", self.describe()),
            ))
        }
    }

    fn parse_cfg_or(&mut self) -> PResult<CfgExpr> {
        let mut lhs = self.parse_cfg_and()?;
        while self.at(&Tok::OrOr) {
            self.pos += 1;
            let rhs = self.parse_cfg_and()?;
            lhs = CfgExpr::Or(Box::new(lhs), Box::new(rhs));
        }
        Ok(lhs)
    }

    fn parse_cfg_and(&mut self) -> PResult<CfgExpr> {
        let mut lhs = self.parse_cfg_unary()?;
        while self.at(&Tok::AndAnd) {
            self.pos += 1;
            let rhs = self.parse_cfg_unary()?;
            lhs = CfgExpr::And(Box::new(lhs), Box::new(rhs));
        }
        Ok(lhs)
    }

    fn parse_cfg_unary(&mut self) -> PResult<CfgExpr> {
        if self.is_kw("not") {
            self.pos += 1;
            return Ok(CfgExpr::Not(Box::new(self.parse_cfg_unary()?)));
        }
        if self.eat(&Tok::LParen) {
            let inner = self.parse_cfg_or()?;
            self.expect(&Tok::RParen, "`)`")?;
            return Ok(inner);
        }
        match self.peek() {
            Some(Token { tok: Tok::Ident(name), .. }) => {
                let name = name.clone();
                self.pos += 1;
                if name == "true" {
                    return Ok(CfgExpr::Literal(true));
                }
                if name == "false" {
                    return Ok(CfgExpr::Literal(false));
                }
                // `os == "linux"` compares a setting against a value.
                if self.at(&Tok::EqEq) {
                    self.pos += 1;
                    let Some(Token { tok: Tok::Str(value), .. }) = self.peek() else {
                        return Err(Diag::error(
                            self.span(),
                            "a `when` comparison needs a string on the right",
                        ));
                    };
                    let value = value.clone();
                    self.pos += 1;
                    return Ok(CfgExpr::Equals(name, value));
                }
                Ok(CfgExpr::Flag(name))
            }
            _ => Err(Diag::error(
                self.span(),
                format!("expected a condition after `when`, found {}", self.describe()),
            )),
        }
    }

    fn parse_derive(&mut self) -> PResult<Derive> {
        let start = self.span();
        self.pos += 1; // `derive`
        let mut path = vec![self.expect_ident("a module name")?.0];
        while self.eat(&Tok::Dot) {
            path.push(self.expect_ident("a module path segment")?.0);
        }
        let alias = if self.eat_kw("as") {
            self.expect_ident("an alias name")?.0
        } else {
            path.last().cloned().unwrap_or_default()
        };
        Ok(Derive { path, alias, span: start.to(self.prev_span()) })
    }

    fn parse_virtual(&mut self) -> PResult<VirtualDecl> {
        let start = self.span();
        self.pos += 1; // `virtual`
        if !self.is_kw("let") {
            return Err(Diag::error(self.span(), "expected `let` after `virtual`")
                .with_note("a virtual is declared as `virtual let name params;`"));
        }
        let decl_start = self.span();
        self.pos += 1; // `let`
        let (name, name_span) = self.expect_ident("a virtual's name")?;
        let impure = name.ends_with('!');
        let params = self.parse_params()?;
        if params.is_empty() {
            return Err(Diag::error(decl_start, "a virtual needs at least one parameter")
                .with_note("a parameterless virtual would be a constant, not a hole"));
        }
        // A body is the default used when a deriving module does not override.
        let default = if self.eat(&Tok::Eq) { Some(self.parse_expr()?) } else { None };
        Ok(VirtualDecl {
            name,
            impure,
            params,
            default,
            span: start.to(self.prev_span()),
            name_span,
        })
    }

    /// The parameter list shared by `let` and `virtual let`.
    fn parse_params(&mut self) -> PResult<Vec<Param>> {
        let mut params = Vec::new();
        loop {
            if self.at(&Tok::LParen) {
                self.pos += 1;
                if self.eat(&Tok::RParen) {
                    // `()` -- a unit parameter, occupying a slot but unnamed.
                    params.push(Param { name: "_".into(), span: self.prev_span() });
                } else {
                    loop {
                        let (p, sp) = self.expect_ident("a parameter name")?;
                        params.push(Param { name: p, span: sp });
                        if !self.eat(&Tok::Comma) {
                            break;
                        }
                    }
                    self.expect(&Tok::RParen, "`)`")?;
                }
            } else if let Some(s) = self.ident_str() {
                if KEYWORDS.contains(&s) {
                    break;
                }
                let (p, sp) = self.expect_ident("a parameter name")?;
                params.push(Param { name: p, span: sp });
            } else {
                break;
            }
        }
        Ok(params)
    }

    fn parse_let_decl(&mut self) -> PResult<LetDecl> {
        let start = self.span();
        self.pos += 1; // `let`
        let rec = self.eat_kw("rec");
        let (name, name_span) = self.expect_ident("a binding name")?;
        let impure = name.ends_with('!');

        let params = self.parse_params()?;
        self.expect(&Tok::Eq, "`=` after the binding head")?;
        let body = self.parse_expr()?;
        Ok(LetDecl { name, impure, rec, params, body, span: start.to(self.prev_span()), name_span })
    }

    // ---- expressions ------------------------------------------------------

    pub fn parse_expr(&mut self) -> PResult<Expr> {
        self.parse_pipe()
    }

    fn parse_pipe(&mut self) -> PResult<Expr> {
        let mut lhs = self.parse_binary(0)?;
        while self.at(&Tok::PipeInto) {
            self.pos += 1;
            let rhs = self.parse_binary(0)?;
            let span = lhs.span.to(rhs.span);
            lhs = Expr { kind: ExprKind::Pipe(Box::new(lhs), Box::new(rhs)), span };
        }
        Ok(lhs)
    }

    /// Precedence climbing over the infix operators. Level 0 is `||`.
    fn parse_binary(&mut self, min_level: u8) -> PResult<Expr> {
        let mut lhs = self.parse_unary()?;
        loop {
            let Some((op, level)) = self.peek().and_then(|t| infix_op(&t.tok)) else { break };
            if level < min_level {
                break;
            }
            self.pos += 1;
            // All infix operators are left-associative.
            let rhs = self.parse_binary(level + 1)?;
            let span = lhs.span.to(rhs.span);
            lhs = Expr { kind: ExprKind::Binary(op, Box::new(lhs), Box::new(rhs)), span };
        }
        Ok(lhs)
    }

    fn parse_unary(&mut self) -> PResult<Expr> {
        let start = self.span();
        if self.at(&Tok::Minus) {
            self.pos += 1;
            let e = self.parse_unary()?;
            let span = start.to(e.span);
            return Ok(Expr { kind: ExprKind::Unary(UnOp::Neg, Box::new(e)), span });
        }
        if self.is_kw("not") {
            self.pos += 1;
            let e = self.parse_unary()?;
            let span = start.to(e.span);
            return Ok(Expr { kind: ExprKind::Unary(UnOp::Not, Box::new(e)), span });
        }
        self.parse_apply()
    }

    fn parse_apply(&mut self) -> PResult<Expr> {
        let head = self.parse_postfix()?;
        let mut args = Vec::new();
        while self.can_start_arg() {
            args.push(self.parse_postfix()?);
        }
        if args.is_empty() {
            return Ok(head);
        }
        let span = head.span.to(self.prev_span());
        Ok(Expr { kind: ExprKind::Apply(Box::new(head), args), span })
    }

    fn can_start_arg(&self) -> bool {
        if !self.continues() {
            return false;
        }
        match self.peek().map(|t| &t.tok) {
            Some(Tok::Ident(s)) => !NON_STARTERS.contains(&s.as_str()),
            Some(
                Tok::Int(_)
                | Tok::Float(_)
                | Tok::Str(_)
                | Tok::Char(_)
                | Tok::Atom(_)
                | Tok::LParen
                | Tok::LBracket
                | Tok::DollarParen
                | Tok::HashBracket
                | Tok::PercentBrace,
            ) => true,
            Some(Tok::LBrace) => !self.no_brace,
            _ => false,
        }
    }

    fn parse_postfix(&mut self) -> PResult<Expr> {
        let mut e = self.parse_primary()?;
        // A leading `.` on a new line continues the expression.
        while self.at(&Tok::Dot) && matches!(self.peek_at(1).map(|t| &t.tok), Some(Tok::Ident(_))) {
            self.pos += 1;
            let Some(Token { tok: Tok::Ident(name), span, .. }) = self.bump() else {
                unreachable!()
            };
            let full = e.span.to(*span);
            e = Expr { kind: ExprKind::Field(Box::new(e), name.clone()), span: full };
        }
        Ok(e)
    }

    /// Run `f` with newline sensitivity disabled (inside a bracket group).
    fn grouped<T>(&mut self, f: impl FnOnce(&mut Self) -> PResult<T>) -> PResult<T> {
        let (nl, nb) = (self.nl_sensitive, self.no_brace);
        self.nl_sensitive = false;
        self.no_brace = false;
        let r = f(self);
        self.nl_sensitive = nl;
        self.no_brace = nb;
        r
    }

    fn parse_primary(&mut self) -> PResult<Expr> {
        let start = self.span();
        let Some(t) = self.peek() else {
            return Err(Diag::error(self.end, "unexpected end of file in expression"));
        };
        let kind = match &t.tok {
            Tok::Int(v) => {
                let v = *v;
                self.pos += 1;
                ExprKind::Int(v)
            }
            Tok::Float(v) => {
                let v = *v;
                self.pos += 1;
                ExprKind::Float(v)
            }
            Tok::Str(s) => {
                let s = s.clone();
                self.pos += 1;
                ExprKind::Str(s)
            }
            Tok::Char(c) => {
                let c = *c;
                self.pos += 1;
                ExprKind::Char(c)
            }
            Tok::Atom(s) => {
                let s = s.clone();
                self.pos += 1;
                ExprKind::Atom(s)
            }
            Tok::Ident(s) => match s.as_str() {
                "true" => {
                    self.pos += 1;
                    ExprKind::Bool(true)
                }
                "false" => {
                    self.pos += 1;
                    ExprKind::Bool(false)
                }
                "if" => return self.parse_if(),
                "try!" => return self.parse_try(),
                "fn" => return self.parse_lambda(),
                "comp" | "comp!" => {
                    let impure = s == "comp!";
                    let start = self.span();
                    self.pos += 1;
                    // `comp` binds tighter than any operator but looser than
                    // application, so `comp f x` folds the whole call.
                    let inner = self.parse_apply()?;
                    let span = start.to(self.prev_span());
                    return Ok(Expr {
                        kind: ExprKind::Comp { impure, expr: Box::new(inner) },
                        span,
                    });
                }
                other if NON_STARTERS.contains(&other) => {
                    return Err(Diag::error(
                        start,
                        format!("`{other}` cannot start an expression"),
                    ));
                }
                _ => {
                    let s = s.clone();
                    self.pos += 1;
                    ExprKind::Name(s)
                }
            },
            Tok::LParen => {
                self.pos += 1;
                if self.eat(&Tok::RParen) {
                    ExprKind::Unit
                } else {
                    let inner = self.grouped(|p| p.parse_expr())?;
                    self.expect(&Tok::RParen, "`)`")?;
                    return Ok(Expr { kind: inner.kind, span: start.to(self.prev_span()) });
                }
            }
            Tok::DollarParen => {
                self.pos += 1;
                let inner = self.grouped(|p| p.parse_expr())?;
                self.expect(&Tok::RParen, "`)` closing the thunk")?;
                ExprKind::Thunk(Box::new(inner))
            }
            Tok::LBracket => {
                self.pos += 1;
                let items = self.parse_comma_list(&Tok::RBracket, "`]`")?;
                ExprKind::List(items)
            }
            Tok::HashBracket => {
                self.pos += 1;
                let items = self.parse_comma_list(&Tok::RBracket, "`]`")?;
                ExprKind::Array(items)
            }
            Tok::PercentBrace => {
                self.pos += 1;
                let pairs = self.grouped(|p| {
                    let mut pairs = Vec::new();
                    while !p.at(&Tok::RBrace) && p.peek().is_some() {
                        let k = p.parse_expr()?;
                        p.expect(&Tok::FatArrow, "`=>` between map key and value")?;
                        let v = p.parse_expr()?;
                        pairs.push((k, v));
                        if !p.eat(&Tok::Comma) {
                            break;
                        }
                    }
                    Ok(pairs)
                })?;
                self.expect(&Tok::RBrace, "`}` closing the map")?;
                ExprKind::Map(pairs)
            }
            Tok::LBrace => return self.parse_block(),
            other => {
                return Err(Diag::error(
                    start,
                    format!("expected an expression, found `{}`", tok_text(other)),
                ));
            }
        };
        Ok(Expr { kind, span: start.to(self.prev_span()) })
    }

    fn parse_comma_list(&mut self, close: &Tok, what: &str) -> PResult<Vec<Expr>> {
        let items = self.grouped(|p| {
            let mut items = Vec::new();
            while !p.at(close) && p.peek().is_some() {
                items.push(p.parse_expr()?);
                if !p.eat(&Tok::Comma) {
                    break;
                }
            }
            Ok(items)
        })?;
        self.expect(close, what)?;
        Ok(items)
    }

    fn parse_if(&mut self) -> PResult<Expr> {
        let start = self.span();
        self.pos += 1; // `if`
        let saved = self.no_brace;
        self.no_brace = true;
        let cond = self.parse_expr();
        self.no_brace = saved;
        let cond = cond?;

        let then = self.parse_block()?;
        // `else` can never start a statement, so accept it across a newline.
        let els = if self.is_kw("else") {
            self.pos += 1;
            if self.is_kw("if") {
                Some(Box::new(self.parse_if()?))
            } else {
                Some(Box::new(self.parse_block()?))
            }
        } else {
            None
        };
        let span = start.to(self.prev_span());
        Ok(Expr { kind: ExprKind::If(Box::new(cond), Box::new(then), els), span })
    }

    fn parse_try(&mut self) -> PResult<Expr> {
        let start = self.span();
        self.pos += 1; // `try!`
        let body = self.parse_block()?;
        if !self.is_kw("catch") {
            return Err(Diag::error(self.span(), "expected `catch` after the `try!` body")
                .with_note("every `try!` must name a handler: `try! { .. } catch e { .. }`"));
        }
        self.pos += 1;
        let binder = match self.peek() {
            Some(Token { tok: Tok::Ident(s), .. }) if !KEYWORDS.contains(&s.as_str()) => {
                let s = s.clone();
                self.pos += 1;
                s
            }
            _ => return Err(Diag::error(self.span(), "expected a name to bind the caught error to")),
        };
        let handler = self.parse_block()?;
        let span = start.to(self.prev_span());
        Ok(Expr {
            kind: ExprKind::Try { body: Box::new(body), binder, handler: Box::new(handler) },
            span,
        })
    }

    fn parse_lambda(&mut self) -> PResult<Expr> {
        let start = self.span();
        self.pos += 1; // `fn`
        let mut params = Vec::new();
        while let Some(s) = self.ident_str() {
            if KEYWORDS.contains(&s) {
                break;
            }
            let (p, sp) = self.expect_ident("a parameter name")?;
            params.push(Param { name: p, span: sp });
        }
        if params.is_empty() {
            return Err(Diag::error(start, "`fn` needs at least one parameter"));
        }
        self.expect(&Tok::Arrow, "`->` after the lambda parameters")?;
        let body = self.parse_expr()?;
        let span = start.to(self.prev_span());
        Ok(Expr { kind: ExprKind::Lambda { params, body: Box::new(body) }, span })
    }

    fn parse_block(&mut self) -> PResult<Expr> {
        let start = self.expect(&Tok::LBrace, "`{`")?;
        let (nl, nb) = (self.nl_sensitive, self.no_brace);
        self.nl_sensitive = true;
        self.no_brace = false;

        let mut stmts = Vec::new();
        let result = loop {
            while self.eat(&Tok::Semi) {}
            if self.at(&Tok::RBrace) || self.peek().is_none() {
                break Ok(());
            }
            let stmt = if self.is_kw("let") {
                self.parse_let_decl().map(Stmt::Let)
            } else {
                self.parse_expr().map(Stmt::Expr)
            };
            match stmt {
                Ok(s) => stmts.push(s),
                Err(e) => break Err(e),
            }
            // A statement ends at `;`, `}`, or a line break.
            let terminated = self.at(&Tok::Semi)
                || self.at(&Tok::RBrace)
                || self.peek().is_none_or(|t| t.starts_line);
            if !terminated {
                break Err(Diag::error(
                    self.span(),
                    format!("expected `;` or a new line to end the statement, found {}", self.describe()),
                ));
            }
            self.eat(&Tok::Semi);
        };

        self.nl_sensitive = nl;
        self.no_brace = nb;
        result?;
        // The statement loop only stops at `}` or end of input, so getting here
        // without a brace means the block was never closed. Point at the `{`,
        // which is where the reader has to look, rather than at the far end of
        // the file.
        if !self.at(&Tok::RBrace) {
            return Err(Diag::error(start, "this block is never closed")
                .with_note("expected a matching `}`"));
        }
        self.pos += 1;
        Ok(Expr { kind: ExprKind::Block(stmts), span: start.to(self.prev_span()) })
    }
}

/// `(operator, precedence)` — higher binds tighter. `|>` sits below all of these.
fn infix_op(t: &Tok) -> Option<(BinOp, u8)> {
    Some(match t {
        Tok::OrOr => (BinOp::Or, 0),
        Tok::AndAnd => (BinOp::And, 1),
        Tok::EqEq => (BinOp::Eq, 2),
        Tok::BangEq => (BinOp::Ne, 2),
        Tok::Lt => (BinOp::Lt, 2),
        Tok::Le => (BinOp::Le, 2),
        Tok::Gt => (BinOp::Gt, 2),
        Tok::Ge => (BinOp::Ge, 2),
        Tok::Plus => (BinOp::Add, 3),
        Tok::Minus => (BinOp::Sub, 3),
        Tok::Star => (BinOp::Mul, 4),
        Tok::Slash => (BinOp::Div, 4),
        Tok::Percent => (BinOp::Mod, 4),
        _ => return None,
    })
}

fn tok_text(t: &Tok) -> &'static str {
    match t {
        Tok::LParen => "(",
        Tok::RParen => ")",
        Tok::LBrace => "{",
        Tok::RBrace => "}",
        Tok::LBracket => "[",
        Tok::RBracket => "]",
        Tok::DollarParen => "$(",
        Tok::HashBracket => "#[",
        Tok::PercentBrace => "%{",
        Tok::PipeInto => "|>",
        Tok::Arrow => "->",
        Tok::FatArrow => "=>",
        Tok::EqEq => "==",
        Tok::BangEq => "!=",
        Tok::Le => "<=",
        Tok::Ge => ">=",
        Tok::AndAnd => "&&",
        Tok::OrOr => "||",
        Tok::Lt => "<",
        Tok::Gt => ">",
        Tok::Eq => "=",
        Tok::Plus => "+",
        Tok::Minus => "-",
        Tok::Star => "*",
        Tok::Slash => "/",
        Tok::Percent => "%",
        Tok::Dot => ".",
        Tok::Comma => ",",
        Tok::Semi => ";",
        Tok::Colon => ":",
        _ => "<token>",
    }
}
