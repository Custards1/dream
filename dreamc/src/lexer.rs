//! Lexer for the Dream language.
//!
//! Dream is newline-sensitive at statement level: inside a block, a newline ends
//! a statement unless the parser is mid-expression or inside a bracket group.
//! Rather than emitting newline tokens we record, for every token, whether a
//! line break appeared in the gap before it. The parser consults that at the
//! optional continuation points (application arguments, infix operators).

use logos::Logos;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Span {
    pub start: u32,
    pub end: u32,
}

impl Span {
    pub fn new(start: usize, end: usize) -> Span {
        Span { start: start as u32, end: end as u32 }
    }
    pub fn to(self, other: Span) -> Span {
        Span { start: self.start, end: other.end }
    }
}

/// Resolve a byte offset to a 1-based (line, column) pair.
pub fn line_col(src: &str, offset: u32) -> (usize, usize) {
    let offset = (offset as usize).min(src.len());
    let mut line = 1;
    let mut col = 1;
    for ch in src[..offset].chars() {
        if ch == '\n' {
            line += 1;
            col = 1;
        } else {
            col += 1;
        }
    }
    (line, col)
}

pub fn line_text(src: &str, offset: u32) -> &str {
    let offset = (offset as usize).min(src.len());
    let start = src[..offset].rfind('\n').map(|i| i + 1).unwrap_or(0);
    let end = src[offset..].find('\n').map(|i| offset + i).unwrap_or(src.len());
    &src[start..end]
}

#[derive(Logos, Debug, Clone, PartialEq)]
#[logos(skip r"[ \t\r\n\f]+")]
#[logos(skip(r"//[^\n]*", allow_greedy = true))]
#[logos(skip r"/\*([^*]|\*+[^*/])*\*+/")]
pub enum Tok {
    // An identifier; a trailing `!` is part of the name and marks impurity.
    #[regex(r"[A-Za-z_][A-Za-z0-9_]*!?", |lex| lex.slice().to_string())]
    Ident(String),

    // `:name` is an atom (symbol) literal.
    #[regex(r":[A-Za-z_][A-Za-z0-9_]*", |lex| lex.slice()[1..].to_string())]
    Atom(String),

    #[regex(r"[0-9][0-9_]*", |lex| parse_int(lex.slice(), 10, 0))]
    #[regex(r"0[xX][0-9a-fA-F_]+", |lex| parse_int(lex.slice(), 16, 2))]
    #[regex(r"0[bB][01_]+", |lex| parse_int(lex.slice(), 2, 2))]
    #[regex(r"0[oO][0-7_]+", |lex| parse_int(lex.slice(), 8, 2))]
    Int(i64),

    #[regex(r"[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9]+)?", |lex| parse_float(lex.slice()))]
    #[regex(r"[0-9][0-9_]*[eE][+-]?[0-9]+", |lex| parse_float(lex.slice()))]
    Float(f64),

    #[regex(r#""([^"\\]|\\.)*""#, |lex| unescape(&lex.slice()[1..lex.slice().len()-1]))]
    Str(String),

    #[regex(r"'([^'\\]|\\.|\\u\{[0-9a-fA-F]+\})'", |lex| {
        let s = lex.slice();
        unescape(&s[1..s.len()-1]).chars().next()
    })]
    Char(char),

    // Bracket openers that introduce a *newline-insensitive* group.
    #[token("(")] LParen,
    #[token("[")] LBracket,
    #[token("$(")] DollarParen,
    #[token("#[")] HashBracket,
    #[token("%{")] PercentBrace,

    #[token(")")] RParen,
    #[token("]")] RBracket,
    #[token("{")] LBrace,
    #[token("}")] RBrace,

    #[token("|>")] PipeInto,
    #[token("->")] Arrow,
    #[token("=>")] FatArrow,
    #[token("==")] EqEq,
    #[token("!=")] BangEq,
    #[token("<=")] Le,
    #[token(">=")] Ge,
    #[token("&&")] AndAnd,
    #[token("||")] OrOr,
    #[token("<")] Lt,
    #[token(">")] Gt,
    #[token("=")] Eq,
    #[token("+")] Plus,
    #[token("-")] Minus,
    #[token("*")] Star,
    #[token("/")] Slash,
    #[token("%")] Percent,
    // Longer first: `..` is the rest of a list pattern, `.` is a member.
    #[token("..")] DotDot,
    #[token(".")] Dot,
    #[token(",")] Comma,
    #[token(";")] Semi,
    #[token(":")] Colon,
}

fn parse_int(s: &str, radix: u32, skip: usize) -> Option<i64> {
    let cleaned: String = s[skip..].chars().filter(|c| *c != '_').collect();
    i64::from_str_radix(&cleaned, radix).ok()
}

fn parse_float(s: &str) -> Option<f64> {
    let cleaned: String = s.chars().filter(|c| *c != '_').collect();
    cleaned.parse().ok()
}

fn unescape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('t') => out.push('\t'),
            Some('r') => out.push('\r'),
            Some('0') => out.push('\0'),
            Some('\\') => out.push('\\'),
            Some('\'') => out.push('\''),
            Some('"') => out.push('"'),
            Some('u') => {
                // \u{XXXX}
                let mut hex = String::new();
                if chars.next() == Some('{') {
                    for c in chars.by_ref() {
                        if c == '}' {
                            break;
                        }
                        hex.push(c);
                    }
                }
                if let Some(ch) = u32::from_str_radix(&hex, 16).ok().and_then(char::from_u32) {
                    out.push(ch);
                }
            }
            Some(other) => out.push(other),
            None => {}
        }
    }
    out
}

#[derive(Debug, Clone)]
pub struct Token {
    pub tok: Tok,
    pub span: Span,
    /// True when a line break separates this token from the previous one.
    pub starts_line: bool,
    /// 1-based column. Only meaningful for a token that starts a line, where
    /// it decides whether the line continues the statement above it.
    pub col: u32,
}

#[derive(Debug)]
pub struct LexError {
    pub span: Span,
}

/// Tokenize the whole source. Returns every token plus a synthetic EOF marker
/// carried by the caller as "index == len".
pub fn lex(src: &str) -> Result<Vec<Token>, LexError> {
    let mut out = Vec::new();
    let mut prev_end = 0usize;
    for (res, range) in Tok::lexer(src).spanned() {
        let tok = res.map_err(|_| LexError { span: Span::new(range.start, range.end) })?;
        let starts_line = src[prev_end..range.start].contains('\n');
        let line_start = src[..range.start].rfind('\n').map(|i| i + 1).unwrap_or(0);
        // Counted in characters rather than bytes, so a line indented with
        // anything non-ASCII still lines up with its neighbours.
        let col = src[line_start..range.start].chars().count() as u32 + 1;
        out.push(Token { tok, span: Span::new(range.start, range.end), starts_line, col });
        prev_end = range.end;
    }
    Ok(out)
}
