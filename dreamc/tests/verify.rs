//! Tests for the IR verifier.
//!
//! A validator nobody has watched fail is not evidence of anything, so each
//! test breaks exactly one invariant and checks that the verifier names it.

use dreamc::ir::*;
use dreamc::verify::verify;

/// Compile something small and well formed to corrupt.
fn program(src: &str) -> Program {
    dreamc::compile_source(src, "test", "test.dr", true)
        .expect("the fixture itself should compile")
        .program
}

fn problems(p: &Program) -> String {
    verify(p).iter().map(|x| x.to_string()).collect::<Vec<_>>().join("\n")
}

fn find_node(p: &Program, op: Op) -> usize {
    p.nodes
        .iter()
        .position(|n| n.opcode() == Some(op))
        .unwrap_or_else(|| panic!("fixture has no {op:?} node"))
}

#[test]
fn a_well_formed_program_passes() {
    let p = program(
        "import std.console;\n\
         let rec fac n = if n <= 1 { 1 } else { n * fac (n - 1) };\n\
         let xs = [1, 2, 3];\n\
         let m = %{ :a => 1 };\n\
         let f a b = fn x -> a + b + x;\n\
         let main! = { console.print! \"x\" (fac 5) }",
    );
    assert_eq!(verify(&p).len(), 0, "{}", problems(&p));
}

#[test]
fn an_out_of_range_child_is_caught() {
    let mut p = program("let f x = x + 1;");
    let i = find_node(&p, Op::Add);
    p.nodes[i].b = 9999;
    assert!(problems(&p).contains("out of range"), "{}", problems(&p));
}

#[test]
fn a_slot_outside_the_frame_is_caught() {
    let mut p = program("let f x = x;");
    let i = find_node(&p, Op::Local);
    p.nodes[i].a = 40;
    let text = problems(&p);
    assert!(text.contains("slot 40"), "{text}");
}

#[test]
fn a_capture_outside_the_closure_is_caught() {
    let mut p = program("let f a = fn x -> a + x;");
    let i = find_node(&p, Op::Capture);
    p.nodes[i].a = 7;
    let text = problems(&p);
    assert!(text.contains("capture 7"), "{text}");
}

#[test]
fn a_dangling_constant_index_is_caught() {
    let mut p = program("let x = 42;");
    let i = find_node(&p, Op::ConstInt);
    p.nodes[i].a = 500;
    assert!(problems(&p).contains("does not exist"), "{}", problems(&p));
}

#[test]
fn a_dangling_global_reference_is_caught() {
    let mut p = program("let a = 1;\nlet b = a;");
    let i = find_node(&p, Op::Global);
    p.nodes[i].a = 900;
    assert!(problems(&p).contains("global 900"), "{}", problems(&p));
}

#[test]
fn a_cycle_in_the_tree_is_caught() {
    // Nothing about a cycle is an out-of-range index, so only the acyclicity
    // check finds it -- and it is the one that would hang the VM.
    let mut p = program("let f x = x + 1;");
    let add = find_node(&p, Op::Add);
    p.nodes[add].a = add as u32;
    assert!(problems(&p).contains("cycle"), "{}", problems(&p));
}

#[test]
fn a_self_referential_module_is_caught() {
    let mut p = program("let x = 1;");
    p.modules[0].derives = 0;
    assert!(problems(&p).contains("cannot derive itself"), "{}", problems(&p));
}

#[test]
fn a_gap_in_module_global_coverage_is_caught() {
    let mut p = program("let x = 1;\nlet y = 2;");
    p.modules[0].globals_count -= 1;
    let text = problems(&p);
    assert!(text.contains("account for"), "{text}");
}

#[test]
fn an_entry_point_that_takes_arguments_is_caught() {
    let mut p = program("let f x = x;\nlet main! = { 1 }");
    // Point the entry at `f`, which needs an argument nothing can supply.
    let f = p
        .globals
        .iter()
        .find(|g| p.k.strings[g.name as usize] == "f")
        .unwrap()
        .target;
    p.entry = f;
    assert!(problems(&p).contains("takes parameters"), "{}", problems(&p));
}

#[test]
fn an_unknown_opcode_is_caught() {
    let mut p = program("let x = 1;");
    p.nodes[0].op = 200;
    assert!(problems(&p).contains("unknown opcode"), "{}", problems(&p));
}

#[test]
fn a_short_child_pool_is_caught() {
    let mut p = program("let xs = [1, 2, 3];");
    let i = find_node(&p, Op::MakeList);
    p.nodes[i].b = 100;
    assert!(problems(&p).contains("child pool"), "{}", problems(&p));
}

#[test]
fn mismatched_debug_info_is_caught() {
    // Needs more than one node: dropping the only span leaves an empty list,
    // which legitimately means "this image carries no debug info".
    let mut p = program("let x = 1 + 2 + 3;");
    p.spans.pop();
    assert!(problems(&p).contains("parallel to the arena"), "{}", problems(&p));
}

#[test]
fn the_verifier_reports_where_the_problem_is() {
    let mut p = program("let f x = x;");
    let i = find_node(&p, Op::Local);
    p.nodes[i].a = 40;
    let text = problems(&p);
    // The report has to name the function, or it is not actionable.
    assert!(text.contains("fn#") && text.contains('`'), "{text}");
}
