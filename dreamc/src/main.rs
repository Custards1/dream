//! `dreamc` — the Dream bytecode compiler.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use dreamc::diag::render_program;
use dreamc::{compile_program, disasm, lexer, parser};

struct Options {
    input: PathBuf,
    output: Option<PathBuf>,
    include: Vec<PathBuf>,
    package_roots: Vec<PathBuf>,
    config: dreamc::config::Config,
    test_mode: bool,
    host_modules: Vec<String>,
    print_cfg: bool,
    dump: bool,
    show_ast: bool,
    show_modules: bool,
    show_packages: bool,
    emit_file: bool,
    debug_info: bool,
}

const USAGE: &str = "\
dreamc — the Dream bytecode compiler

usage: dreamc <input.dr> [options]

options:
  -o, --output <path>   write the image here (default: <input>.dream)
  -I, --include <dir>   also search this directory for module files
  -L, --package-path <dir>  a package, or a directory holding packages
      --packages        list the packages that were found
      --host-module <path>  a module the host registers at run time, so that
                        importing it compiles (repeatable)
      --test            compile a test runner from each module's `tests`
  -D, --define <name[=value]>  define a flag or setting for `when`
      --release         define `release`
      --debug-cfg       define `debug`
      --print-cfg       list the flags and settings `when` can see
      --dump            disassemble the image to stdout after compiling
      --modules         list the modules that went into the program
      --ast             print the parse tree of the root module
      --no-emit         check only; do not write a file
      --no-debug        omit the SPAN debug section
  -h, --help            show this message

A program is compiled whole: the root file plus every module it imports.

An import names a package and a module inside it: `import std.list` is the
module `list` in whichever package calls itself `std`. A package is any
directory holding a `mind.toml`. Packages are found by walking up from the root
file, by following that manifest's dependencies, and in any -L directory or
$DREAM_PACKAGES entry. Within a package, its own modules are also reachable
unqualified.
";

fn parse_args() -> Result<Options, String> {
    let mut args = std::env::args().skip(1);
    let mut input = None;
    let mut opts = Options {
        input: PathBuf::new(),
        output: None,
        include: Vec::new(),
        package_roots: Vec::new(),
        config: dreamc::config::Config::new(),
        test_mode: false,
        host_modules: Vec::new(),
        print_cfg: false,
        dump: false,
        show_ast: false,
        show_modules: false,
        show_packages: false,
        emit_file: true,
        debug_info: true,
    };
    while let Some(a) = args.next() {
        match a.as_str() {
            "-h" | "--help" => {
                print!("{USAGE}");
                std::process::exit(0);
            }
            "-o" | "--output" => {
                opts.output = Some(PathBuf::from(args.next().ok_or("`-o` needs a path")?));
            }
            "-I" | "--include" => {
                opts.include.push(PathBuf::from(args.next().ok_or("`-I` needs a directory")?));
            }
            "-L" | "--package-path" => {
                opts.package_roots
                    .push(PathBuf::from(args.next().ok_or("`-L` needs a directory")?));
            }
            "--packages" => opts.show_packages = true,
            "--host-module" => {
                opts.host_modules
                    .push(args.next().ok_or("`--host-module` needs a module path")?);
            }
            "--test" => {
                // Test builds turn on the `test` flag and swap the entry point
                // for one that runs what the modules export.
                opts.config.define("test");
                opts.test_mode = true;
            }
            "-D" | "--define" => {
                opts.config
                    .define_arg(&args.next().ok_or("`-D` needs a name or name=value")?);
            }
            "--release" => opts.config.define("release"),
            "--debug-cfg" => opts.config.define("debug"),
            "--print-cfg" => opts.print_cfg = true,
            "--modules" => opts.show_modules = true,
            "--dump" => opts.dump = true,
            "--ast" => opts.show_ast = true,
            "--no-emit" => opts.emit_file = false,
            "--no-debug" => opts.debug_info = false,
            other if other.starts_with('-') => return Err(format!("unknown option `{other}`")),
            other => {
                if input.is_some() {
                    return Err("only one input file is supported".into());
                }
                input = Some(PathBuf::from(other));
            }
        }
    }
    // `--print-cfg` answers a question about the compiler, not about a program.
    if opts.print_cfg {
        opts.input = input.unwrap_or_default();
        return Ok(opts);
    }
    opts.input = input.ok_or("no input file given (try `dreamc --help`)")?;
    Ok(opts)
}

fn main() -> ExitCode {
    let opts = match parse_args() {
        Ok(o) => o,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::FAILURE;
        }
    };
    match run(&opts) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprint!("{e}");
            ExitCode::FAILURE
        }
    }
}

fn run(opts: &Options) -> Result<(), String> {
    let path = &opts.input;
    let display = path.display().to_string();

    if opts.show_ast {
        // A debugging view of the root module only; parse it separately rather
        // than threading a "keep the tree" flag through the whole pipeline.
        let src = std::fs::read_to_string(path)
            .map_err(|e| format!("error: cannot read {display}: {e}\n"))?;
        let tokens = lexer::lex(&src).map_err(|_| "error: unrecognized character\n".to_string())?;
        let mut p = parser::Parser::new(&tokens, src.len());
        let module = p.parse_module();
        if p.diags.is_empty() {
            println!("{module:#?}");
        }
    }

    if opts.print_cfg {
        for line in opts.config.describe() {
            println!("{line}");
        }
        return Ok(());
    }

    let options = dreamc::Options {
        include: opts.include.clone(),
        package_roots: opts.package_roots.clone(),
        debug_info: opts.debug_info,
        config: opts.config.clone(),
        test_mode: opts.test_mode,
        host_modules: opts.host_modules.clone(),
    };

    if opts.show_packages {
        let packages = dreamc::package::PackageSet::discover(path, &opts.package_roots);
        match packages.current() {
            Some(c) => println!("current package: {} ({})", c.name, c.root.display()),
            None => println!("current package: none (no mind.toml above {})", display),
        }
        for p in &packages.packages {
            println!(
                "  {:<16} {:<10} {}\n      modules: {}",
                p.name,
                p.version,
                p.root.display(),
                {
                    let m = p.modules();
                    if m.is_empty() { "none".to_string() } else { m.join(", ") }
                }
            );
        }
        for n in &packages.notes {
            eprintln!("warning: {n}");
        }
        return Ok(());
    }

    let compiled = compile_program(path, &options).map_err(|e| e.render())?;

    if !compiled.warnings.is_empty() {
        eprint!("{}", render_program(&compiled.warnings, &compiled.files));
    }

    if opts.show_modules {
        for m in &compiled.program.modules {
            let name = &compiled.program.k.strings[m.name as usize];
            let src = &compiled.program.k.strings[m.source as usize];
            let mut tags = Vec::new();
            if m.flags & dreamc::ir::M_ABSTRACT != 0 {
                tags.push("abstract");
            }
            if m.derives != dreamc::ir::NO_NODE {
                let base = &compiled.program.k.strings
                    [compiled.program.modules[m.derives as usize].name as usize];
                tags.push(Box::leak(format!("derives {base}").into_boxed_str()));
            }
            println!(
                "{name:<24} {:>3} globals  {src}{}",
                m.globals_count,
                if tags.is_empty() { String::new() } else { format!("  [{}]", tags.join(" ")) }
            );
        }
    }

    if opts.dump {
        let img = disasm::Image::load(&compiled.image)
            .map_err(|e| format!("error: the image we just produced is invalid: {e}\n"))?;
        print!("{}", disasm::dump(&img));
    }

    if opts.emit_file {
        let out = opts.output.clone().unwrap_or_else(|| default_output(path));
        std::fs::write(&out, &compiled.image)
            .map_err(|e| format!("error: cannot write {}: {e}\n", out.display()))?;
        eprintln!(
            "compiled {display} -> {} ({} bytes, {} modules, {} nodes, {} functions)",
            out.display(),
            compiled.image.len(),
            compiled.program.modules.len(),
            compiled.program.nodes.len(),
            compiled.program.funcs.len()
        );
    }
    Ok(())
}

fn default_output(input: &Path) -> PathBuf {
    input.with_extension("dream")
}

