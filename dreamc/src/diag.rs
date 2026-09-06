//! Diagnostics with source-mapped rendering.

use crate::lexer::{Span, line_col, line_text};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Level {
    Error,
    Warning,
}

#[derive(Debug, Clone)]
pub struct Diag {
    pub level: Level,
    pub span: Span,
    pub message: String,
    pub note: Option<String>,
    /// Which source file the span belongs to. A program is many files now, so
    /// a span alone no longer says where it is.
    pub source: usize,
}

/// One source file, for rendering diagnostics across a whole program.
#[derive(Debug, Clone)]
pub struct SourceFile {
    pub path: String,
    pub text: String,
}

impl Diag {
    pub fn error(span: Span, message: impl Into<String>) -> Diag {
        Diag { level: Level::Error, span, message: message.into(), note: None, source: 0 }
    }
    pub fn warning(span: Span, message: impl Into<String>) -> Diag {
        Diag { level: Level::Warning, span, message: message.into(), note: None, source: 0 }
    }
    pub fn with_note(mut self, note: impl Into<String>) -> Diag {
        self.note = Some(note.into());
        self
    }
    pub fn in_source(mut self, source: usize) -> Diag {
        self.source = source;
        self
    }

    pub fn render(&self, src: &str, path: &str) -> String {
        let (line, col) = line_col(src, self.span.start);
        let text = line_text(src, self.span.start);
        let label = match self.level {
            Level::Error => "error",
            Level::Warning => "warning",
        };
        let gutter = format!("{line}");
        let pad = " ".repeat(gutter.len());
        let width = {
            let end = line_col(src, self.span.end);
            if end.0 == line { (end.1 - col).max(1) } else { 1 }
        };
        let mut out = format!(
            "{label}: {msg}\n{pad}--> {path}:{line}:{col}\n{pad} |\n{gutter} | {text}\n{pad} | {marker}{carets}\n",
            msg = self.message,
            marker = " ".repeat(col.saturating_sub(1)),
            carets = "^".repeat(width),
        );
        if let Some(note) = &self.note {
            out.push_str(&format!("{pad} = note: {note}\n"));
        }
        out
    }
}

pub fn render_all(diags: &[Diag], src: &str, path: &str) -> String {
    diags.iter().map(|d| d.render(src, path)).collect::<Vec<_>>().join("\n")
}

/// Render diagnostics that may come from any file in the program.
pub fn render_program(diags: &[Diag], files: &[SourceFile]) -> String {
    diags
        .iter()
        .map(|d| match files.get(d.source) {
            Some(f) => d.render(&f.text, &f.path),
            // A diagnostic with no file behind it still has to carry
            // everything it knows, including its note.
            None => {
                let label = match d.level {
                    Level::Error => "error",
                    Level::Warning => "warning",
                };
                match &d.note {
                    Some(n) => format!("{label}: {}\n = note: {n}\n", d.message),
                    None => format!("{label}: {}\n", d.message),
                }
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}
