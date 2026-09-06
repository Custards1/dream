//! End-to-end tests: source in, bytecode image out.

use dreamc::disasm::Image;
use dreamc::emit::*;
use dreamc::ir::*;
use dreamc::{Compiled, compile_source};

fn ok(src: &str) -> Compiled {
    match compile_source(src, "test", "test.dr", true) {
        Ok(c) => c,
        Err(diags) => panic!(
            "expected success, got: {}",
            diags.iter().map(|d| d.message.clone()).collect::<Vec<_>>().join("; ")
        ),
    }
}

fn errors(src: &str) -> Vec<String> {
    match compile_source(src, "test", "test.dr", true) {
        Ok(_) => panic!("expected failure, but compilation succeeded"),
        Err(diags) => diags.into_iter().map(|d| d.message).collect(),
    }
}

fn func<'a>(p: &'a Program, name: &str) -> &'a Func {
    p.funcs
        .iter()
        .find(|f| p.k.strings[f.name as usize] == name)
        .unwrap_or_else(|| panic!("no function named `{name}`"))
}

fn node(p: &Program, idx: u32) -> Node {
    p.nodes[idx as usize]
}

fn kids(p: &Program, off: u32, count: u32) -> Vec<u32> {
    (0..count).map(|i| p.kids[(off + i) as usize]).collect()
}

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------

#[test]
fn header_carries_magic_version_and_entry() {
    let c = ok("let main! = { 1 }");
    assert_eq!(&c.image[..8], b"DAGNCAAF");
    assert_eq!(u16::from_le_bytes([c.image[8], c.image[9]]), VERSION_MAJOR);

    let img = Image::load(&c.image).expect("image should load");
    assert_eq!(img.string(img.module_name), "test");
    assert_ne!(img.entry, NO_NODE, "`main!` should be recorded as the entry point");
    assert_eq!(img.string(img.func(img.entry).unwrap().name), "main!");
}

#[test]
fn image_round_trips_through_the_reader() {
    let c = ok("let rec fac n = if n <= 1 { 1 } else { n * fac (n - 1) };\nlet main! = { fac 5 }");
    let img = Image::load(&c.image).unwrap();
    assert_eq!(img.func_count() as usize, c.program.funcs.len());
    assert_eq!(img.global_count() as usize, c.program.globals.len());
    for (i, want) in c.program.nodes.iter().enumerate() {
        assert_eq!(&img.node(i as u32), want, "node {i} differs after a round trip");
    }
    for (i, want) in c.program.k.strings.iter().enumerate() {
        assert_eq!(img.string(i as u32), want);
    }
}

#[test]
fn output_is_deterministic() {
    let src = "import std.console;\nlet main! = { console.print! \"hi\" }";
    assert_eq!(ok(src).image, ok(src).image);
}

#[test]
fn debug_info_is_optional() {
    let src = "let main! = { 1 }";
    let with = compile_source(src, "t", "t.dr", true).unwrap().image;
    let without = compile_source(src, "t", "t.dr", false).unwrap().image;

    assert!(Image::load(&with).unwrap().section(T_SPAN).is_some());
    assert!(Image::load(&without).unwrap().section(T_SPAN).is_none());
    assert!(without.len() < with.len());
}

#[test]
fn every_section_is_eight_byte_aligned() {
    let c = ok("let main! = { [1, 2, 3] }");
    let img = Image::load(&c.image).unwrap();
    for kind in &img.order {
        let (off, _, _) = img.section_meta(*kind).unwrap();
        assert_eq!(off % 8, 0, "section {} is misaligned", tag_name(*kind));
    }
}

#[test]
fn a_truncated_image_is_rejected_rather_than_read() {
    let c = ok("let main! = { 1 }");
    assert!(Image::load(&c.image[..16]).is_err());
    let mut corrupt = c.image.clone();
    corrupt[0] = b'X';
    assert!(Image::load(&corrupt).is_err());
}

// ---------------------------------------------------------------------------
// Grammar
// ---------------------------------------------------------------------------

#[test]
fn a_newline_ends_a_statement() {
    // Without newline sensitivity this would parse as one call, `g! 1 g! 2`.
    let c = ok("let g! x = x;\nlet main! = {\n    g! 1\n    g! 2\n}");
    let body = node(&c.program, func(&c.program, "main!").body);
    assert_eq!(body.opcode(), Some(Op::Block));
    assert_eq!(body.b, 2, "the block should hold two statements");
}

#[test]
fn a_bracket_group_suspends_newline_sensitivity() {
    let c = ok("let f a b = a;\nlet main! = {\n    f [1,\n       2] 3\n}");
    let body = node(&c.program, func(&c.program, "main!").body);
    assert_eq!(body.b, 1, "the bracketed argument spans lines without ending the statement");
    let call = node(&c.program, c.program.kids[body.a as usize]);
    assert_eq!(call.opcode(), Some(Op::Apply));
    assert_eq!(call.c, 2, "both arguments belong to the same call");
}

#[test]
fn pipe_feeds_the_last_argument() {
    // `1 |> f 2`  ==>  `f 2 1`
    let c = ok("let f a b = a;\nlet m = 1 |> f 2;");
    let body = node(&c.program, func(&c.program, "m").body);
    assert_eq!(body.opcode(), Some(Op::Apply));
    assert_eq!(body.c, 2);
    let args = kids(&c.program, body.b, body.c);
    let value_of = |n: u32| c.program.k.ints[node(&c.program, n).a as usize];
    assert_eq!(value_of(args[0]), 2);
    assert_eq!(value_of(args[1]), 1, "the piped value lands last");
}

#[test]
fn application_binds_tighter_than_arithmetic() {
    // `f n - 1` is `(f n) - 1`, as in every other ML-family language.
    let c = ok("let f x = x;\nlet m n = f n - 1;");
    let body = node(&c.program, func(&c.program, "m").body);
    assert_eq!(body.opcode(), Some(Op::Sub));
    assert_eq!(node(&c.program, body.a).opcode(), Some(Op::Apply));
}

#[test]
fn an_if_condition_does_not_swallow_the_branch_brace() {
    let c = ok("let m n = if n <= 1 { 1 } else { 2 };");
    let body = node(&c.program, func(&c.program, "m").body);
    assert_eq!(body.opcode(), Some(Op::If));
    assert_eq!(node(&c.program, body.a).opcode(), Some(Op::Le));
}

#[test]
fn else_if_chains_nest() {
    let c = ok("let m n = if n < 0 { :neg } else if n == 0 { :zero } else { :pos };");
    let body = node(&c.program, func(&c.program, "m").body);
    assert_eq!(node(&c.program, body.c).opcode(), Some(Op::If));
}

#[test]
fn operator_precedence_follows_the_usual_ladder() {
    // `1 + 2 * 3 == 7 && true`  ==>  `((1 + (2 * 3)) == 7) && true`
    let c = ok("let m = 1 + 2 * 3 == 7 && true;");
    let and = node(&c.program, func(&c.program, "m").body);
    assert_eq!(and.opcode(), Some(Op::And));
    let eq = node(&c.program, and.a);
    assert_eq!(eq.opcode(), Some(Op::Eq));
    let add = node(&c.program, eq.a);
    assert_eq!(add.opcode(), Some(Op::Add));
    assert_eq!(node(&c.program, add.b).opcode(), Some(Op::Mul));
}

#[test]
fn every_literal_form_lowers() {
    let c = ok(
        "let a = 1;\nlet b = 2.5;\nlet ch = 'q';\nlet t = true;\nlet s = \"hi\";\n\
         let at = :sym;\nlet l = [1];\nlet ar = #[1];\nlet mp = %{ :k => 1 };\nlet u = ();",
    );
    let p = &c.program;
    let op = |name: &str| node(p, func(p, name).body).opcode().unwrap();
    assert_eq!(op("a"), Op::ConstInt);
    assert_eq!(op("b"), Op::ConstFloat);
    assert_eq!(op("ch"), Op::ConstChar);
    assert_eq!(op("t"), Op::ConstBool);
    assert_eq!(op("s"), Op::ConstStr);
    assert_eq!(op("at"), Op::ConstAtom);
    assert_eq!(op("l"), Op::MakeList);
    assert_eq!(op("ar"), Op::MakeArray);
    assert_eq!(op("mp"), Op::MakeMap);
    assert_eq!(op("u"), Op::Unit);
}

#[test]
fn constants_are_interned() {
    let c = ok("let a = \"same\";\nlet b = \"same\";\nlet x = 42;\nlet y = 42;");
    assert_eq!(c.program.k.strings.iter().filter(|s| *s == "same").count(), 1);
    assert_eq!(c.program.k.ints.iter().filter(|i| **i == 42).count(), 1);
}

// ---------------------------------------------------------------------------
// Resolution and closures
// ---------------------------------------------------------------------------

#[test]
fn a_thunk_captures_the_enclosing_local() {
    let c = ok("let g! x = x;\nlet main! = {\n    let n = 1;\n    spawn! $(g! n)\n}");
    let p = &c.program;
    let thunk = func(p, "<thunk>");
    assert_eq!(thunk.arity, 0);
    assert_eq!(thunk.n_captures, 1);
    let (from_capture, idx) = decode_capture(p.kids[thunk.captures_off as usize]);
    assert!(!from_capture, "`n` is a local of the defining frame");
    assert_eq!(idx, 0);
}

#[test]
fn captures_thread_through_intermediate_closures() {
    let c = ok("let deep a = fn b -> fn c -> a + b + c;");
    let p = &c.program;
    let inner = p.funcs.last().unwrap();
    assert_eq!(inner.n_captures, 2, "the innermost lambda needs both `a` and `b`");
    let (a_from_capture, _) = decode_capture(p.kids[inner.captures_off as usize]);
    let (b_from_capture, _) = decode_capture(p.kids[inner.captures_off as usize + 1]);
    assert!(a_from_capture, "`a` arrives via the middle lambda's capture list");
    assert!(!b_from_capture, "`b` is a local of the middle lambda");
}

#[test]
fn a_local_let_rec_can_see_itself() {
    let c = ok("let m = {\n    let rec down k = if k <= 0 { 0 } else { down (k - 1) }\n    down 3\n};");
    let p = &c.program;
    let down = func(p, "down");
    assert_eq!(down.n_captures, 1, "`down` captures its own binding cell");
}

#[test]
fn top_level_bindings_may_be_used_before_they_are_defined() {
    // Mutual recursion needs both names visible during the other's lowering.
    let c = ok("let rec even n = if n == 0 { true } else { odd (n - 1) }\nlet rec odd n = if n == 0 { false } else { even (n - 1) }");
    assert_eq!(c.program.funcs.len(), 2);
}

#[test]
fn a_parameterless_pure_binding_is_a_memoized_thunk() {
    let c = ok("let value = 1 + 1;\nlet act! = { 1 }");
    let p = &c.program;
    let value = func(p, "value");
    assert_eq!(value.arity, 0);
    assert!(value.flags & FN_GLOBAL_VALUE != 0, "a pure value is forced once");

    let act = func(p, "act!");
    assert!(act.flags & FN_IMPURE != 0);
    assert!(
        act.flags & FN_GLOBAL_VALUE == 0,
        "an impure action must re-run on every call, so it is never memoized"
    );
}

#[test]
fn builtins_resolve_without_an_import() {
    let c = ok("let main! = { spawn! $(1) }");
    let p = &c.program;
    let body = node(p, func(p, "main!").body);
    let call = node(p, p.kids[body.a as usize]);
    assert_eq!(node(p, call.a).opcode(), Some(Op::Builtin));
    assert_eq!(node(p, call.a).a, builtin_id("spawn!").unwrap());
}

#[test]
fn a_local_binding_shadows_a_builtin() {
    let c = ok("let m len = len + 1;");
    let p = &c.program;
    let add = node(p, func(p, "m").body);
    assert_eq!(node(p, add.a).opcode(), Some(Op::Local), "the parameter wins over `len`");
}

// ---------------------------------------------------------------------------
// Purity
// ---------------------------------------------------------------------------

#[test]
fn a_pure_function_may_not_call_an_impure_one() {
    let msgs = errors("import std.console;\nlet greet name = console.print! name;");
    assert!(msgs.iter().any(|m| m.contains("pure function `greet`")), "{msgs:?}");
}

#[test]
fn a_pure_function_may_not_use_try() {
    let msgs = errors("let risky n = try! { 1 / n } catch e { 0 };");
    assert!(msgs.iter().any(|m| m.contains("`try!`")), "{msgs:?}");
}

#[test]
fn an_impure_function_may_perform_effects() {
    let c = ok("import std.console;\nlet greet! name = console.print! name;");
    assert!(func(&c.program, "greet!").flags & FN_IMPURE != 0);
}

#[test]
fn effects_propagate_out_of_nested_expressions() {
    let c = ok("import std.console;\nlet greet! n = 1 + (console.print! n);");
    let p = &c.program;
    let body = node(p, func(p, "greet!").body);
    assert!(body.flags & F_IMPURE != 0, "the `+` node inherits the effect below it");
}

#[test]
fn suspending_an_effect_is_pure_but_running_it_is_not() {
    // Building a suspended computation does not perform it, so a pure binding
    // may hold one. That is what lets a pure list carry effectful work -- a
    // list of test cases, a queue of jobs.
    ok("let g! x = x;\nlet actions = [$(g! 1), $(g! 2)];");
    ok("let g! x = x;\nlet main! = { spawn! $(g! 1) }");

    // Running it is still an effect, and still needs an impure context.
    let msgs = errors("let g! x = x;\nlet started = spawn! $(g! 1);");
    assert!(msgs.iter().any(|m| m.contains("pure function")), "{msgs:?}");
}

// ---------------------------------------------------------------------------
// Evaluation-order flags
// ---------------------------------------------------------------------------

#[test]
fn effectful_statements_are_sequenced_but_bindings_stay_lazy() {
    let c = ok("let g! x = x;\nlet main! = {\n    let n = 1 + 1;\n    g! n\n    g! 2\n}");
    let p = &c.program;
    let body = node(p, func(p, "main!").body);
    let stmts = kids(p, body.a, body.b);

    let bind = node(p, stmts[0]);
    assert_eq!(bind.opcode(), Some(Op::Bind));
    assert_eq!(bind.flags & F_STRICT, 0, "a pure binding is not forced at its `let`");

    let first_call = node(p, stmts[1]);
    assert!(first_call.flags & F_STRICT != 0, "a discarded effect must still happen");
    assert!(first_call.flags & F_IMPURE != 0);
}

#[test]
fn binding_an_effect_sequences_it_at_the_let() {
    let c = ok("let g! x = x;\nlet main! = {\n    let n = g! 1;\n    2\n}");
    let p = &c.program;
    let body = node(p, func(p, "main!").body);
    let bind = node(p, p.kids[body.a as usize]);
    assert!(
        bind.flags & F_STRICT != 0,
        "an effectful value runs where it is written, not at first use"
    );
}

#[test]
fn a_discarded_pure_statement_warns_instead_of_being_forced() {
    let c = ok("let m n = {\n    n + 1\n    n * 2\n};");
    assert_eq!(c.warnings.len(), 1);
    assert!(c.warnings[0].message.contains("discarded"));

    let p = &c.program;
    let body = node(p, func(p, "m").body);
    let first = node(p, p.kids[body.a as usize]);
    assert_eq!(first.flags & F_STRICT, 0, "forcing it could raise an error nobody asked for");
}

#[test]
fn tail_calls_are_marked() {
    let c = ok("let rec loop n = if n <= 0 { 0 } else { loop (n - 1) };");
    let p = &c.program;
    let iff = node(p, func(p, "loop").body);
    let else_block = node(p, iff.c);
    let call = node(p, p.kids[else_block.a as usize]);
    assert_eq!(call.opcode(), Some(Op::Apply));
    assert!(call.flags & F_TAIL != 0);
}

#[test]
fn a_non_tail_call_is_not_marked() {
    let c = ok("let rec sum n = if n <= 0 { 0 } else { n + sum (n - 1) };");
    let p = &c.program;
    let iff = node(p, func(p, "sum").body);
    let else_block = node(p, iff.c);
    let add = node(p, p.kids[else_block.a as usize]);
    assert_eq!(add.opcode(), Some(Op::Add));
    assert_eq!(node(p, add.b).flags & F_TAIL, 0, "the call feeds an addition, so it is not a tail call");
}

#[test]
fn an_if_condition_is_always_forced() {
    let c = ok("let m n = if n { 1 } else { 2 };");
    let p = &c.program;
    let iff = node(p, func(p, "m").body);
    assert!(node(p, iff.a).flags & F_STRICT != 0);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

#[test]
fn an_unknown_name_is_reported() {
    let msgs = errors("let f x = x + nope;");
    assert!(msgs.iter().any(|m| m.contains("cannot find `nope`")), "{msgs:?}");
}

#[test]
fn a_duplicate_binding_is_reported() {
    let msgs = errors("let f x = x;\nlet f y = y;");
    assert!(msgs.iter().any(|m| m.contains("already bound")), "{msgs:?}");
}

#[test]
fn an_unclosed_block_points_at_its_opening_brace() {
    let src = "let f x = { x";
    let diags = compile_source(src, "t", "t.dr", true).unwrap_err();
    assert!(diags[0].message.contains("never closed"), "{:?}", diags[0].message);
    assert_eq!(diags[0].span.start as usize, src.find('{').unwrap() as usize);
}

#[test]
fn a_try_without_a_catch_is_reported() {
    let msgs = errors("let f! x = try! { x };");
    assert!(msgs.iter().any(|m| m.contains("catch")), "{msgs:?}");
}

#[test]
fn parsing_recovers_far_enough_to_report_more_than_one_error() {
    let msgs = errors("let a = ;\nlet b = ;");
    assert!(msgs.len() >= 2, "expected several errors, got {msgs:?}");
}

#[test]
fn diagnostics_render_with_a_caret_under_the_span() {
    let src = "let f x = x + nope;";
    let diags = compile_source(src, "t", "t.dr", true).unwrap_err();
    let text = diags[0].render(src, "t.dr");
    assert!(text.contains("t.dr:1:15"), "{text}");
    assert!(text.contains("^^^^"), "{text}");
}

// ---------------------------------------------------------------------------
// Modules, virtuals and compile-time evaluation
// ---------------------------------------------------------------------------

use std::path::{Path, PathBuf};

/// Write a small multi-file project into a scratch directory and compile it.
struct Project {
    dir: PathBuf,
}

impl Project {
    fn new(name: &str) -> Project {
        let dir = std::env::temp_dir().join(format!("dawnc-test-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        Project { dir }
    }
    fn file(&self, rel: &str, text: &str) -> &Project {
        let path = self.dir.join(rel);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, text).unwrap();
        self
    }
    fn compile(&self, root: &str) -> Result<dreamc::Compiled, dreamc::CompileError> {
        dreamc::compile_program(&self.dir.join(root), &dreamc::Options::default())
    }
    fn errors(&self, root: &str) -> String {
        match self.compile(root) {
            Ok(_) => panic!("expected compilation to fail"),
            Err(e) => e.render(),
        }
    }
}

impl Drop for Project {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn global_of<'a>(p: &'a Program, module: &str, name: &str) -> Option<&'a Global> {
    let m = p
        .modules
        .iter()
        .find(|m| p.k.strings[m.name as usize] == module)?;
    (m.globals_start..m.globals_start + m.globals_count)
        .map(|i| &p.globals[i as usize])
        .find(|g| p.k.strings[g.name as usize] == name)
}

#[test]
fn an_import_pulls_in_another_file() {
    let p = Project::new("import");
    p.file("util.dr", "let double x = x * 2;")
        .file("main.dr", "import util;\nlet main! = { util.double 21 }");
    let c = p.compile("main.dr").unwrap();

    assert_eq!(c.program.modules.len(), 2);
    // Dependencies come first, so the root module is last.
    assert_eq!(
        c.program.k.strings[c.program.modules[1].name as usize],
        "main"
    );
    assert_eq!(c.program.k.strings[c.program.modules[0].name as usize], "util");
    assert!(global_of(&c.program, "util", "double").is_some());
}

#[test]
fn a_call_into_another_module_resolves_to_a_global() {
    // Whole-program compilation means no name lookup survives to run time.
    let p = Project::new("static");
    p.file("util.dr", "let double x = x * 2;")
        .file("main.dr", "import util;\nlet main! = { util.double 21 }");
    let c = p.compile("main.dr").unwrap();

    let target = global_of(&c.program, "util", "double").unwrap().target;
    let uses_global = c.program.nodes.iter().any(|n| {
        n.opcode() == Some(Op::Global)
            && c.program.globals[n.a as usize].target == target
    });
    assert!(uses_global, "the cross-module call should be a direct global reference");
    // And nothing should be looking a member up by name.
    assert!(!c.program.nodes.iter().any(|n| n.opcode() == Some(Op::Field)));
}

#[test]
fn a_missing_module_is_reported_with_where_it_looked() {
    let p = Project::new("missing");
    p.file("main.dr", "import nope.gone;\nlet main! = { 1 }");
    let text = p.errors("main.dr");
    assert!(text.contains("cannot find module `nope.gone`"), "{text}");
    // The message has to say why, not just that it failed.
    assert!(text.contains("no package is named `nope`"), "{text}");
    assert!(text.contains("Host-provided modules"), "{text}");
}

#[test]
fn an_import_cycle_is_reported() {
    let p = Project::new("cycle");
    p.file("a.dr", "import b;\nlet x = 1;")
        .file("b.dr", "import a;\nlet y = 2;")
        .file("main.dr", "import a;\nlet main! = { 1 }");
    let text = p.errors("main.dr");
    assert!(text.contains("import cycle"), "{text}");
}

#[test]
fn deriving_binds_a_virtual_to_the_implementation() {
    let p = Project::new("derive");
    p.file("shape.dr", "virtual let area s;\nlet describe s = area s * 10;")
        .file("rect.dr", "derive shape;\nlet area s = 4;")
        .file("main.dr", "import rect;\nlet main! = { rect.describe 0 }");
    let c = p.compile("main.dr").unwrap();

    // The deriving module gets its own copy of the base's code.
    assert!(global_of(&c.program, "rect", "describe").is_some());
    assert!(global_of(&c.program, "rect", "area").is_some());
    assert!(global_of(&c.program, "shape", "describe").is_some());
    // They are distinct functions: specializing is what binds the virtual.
    assert_ne!(
        global_of(&c.program, "rect", "describe").unwrap().target,
        global_of(&c.program, "shape", "describe").unwrap().target
    );
}

#[test]
fn a_virtual_with_a_default_need_not_be_implemented() {
    let p = Project::new("default");
    p.file("base.dr", "virtual let units s = \"cm\";\nlet label s = units s;")
        .file("impl.dr", "derive base;")
        .file("main.dr", "import impl;\nlet main! = { impl.label 0 }");
    assert!(p.compile("main.dr").is_ok());
}

#[test]
fn a_virtual_without_a_default_must_be_implemented() {
    let p = Project::new("unimpl");
    p.file("base.dr", "virtual let area s;\nlet describe s = area s;")
        .file("impl.dr", "derive base;")
        .file("main.dr", "import impl;\nlet main! = { impl.describe 0 }");
    let text = p.errors("main.dr");
    assert!(text.contains("does not implement `area`"), "{text}");
}

#[test]
fn a_derived_module_can_override_a_default() {
    let p = Project::new("override");
    p.file("base.dr", "virtual let units s = \"cm\";\nlet label s = units s;")
        .file("impl.dr", "derive base;\nlet units s = \"m\";")
        .file("main.dr", "import impl;\nlet main! = { impl.label 0 }");
    let c = p.compile("main.dr").unwrap();
    // The override wins, so the module has exactly one `units`.
    let m = c
        .program
        .modules
        .iter()
        .find(|m| c.program.k.strings[m.name as usize] == "impl")
        .unwrap();
    let count = (m.globals_start..m.globals_start + m.globals_count)
        .filter(|i| c.program.k.strings[c.program.globals[*i as usize].name as usize] == "units")
        .count();
    assert_eq!(count, 1);
}

#[test]
fn comp_folds_to_a_constant() {
    let c = ok("let rec sum n = if n <= 0 { 0 } else { n + sum (n - 1) };\n\
                let total = comp (sum 100);");
    let body = node(&c.program, func(&c.program, "total").body);
    assert_eq!(body.opcode(), Some(Op::ConstInt));
    assert_eq!(c.program.k.ints[body.a as usize], 5050);
    // Folded means folded: no call survives in that function.
    assert_eq!(node(&c.program, func(&c.program, "total").body).opcode(), Some(Op::ConstInt));
}

#[test]
fn comp_builds_structures() {
    let c = ok("let xs = comp [1 + 1, 2 * 2];");
    let body = node(&c.program, func(&c.program, "xs").body);
    assert_eq!(body.opcode(), Some(Op::MakeList));
    assert_eq!(body.b, 2);
    let items = kids(&c.program, body.a, body.b);
    assert_eq!(c.program.k.ints[node(&c.program, items[0]).a as usize], 2);
    assert_eq!(c.program.k.ints[node(&c.program, items[1]).a as usize], 4);
}

#[test]
fn comp_rejects_effects() {
    let msgs = errors("import std.console;\nlet x = comp (console.print! \"hi\" 1);");
    assert!(msgs.iter().any(|m| m.contains("comp")), "{msgs:?}");
}

#[test]
fn comp_rejects_a_non_terminating_expression() {
    let msgs = errors("let rec spin n = spin (n + 1);\nlet x = comp (spin 0);");
    // Either guard is a correct answer; what matters is that it stops.
    assert!(
        msgs.iter().any(|m| m.contains("budget") || m.contains("recursed too deeply")),
        "{msgs:?}"
    );
}

#[test]
fn comp_cannot_see_a_runtime_value() {
    // A compile-time value must not depend on a parameter.
    let msgs = errors("let f x = comp (x + 1);");
    assert!(
        msgs.iter().any(|m| m.contains("cannot find `x`")),
        "{msgs:?}"
    );
}

#[test]
fn comp_division_by_zero_is_a_compile_error() {
    let msgs = errors("let x = comp (1 / 0);");
    assert!(msgs.iter().any(|m| m.contains("division by zero")), "{msgs:?}");
}

#[test]
fn a_local_binding_shadows_a_module_alias() {
    let p = Project::new("shadow");
    p.file("util.dr", "let double x = x * 2;")
        .file(
            "main.dr",
            "import util;\nlet f util = util;\nlet main! = { f 1 }",
        );
    // `util` as a parameter must win over the module of the same name.
    assert!(p.compile("main.dr").is_ok());
}

// ---------------------------------------------------------------------------
// Packages
// ---------------------------------------------------------------------------

impl Project {
    fn compile_with(&self, root: &str, opts: &dreamc::Options) -> Result<dreamc::Compiled, dreamc::CompileError> {
        dreamc::compile_program(&self.dir.join(root), opts)
    }
}

fn module_names(c: &dreamc::Compiled) -> Vec<String> {
    c.program
        .modules
        .iter()
        .map(|m| c.program.k.strings[m.name as usize].clone())
        .collect()
}

#[test]
fn a_package_groups_modules_under_its_name() {
    let p = Project::new("pkg-basic");
    p.file("lib/dusk.toml", "[package]\nname = \"mylib\"\nversion = \"1.0\"\n")
        .file("lib/src/math.dr", "let add a b = a + b;")
        .file("app/dusk.toml", "[package]\nname = \"app\"\n\n[dependencies]\nmylib = { path = \"../lib\" }\n")
        .file("app/src/main.dr", "import mylib.math;\nlet main! = { mylib_unused }\nlet mylib_unused = mylib.math.add 1 2;");

    // The import path names the package, not the directory layout.
    let p2 = Project::new("pkg-basic2");
    p2.file("lib/dusk.toml", "[package]\nname = \"mylib\"\n")
        .file("lib/src/math.dr", "let add a b = a + b;")
        .file("app/dusk.toml", "[package]\nname = \"app\"\n\n[dependencies]\nmylib = { path = \"../lib\" }\n")
        .file("app/src/main.dr", "import mylib.math;\nlet main! = { math.add 20 22 }");
    let c = p2.compile("app/src/main.dr").unwrap();
    assert!(module_names(&c).contains(&"mylib.math".to_string()), "{:?}", module_names(&c));
    assert!(module_names(&c).contains(&"app.main".to_string()), "{:?}", module_names(&c));
    let _ = p;
}

#[test]
fn a_package_can_be_anywhere_its_manifest_says_its_name() {
    // The directory is called `weird-dir` but the package is `tidy`.
    let p = Project::new("pkg-anywhere");
    p.file("weird-dir/dusk.toml", "[package]\nname = \"tidy\"\n")
        .file("weird-dir/src/thing.dr", "let value = 7;")
        .file("app/dusk.toml", "[package]\nname = \"app\"\n\n[dependencies]\ntidy = { path = \"../weird-dir\" }\n")
        .file("app/src/main.dr", "import tidy.thing;\nlet main! = { thing.value }");
    assert!(p.compile("app/src/main.dr").is_ok());
}

#[test]
fn modules_inside_a_package_are_reachable_unqualified() {
    let p = Project::new("pkg-sibling");
    p.file("dusk.toml", "[package]\nname = \"solo\"\n")
        .file("src/helper.dr", "let two = 2;")
        .file("src/main.dr", "import helper;\nlet main! = { helper.two }");
    let c = p.compile("src/main.dr").unwrap();
    assert!(module_names(&c).contains(&"solo.helper".to_string()), "{:?}", module_names(&c));
}

#[test]
fn a_module_imported_two_ways_is_one_module() {
    // From inside the package it is `helper`; from outside, `solo.helper`.
    // Compiling it twice would duplicate its globals and break identity.
    let p = Project::new("pkg-identity");
    p.file("dusk.toml", "[package]\nname = \"solo\"\n")
        .file("src/helper.dr", "let two = 2;")
        .file("src/other.dr", "import helper;\nlet four = helper.two * 2;")
        .file(
            "src/main.dr",
            "import solo.helper;\nimport other;\nlet main! = { helper.two + other.four }",
        );
    let c = p.compile("src/main.dr").unwrap();
    let names = module_names(&c);
    assert_eq!(
        names.iter().filter(|n| *n == "solo.helper").count(),
        1,
        "helper should appear once, got {names:?}"
    );
}

#[test]
fn nested_modules_keep_their_path() {
    let p = Project::new("pkg-nested");
    p.file("dusk.toml", "[package]\nname = \"deep\"\n")
        .file("src/a/b/c.dr", "let v = 1;")
        .file("src/main.dr", "import deep.a.b.c;\nlet main! = { c.v }");
    let c = p.compile("src/main.dr").unwrap();
    assert!(module_names(&c).contains(&"deep.a.b.c".to_string()), "{:?}", module_names(&c));
}

#[test]
fn a_module_may_be_a_directory() {
    let p = Project::new("pkg-moddir");
    p.file("dusk.toml", "[package]\nname = \"d\"\n")
        .file("src/thing/mod.dr", "let v = 3;")
        .file("src/main.dr", "import thing;\nlet main! = { thing.v }");
    let c = p.compile("src/main.dr").unwrap();
    assert!(module_names(&c).contains(&"d.thing".to_string()), "{:?}", module_names(&c));
}

#[test]
fn a_flat_package_needs_no_src_directory() {
    let p = Project::new("pkg-flat");
    p.file("dusk.toml", "[package]\nname = \"flat\"\n")
        .file("helper.dr", "let v = 5;")
        .file("main.dr", "import helper;\nlet main! = { helper.v }");
    assert!(p.compile("main.dr").is_ok());
}

#[test]
fn a_missing_module_lists_what_the_package_does_provide() {
    let p = Project::new("pkg-missing");
    p.file("dusk.toml", "[package]\nname = \"lib\"\n")
        .file("src/present.dr", "let v = 1;")
        .file("src/main.dr", "import lib.absent;\nlet main! = { 1 }");
    let text = p.errors("src/main.dr");
    assert!(text.contains("has no module `absent`"), "{text}");
    assert!(text.contains("present"), "{text}");
}

#[test]
fn a_package_name_may_not_contain_a_dot() {
    // A dot separates the package from the module, so a dotted package name
    // would make imports ambiguous.
    let p = Project::new("pkg-dotted");
    p.file("dep/dusk.toml", "[package]\nname = \"a.b\"\n")
        .file("dusk.toml", "[package]\nname = \"app\"\n\n[dependencies]\nx = { path = \"dep\" }\n")
        .file("main.dr", "let main! = { 1 }");
    let c = p.compile("main.dr").unwrap();
    assert!(
        c.warnings.iter().any(|w| w.message.contains("cannot contain a dot")),
        "{:?}",
        c.warnings.iter().map(|w| &w.message).collect::<Vec<_>>()
    );
}

#[test]
fn a_dependency_with_no_manifest_is_reported() {
    let p = Project::new("pkg-nomanifest");
    p.file("dusk.toml", "[package]\nname = \"app\"\n\n[dependencies]\nghost = { path = \"nowhere\" }\n")
        .file("main.dr", "let main! = { 1 }");
    let c = p.compile("main.dr").unwrap();
    assert!(
        c.warnings.iter().any(|w| w.message.contains("no manifest")),
        "{:?}",
        c.warnings.iter().map(|w| &w.message).collect::<Vec<_>>()
    );
}

#[test]
fn a_package_root_may_hold_several_packages() {
    let p = Project::new("pkg-root");
    p.file("vendor/one/dusk.toml", "[package]\nname = \"one\"\n")
        .file("vendor/one/src/a.dr", "let v = 1;")
        .file("vendor/two/dusk.toml", "[package]\nname = \"two\"\n")
        .file("vendor/two/src/b.dr", "let v = 2;")
        .file("main.dr", "import one.a;\nimport two.b;\nlet main! = { a.v + b.v }");
    let opts = dreamc::Options {
        package_roots: vec![p.dir.join("vendor")],
        ..dreamc::Options::default()
    };
    assert!(p.compile_with("main.dr", &opts).is_ok());
}

#[test]
fn a_project_without_a_manifest_still_compiles() {
    // Packages are a convenience, not a requirement.
    let p = Project::new("pkg-none");
    p.file("util.dr", "let v = 1;")
        .file("main.dr", "import util;\nlet main! = { util.v }");
    assert!(p.compile("main.dr").is_ok());
}
