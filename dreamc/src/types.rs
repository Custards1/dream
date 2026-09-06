//! The language's built-in type spec.
//!
//! This is the single source of truth for what types Dream has. The compiler,
//! the runtime's `type_of`, and the documentation all have to agree, and prose
//! in three places drifts; a table that the tests check does not.
//!
//! Dream has four scalar types and one umbrella type, `object`, whose kinds are
//! listed below. `type_of` returns the name as an atom.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TypeKind {
    /// An immediate value, stored in the value word itself.
    Scalar,
    /// The umbrella type all object kinds belong to.
    Umbrella,
    /// A heap-allocated object.
    Object,
}

#[derive(Debug, Clone, Copy)]
pub struct BuiltinType {
    /// The name `type_of` returns, as an atom.
    pub name: &'static str,
    pub kind: TypeKind,
    /// Other accepted spellings. `thread` is kept for `process` because the
    /// original language spec used that word.
    pub aliases: &'static [&'static str],
    pub doc: &'static str,
}

use TypeKind::{Object, Scalar, Umbrella};

/// Every built-in type, in spec order.
pub const BUILTIN_TYPES: &[BuiltinType] = &[
    BuiltinType {
        name: "integer",
        kind: Scalar,
        aliases: &[],
        doc: "A signed integer.",
    },
    BuiltinType {
        name: "float",
        kind: Scalar,
        aliases: &[],
        doc: "A double-precision floating-point number.",
    },
    BuiltinType {
        name: "char",
        kind: Scalar,
        aliases: &[],
        doc: "A single Unicode scalar value.",
    },
    BuiltinType {
        name: "bool",
        kind: Scalar,
        aliases: &[],
        doc: "`true` or `false`.",
    },
    BuiltinType {
        name: "unit",
        kind: Scalar,
        aliases: &[],
        doc: "`()`, the value of an expression that has nothing to say.",
    },
    BuiltinType {
        name: "object",
        kind: Umbrella,
        aliases: &[],
        doc: "Anything with identity and a heap representation; see the kinds below.",
    },
    BuiltinType {
        name: "pure_fn",
        kind: Object,
        aliases: &[],
        doc: "A function with no effects. Callable from anywhere.",
    },
    BuiltinType {
        name: "impure_fn",
        kind: Object,
        aliases: &[],
        doc: "A function whose name ends in `!`. Only reachable from other impure functions.",
    },
    BuiltinType {
        name: "module",
        kind: Object,
        aliases: &[],
        doc: "A named collection of members, produced by `import`.",
    },
    BuiltinType {
        name: "list",
        kind: Object,
        aliases: &[],
        doc: "A cons list, `[a, b, c]`. Both head and tail are lazy.",
    },
    BuiltinType {
        name: "array",
        kind: Object,
        aliases: &[],
        doc: "A flat sequence with constant-time indexing, `#[a, b, c]`.",
    },
    BuiltinType {
        name: "map",
        kind: Object,
        aliases: &[],
        doc: "A hash map, `%{ k => v }`. Keys are forced; values stay lazy.",
    },
    BuiltinType {
        name: "error",
        kind: Object,
        aliases: &[],
        doc: "A raised value: a kind atom and a payload. Caught by `try! .. catch`.",
    },
    BuiltinType {
        name: "process",
        kind: Object,
        aliases: &["thread"],
        doc: "A green process: an isolated heap, a mailbox, and a scheduler slot. \
              Created by `spawn!`, addressed by `send!`, awaited by `join!`. \
              Processes share no memory -- messages are copied -- so a process \
              is the unit of concurrency, of failure, and of collection.",
    },
    BuiltinType {
        name: "atom",
        kind: Object,
        aliases: &["symbol"],
        doc: "An interned name, written `:like_this`. Compares by identity.",
    },
    BuiltinType {
        name: "string",
        kind: Object,
        aliases: &[],
        doc: "A UTF-8 string.",
    },
];

/// Look up a type by its name or any alias.
pub fn find(name: &str) -> Option<&'static BuiltinType> {
    BUILTIN_TYPES
        .iter()
        .find(|t| t.name == name || t.aliases.contains(&name))
}

/// The canonical name for a type, resolving aliases: `thread` -> `process`.
pub fn canonical(name: &str) -> Option<&'static str> {
    find(name).map(|t| t.name)
}

/// The object kinds, in spec order.
pub fn object_kinds() -> impl Iterator<Item = &'static BuiltinType> {
    BUILTIN_TYPES.iter().filter(|t| t.kind == TypeKind::Object)
}

/// The operations that create or address a process. These are all impure by
/// name, so the purity checker already forbids reaching them from a pure
/// function -- concurrency is an effect.
pub const PROCESS_OPS: &[&str] = &["spawn!", "join!", "send!", "recv!", "self!"];

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn every_name_and_alias_is_unique() {
        let mut seen = HashSet::new();
        for t in BUILTIN_TYPES {
            assert!(seen.insert(t.name), "duplicate type name `{}`", t.name);
            for a in t.aliases {
                assert!(seen.insert(a), "alias `{a}` collides with another type name");
            }
        }
    }

    #[test]
    fn the_original_spec_names_all_resolve() {
        // The list the language was specified with, including `thread`, which
        // is now an alias for `process`.
        for name in [
            "integer", "float", "char", "bool", "object", "impure_fn", "pure_fn", "module",
            "list", "array", "map", "error", "thread", "atom", "string",
        ] {
            assert!(find(name).is_some(), "`{name}` is not in the type spec");
        }
    }

    #[test]
    fn thread_is_an_alias_for_process() {
        assert_eq!(canonical("thread"), Some("process"));
        assert_eq!(canonical("process"), Some("process"));
    }

    #[test]
    fn process_is_an_object_kind() {
        assert!(object_kinds().any(|t| t.name == "process"));
    }

    #[test]
    fn every_process_op_is_impure_and_is_a_builtin() {
        for op in PROCESS_OPS {
            assert!(op.ends_with('!'), "`{op}` must be impure: concurrency is an effect");
            assert!(
                crate::ir::builtin_id(op).is_some(),
                "`{op}` is in the type spec but not in the builtin table"
            );
        }
    }
}
