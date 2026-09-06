//! The Dream compiler, as a library.
//!
//! Dream compiles whole programs: a root file plus every module it imports.
//! The binary is a thin CLI over [`compile_program`]; keeping the pipeline here
//! means the VM can link the compiler directly (for a REPL, or for the `mind`
//! build system) instead of shelling out.

pub mod ast;
pub mod config;
pub mod consteval;
pub mod diag;
pub mod disasm;
pub mod emit;
pub mod ir;
pub mod lexer;
pub mod lower;
pub mod modules;
pub mod package;
pub mod parser;
pub mod types;
pub mod verify;
pub mod vm;

use std::path::{Path, PathBuf};

use diag::{Diag, Level, SourceFile};

#[derive(Debug)]
pub struct Compiled {
    pub image: Vec<u8>,
    pub program: ir::Program,
    /// Non-fatal diagnostics produced by a successful compile.
    pub warnings: Vec<Diag>,
    /// Every file that went into the program, for rendering diagnostics.
    pub files: Vec<SourceFile>,
}

#[derive(Debug)]
pub struct CompileError {
    pub diags: Vec<Diag>,
    pub files: Vec<SourceFile>,
}

impl CompileError {
    pub fn render(&self) -> String {
        let errors = self.diags.iter().filter(|d| d.level == Level::Error).count();
        format!(
            "{}\n{} error{} emitted\n",
            diag::render_program(&self.diags, &self.files),
            errors,
            if errors == 1 { "" } else { "s" }
        )
    }
}

/// How to compile a program.
#[derive(Debug, Clone)]
pub struct Options {
    /// Directories searched for module files directly, for projects that have
    /// no manifest.
    pub include: Vec<PathBuf>,
    /// Directories that are packages, or that hold packages.
    pub package_roots: Vec<PathBuf>,
    pub debug_info: bool,
    /// What `when` conditions are tested against.
    pub config: config::Config,
    /// Build a test runner instead of using the program's own `main!`.
    pub test_mode: bool,
    /// Module paths the host registers at run time, beyond the built-in
    /// `std.*` ones. An embedder names its own module here so that a program
    /// importing it compiles.
    pub host_modules: Vec<String>,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            include: Vec::new(),
            package_roots: Vec::new(),
            debug_info: true,
            config: config::Config::new(),
            test_mode: false,
            host_modules: Vec::new(),
        }
    }
}

impl Options {
    pub fn with_debug(debug_info: bool) -> Options {
        Options { debug_info, ..Options::default() }
    }
}

/// Compile a root file and everything it imports into one image.
pub fn compile_program(root: &Path, opts: &Options) -> Result<Compiled, CompileError> {
    let debug_info = opts.debug_info;
    // A package root is also a reasonable place to look for a loose module, so
    // both lists feed the plain file search.
    let mut search_extra = opts.include.clone();
    search_extra.extend(opts.package_roots.iter().cloned());
    let search = modules::Loader::default_search_paths(root, &search_extra);
    let packages = package::PackageSet::discover(root, &opts.package_roots);
    let package_notes: Vec<String> = packages.notes.clone();
    let (set, mut diags) = modules::Loader::new(search)
        .with_packages(packages)
        .with_config(opts.config.clone())
        .with_host_modules(opts.host_modules.clone())
        .with_test_runner(opts.test_mode)
        .load_root(root);
    for note in package_notes {
        diags.push(Diag::warning(lexer::Span::default(), note));
    }
    if diags.iter().any(|d| d.level == Level::Error) {
        return Err(CompileError { files: set.files, diags });
    }
    // Warnings from loading -- a package that could not be read, a name clash --
    // have to survive into the result, or nobody ever sees them.
    lower_set(set, debug_info, diags)
}

/// Compile a single source string, with imports assumed to be host provided.
/// Used by tests and by anything that has source but no file system.
pub fn compile_source(
    src: &str,
    module_name: &str,
    source_name: &str,
    debug_info: bool,
) -> Result<Compiled, Vec<Diag>> {
    let (set, diags) = modules::Loader::new(Vec::new()).load_source(module_name, source_name, src);
    if diags.iter().any(|d| d.level == Level::Error) {
        return Err(diags);
    }
    lower_set(set, debug_info, diags).map_err(|e| e.diags)
}

fn lower_set(
    set: modules::ModuleSet,
    debug_info: bool,
    carried: Vec<Diag>,
) -> Result<Compiled, CompileError> {
    let files = set.files.clone();
    let root = set.modules.len().saturating_sub(1);
    let (name, source_path) = match set.modules.get(root) {
        Some(m) => (m.name.clone(), files[m.source].path.clone()),
        None => ("main".to_string(), String::new()),
    };

    let mut lo = lower::Lowerer::new(&name, &source_path);
    lo.diags = carried;
    lo.lower_program(&set);
    if lo.diags.iter().any(|d| d.level == Level::Error) {
        return Err(CompileError { diags: lo.diags, files });
    }

    resolve_impure_comp(&mut lo);
    if lo.diags.iter().any(|d| d.level == Level::Error) {
        return Err(CompileError { diags: lo.diags, files });
    }

    // Check the program before writing it out. A malformed image would be
    // rejected by the VM with a message that blames the file rather than the
    // compiler, and only after it had already been produced.
    let problems = verify::verify(&lo.prog);
    if !problems.is_empty() {
        let mut diags = lo.diags;
        for p in problems {
            diags.push(Diag::error(
                lexer::Span::default(),
                format!("internal compiler error: {p}"),
            ));
        }
        return Err(CompileError { diags, files });
    }

    let image = emit::emit(&lo.prog, debug_info);
    Ok(Compiled { image, program: lo.prog, warnings: lo.diags, files })
}

/// Run each `comp!` on the VM and bake its answer into the program.
///
/// This has to happen after everything is lowered: staging an expression means
/// handing the VM a complete image with that expression as its entry point.
/// They are evaluated in the order they were written, so an earlier `comp!` is
/// already a constant by the time a later one might read it.
fn resolve_impure_comp(lo: &mut lower::Lowerer<'_>) {
    let pending = std::mem::take(&mut lo.pending_comp);
    if pending.is_empty() {
        return;
    }

    let vm = match vm::Vm::open_default() {
        Ok(vm) => vm,
        Err(e) => {
            for pc in &pending {
                lo.diags.push(
                    Diag::error(pc.span, "`comp!` needs the Dream VM, which could not be loaded")
                        .with_note(format!(
                            "set DREAM_VM_LIB to libdream.so, or build the VM. Last attempt: {e}"
                        ))
                        .in_source(pc.source),
                );
            }
            return;
        }
    };

    let saved_entry = lo.prog.entry;
    for pc in pending {
        lo.prog.entry = pc.func as u32;
        let staging = emit::emit(&lo.prog, false);
        match vm.eval(&staging, "") {
            Ok(value) => lo.patch_comp(pc.node, &value, pc.span),
            Err(e) => lo.diags.push(
                Diag::error(pc.span, format!("`comp!` failed while evaluating: {e}"))
                    .in_source(pc.source),
            ),
        }
    }
    lo.prog.entry = saved_entry;
}
