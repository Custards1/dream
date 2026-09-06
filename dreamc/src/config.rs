//! Compile-time configuration: what `when` conditions are tested against.
//!
//! A flag is either defined or not (`test`), and a setting has a value
//! (`os == "linux"`). Both come from the command line, except for a handful the
//! compiler knows about itself.
//!
//! Conditions are resolved while loading, before anything is compiled, so a
//! `when` that is off costs nothing -- an `import` inside it is not followed,
//! and a module only test builds need is never even read.

use std::collections::{HashMap, HashSet};

use crate::ast::CfgExpr;

#[derive(Debug, Clone)]
pub struct Config {
    flags: HashSet<String>,
    values: HashMap<String, String>,
}

impl Default for Config {
    fn default() -> Config {
        Config::new()
    }
}

impl Config {
    /// The settings the compiler knows without being told.
    pub fn new() -> Config {
        let mut values = HashMap::new();
        values.insert("os".to_string(), std::env::consts::OS.to_string());
        values.insert("arch".to_string(), std::env::consts::ARCH.to_string());
        values.insert("family".to_string(), std::env::consts::FAMILY.to_string());
        values.insert("dream_version".to_string(), env!("CARGO_PKG_VERSION").to_string());
        Config { flags: HashSet::new(), values }
    }

    pub fn define(&mut self, name: &str) {
        self.flags.insert(name.to_string());
    }

    pub fn set(&mut self, key: &str, value: &str) {
        self.values.insert(key.to_string(), value.to_string());
    }

    /// Accepts `name` or `name=value`, which is what `-D` takes.
    pub fn define_arg(&mut self, arg: &str) {
        match arg.split_once('=') {
            Some((k, v)) => self.set(k, v),
            None => self.define(arg),
        }
    }

    pub fn is_defined(&self, name: &str) -> bool {
        self.flags.contains(name)
    }

    pub fn value(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(|s| s.as_str())
    }

    /// Every flag and setting, for `--print-cfg`.
    pub fn describe(&self) -> Vec<String> {
        let mut out: Vec<String> = self.flags.iter().cloned().collect();
        for (k, v) in &self.values {
            out.push(format!("{k} = {v:?}"));
        }
        out.sort();
        out
    }

    pub fn eval(&self, cond: &CfgExpr) -> bool {
        match cond {
            CfgExpr::Literal(b) => *b,
            // A setting that has a value counts as defined, so `when os { .. }`
            // is true and `when nonsense { .. }` is false.
            CfgExpr::Flag(name) => self.flags.contains(name) || self.values.contains_key(name),
            CfgExpr::Equals(key, want) => self.values.get(key).is_some_and(|v| v == want),
            CfgExpr::Not(inner) => !self.eval(inner),
            CfgExpr::And(a, b) => self.eval(a) && self.eval(b),
            CfgExpr::Or(a, b) => self.eval(a) || self.eval(b),
        }
    }
}
