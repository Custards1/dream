//! Module resolution and loading.
//!
//! Dream compiles whole programs. `import std.list` is resolved to a file, that
//! file is parsed, and its own imports are followed, until the entire reachable
//! program is in hand. Everything is then lowered into a single image.
//!
//! Whole-program compilation is what makes `derive` cheap: specializing a base
//! module's code against a deriving module's implementations needs the base
//! module's syntax tree, not just its signature. It also means a call to
//! another Dream module resolves to a global index at compile time rather than a
//! name lookup at run time.
//!
//! A module path that has no file behind it is assumed to be provided by the
//! host -- `std.console` is C++ in the VM, not Dream. Those are listed
//! explicitly so a typo in an import is an error rather than a mystery at run
//! time.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use crate::ast::{self, Item};
use crate::config::Config;
use crate::diag::{Diag, SourceFile};
use crate::lexer::{self, Span};
use crate::package::PackageSet;
use crate::parser;

/// Modules the VM provides natively. An `import` of one of these resolves to a
/// runtime lookup rather than to Dream source. This must match the runtime's
/// registry exactly; a test proves it does rather than trusting that it still
/// is.
///
/// An embedder can add to this list for one compilation with `--host-module`,
/// which is what makes a program that imports a host module registered through
/// `dream_vm_register_module` compilable. It is deliberately not a wildcard:
/// naming the module is what keeps a typo in an import an error rather than a
/// mystery at run time.
pub const NATIVE_MODULES: &[&str] =
    &["std.console", "std.core", "std.ffi", "std.io", "std.math", "std.net", "std.os", "std.vm"];

pub fn is_native(path: &str) -> bool {
    NATIVE_MODULES.contains(&path)
}

#[derive(Debug)]
pub struct LoadedModule {
    /// Dotted path, e.g. `std.list`. The root module is named for its file.
    pub name: String,
    pub path: PathBuf,
    /// Index into the program's source list, for diagnostics.
    pub source: usize,
    pub ast: ast::Module,
}

#[derive(Debug, Default)]
pub struct ModuleSet {
    /// Dependencies first, so lowering never sees a module before what it uses.
    pub modules: Vec<LoadedModule>,
    /// Every name a module answers to, including the spelling each importer
    /// used. Identity is the file, not the spelling: `std.list` from outside
    /// the package and `list` from inside must be the same module, or it would
    /// be compiled twice and its globals would not match.
    pub by_name: HashMap<String, usize>,
    by_path: HashMap<PathBuf, usize>,
    pub files: Vec<SourceFile>,
    /// Native module paths actually imported, in first-seen order.
    pub natives: Vec<String>,
    /// Dotted paths that name a *namespace* rather than a module -- a package
    /// brought in whole by `import std;`. Its modules are reached through it,
    /// as `std.list`.
    pub namespaces: HashSet<String>,
}

impl ModuleSet {
    pub fn find(&self, name: &str) -> Option<usize> {
        self.by_name.get(name).copied()
    }
    pub fn native_index(&self, name: &str) -> Option<usize> {
        self.natives.iter().position(|n| n == name)
    }

    pub fn find_path(&self, path: &Path) -> Option<usize> {
        self.by_path.get(path).copied()
    }
}

pub struct Loader {
    search_paths: Vec<PathBuf>,
    packages: PackageSet,
    config: Config,
    /// Build an entry point that runs every module's tests.
    test_mode: bool,
    /// When set, an import with no file behind it is assumed to be host
    /// provided rather than reported. Used for compiling a single source
    /// string, where there is no file system to search.
    assume_native: bool,
    /// Host modules this embedder registers at run time, named with
    /// `--host-module`. Treated exactly as `NATIVE_MODULES` are.
    host_modules: Vec<String>,
    set: ModuleSet,
    diags: Vec<Diag>,
    /// Modules currently being loaded, to spot an import cycle.
    in_progress: Vec<String>,
    visited: HashSet<String>,
}

impl Loader {
    pub fn new(search_paths: Vec<PathBuf>) -> Loader {
        Loader {
            search_paths,
            packages: PackageSet::default(),
            config: Config::new(),
            test_mode: false,
            assume_native: false,
            host_modules: Vec::new(),
            set: ModuleSet::default(),
            diags: Vec::new(),
            in_progress: Vec::new(),
            visited: HashSet::new(),
        }
    }

    /// Search paths for a root file: its own directory, anything in `MINDV2_PATH`,
    /// and the nearest enclosing `mind` directory, so a project's `dusk/std`
    /// is found without configuration.
    pub fn default_search_paths(root: &Path, extra: &[PathBuf]) -> Vec<PathBuf> {
        let mut paths: Vec<PathBuf> = extra.to_vec();
        if let Some(dir) = root.parent() {
            paths.push(dir.to_path_buf());
        }
        if let Ok(env) = std::env::var("MINDV2_PATH") {
            for part in env.split(':').filter(|p| !p.is_empty()) {
                paths.push(PathBuf::from(part));
            }
        }
        // Walk up looking for a `mind` directory holding a `std`.
        let mut dir = root.parent().map(|p| p.to_path_buf());
        while let Some(d) = dir {
            let candidate = d.join("mind");
            if candidate.join("std").is_dir() {
                paths.push(candidate);
            }
            dir = d.parent().map(|p| p.to_path_buf());
        }
        paths.dedup();
        paths
    }

    /// Load the root file and everything it reaches.
    ///
    /// The module set comes back even when loading failed, because it carries
    /// the source files the diagnostics point into -- an error that cannot
    /// quote its own line is much less use.
    pub fn load_root(mut self, root: &Path) -> (ModuleSet, Vec<Diag>) {
        // Name the root the way any other module in its package would be
        // named, so a module's identity does not depend on it being the entry
        // point.
        let fallback = root
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("main")
            .to_string();
        let name = self.canonical_name(root, &fallback);
        self.load_file(root, &name, None);
        if self.test_mode {
            self.add_test_runner();
        }
        (self.set, self.diags)
    }

    /// Build an entry point that runs the tests every loaded module exports.
    ///
    /// A module opts in by defining `tests`, conventionally inside a
    /// `when test { .. }` so it costs nothing in a normal build. Generating the
    /// runner from what was actually loaded means no registry to keep in step
    /// and no test that is silently never run.
    fn add_test_runner(&mut self) {
        let mut suites: Vec<(String, String)> = Vec::new();
        for (i, m) in self.set.modules.iter().enumerate() {
            let has_tests = m.ast.items.iter().any(
                |item| matches!(item, Item::Let(d) if d.name == "tests" && d.params.is_empty()),
            );
            if has_tests {
                suites.push((m.name.clone(), format!("dawn_test_module_{i}")));
            }
        }

        let mut src = String::from(
            "// Generated by `dreamc --test`: runs the tests each module exports.\n             import std.test as dawn_test_framework;\n",
        );
        for (module, alias) in &suites {
            src.push_str(&format!("import {module} as {alias};\n"));
        }
        src.push_str("let main! = dawn_test_framework.main_of! [\n");
        for (module, alias) in &suites {
            src.push_str(&format!("    [\"{module}\", {alias}.tests],\n"));
        }
        src.push_str("];\n");

        if suites.is_empty() {
            // Still a valid program: it reports that there was nothing to run,
            // which is more useful than an image with no entry point.
            src = String::from(
                "import std.console;\n                 let main! = { console.print! \"no tests found \" \
                 \"(a module exports tests by defining `tests`)\" };\n",
            );
        }

        self.load_text(src, "<generated test runner>", "dawn.test_main", None);
    }

    /// Give the loader a set of packages to resolve against.
    pub fn with_packages(mut self, packages: PackageSet) -> Loader {
        self.packages = packages;
        self
    }

    pub fn packages(&self) -> &PackageSet {
        &self.packages
    }

    pub fn with_config(mut self, config: Config) -> Loader {
        self.config = config;
        self
    }

    /// Extra module paths the host provides, beyond `NATIVE_MODULES`.
    pub fn with_host_modules(mut self, modules: Vec<String>) -> Loader {
        self.host_modules = modules;
        self
    }

    /// Is this path provided by the runtime rather than by Dream source?
    fn is_host_module(&self, path: &str) -> bool {
        is_native(path) || self.host_modules.iter().any(|m| m == path)
    }

    pub fn with_test_runner(mut self, yes: bool) -> Loader {
        self.test_mode = yes;
        self
    }

    pub fn assume_missing_are_native(mut self, yes: bool) -> Loader {
        self.assume_native = yes;
        self
    }

    /// Load a single module from memory, with no imports resolved from disk.
    pub fn load_source(mut self, name: &str, path: &str, text: &str) -> (ModuleSet, Vec<Diag>) {
        self.assume_native = true;
        self.load_text(text.to_string(), path, name, None);
        (self.set, self.diags)
    }

    pub fn take_diags(&mut self) -> Vec<Diag> {
        std::mem::take(&mut self.diags)
    }

    /// Turn a dotted path into a file.
    ///
    /// The first segment may name a package, which is what makes grouping
    /// independent of layout: `std.list` is the module `list` in whichever
    /// directory calls itself `std`. Failing that, the whole path is a module
    /// inside the current package, so a project's own files are reachable
    /// without repeating its name. The bare search paths come last, so a
    /// project with no manifest still works.
    fn resolve_path(&self, dotted: &str) -> Option<PathBuf> {
        let segments: Vec<&str> = dotted.split('.').collect();

        if segments.len() > 1 {
            if let Some(pkg) = self.packages.find(segments[0]) {
                if let Some(f) = pkg.module_file(&segments[1..]) {
                    return Some(f);
                }
            }
        }
        // A package may also hold a module named after itself: `std` meaning
        // `std/std.dr` or `std/mod.dr`.
        if let Some(pkg) = self.packages.find(segments[0]) {
            if segments.len() == 1 {
                if let Some(f) = pkg.module_file(&["mod"]).or_else(|| pkg.module_file(&[segments[0]])) {
                    return Some(f);
                }
            }
        }
        if let Some(cur) = self.packages.current() {
            if let Some(f) = cur.module_file(&segments) {
                return Some(f);
            }
        }

        let rel: PathBuf = segments.iter().collect();
        for base in &self.search_paths {
            for ext in crate::package::MODULE_EXTENSIONS {
                let candidate = base.join(&rel).with_extension(ext);
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
            // `std/list/mod.dr` lets a module grow into a directory later.
            let as_dir = base.join(&rel).join("mod.dr");
            if as_dir.is_file() {
                return Some(as_dir);
            }
        }
        None
    }

    /// Is `dep` a module already loaded under one of `current`'s enclosing
    /// scopes? `derive shape` inside `a.circle` means `a.shape`, the sibling,
    /// so the search walks outwards: `a.circle.shape`, then `a.shape`, then
    /// nothing. Returns the qualified name it found.
    fn local_module(&self, current: &str, dep: &str) -> Option<String> {
        let mut scope = current;
        loop {
            let candidate = format!("{scope}.{dep}");
            if self.set.by_name.contains_key(&candidate) {
                return Some(candidate);
            }
            match scope.rfind('.') {
                Some(cut) => scope = &scope[..cut],
                None => return None,
            }
        }
    }

    /// `import std;` where `std` is a package rather than a module: load every
    /// module the package provides, and record the name as a namespace so that
    /// `std.list` resolves through it.
    ///
    /// Returns false when nothing by that name is a package, so the caller can
    /// report the import as unresolved.
    fn load_package(&mut self, dotted: &str, at: Span, from_source: usize) -> bool {
        let Some(pkg) = self.packages.find(dotted) else { return false };
        let members = pkg.modules();
        if members.is_empty() {
            self.diags.push(
                Diag::error(at, format!("package `{dotted}` provides no modules"))
                    .with_note("a package's modules are the `.dr` files under its `src`")
                    .in_source(from_source),
            );
            return true;
        }
        self.set.namespaces.insert(dotted.to_string());
        self.visited.insert(dotted.to_string());
        for m in members {
            let path = format!("{dotted}.{m}");
            // A module of the package that is already being loaded is the one
            // doing the importing: `mind/std/all.dr` says `import std;`. Taking
            // it as a cycle would be wrong -- it is asking for its siblings.
            if self.in_progress.iter().any(|p| *p == path) {
                continue;
            }
            self.load_named(&path, at, from_source);
        }
        true
    }

    /// Load a module by dotted name, if it is not already loaded.
    fn load_named(&mut self, dotted: &str, at: Span, from_source: usize) {
        if self.visited.contains(dotted) || self.is_host_module(dotted) {
            if self.is_host_module(dotted) && !self.set.natives.iter().any(|n| n == dotted) {
                self.set.natives.push(dotted.to_string());
            }
            return;
        }
        if self.in_progress.iter().any(|m| m == dotted) {
            let cycle = self.in_progress.join(" -> ");
            self.diags.push(
                Diag::error(at, format!("import cycle: {cycle} -> {dotted}"))
                    .with_note("modules are compiled together, so their imports must form a tree")
                    .in_source(from_source),
            );
            return;
        }
        if self.assume_native {
            if !self.set.natives.iter().any(|n| n == dotted) {
                self.set.natives.push(dotted.to_string());
            }
            return;
        }
        let Some(path) = self.resolve_path(dotted) else {
            // No module by that name -- but a *package* by that name is
            // `import std;`, which brings in everything the package provides.
            if self.load_package(dotted, at, from_source) {
                return;
            }
            self.diags.push(self.unresolved(dotted, at, from_source));
            return;
        };
        // Identity is the file. If it is already loaded under another
        // spelling, record this name as another way to reach it.
        let canonical = std::fs::canonicalize(&path).unwrap_or_else(|_| path.clone());
        if let Some(existing) = self.set.by_path.get(&canonical).copied() {
            self.set.by_name.insert(dotted.to_string(), existing);
            self.visited.insert(dotted.to_string());
            return;
        }
        let name = self.canonical_name(&path, dotted);
        self.load_file(&path, &name, Some((at, from_source)));
        if let Some(&i) = self.set.by_name.get(&name) {
            self.set.by_name.insert(dotted.to_string(), i);
        }
        self.visited.insert(dotted.to_string());
    }

    /// Flatten `when` items, keeping only the branches whose condition holds.
    fn expand_items(&self, items: Vec<Item>, out: &mut Vec<Item>) {
        for item in items {
            match item {
                Item::When { cond, items, .. } => {
                    if self.config.eval(&cond) {
                        self.expand_items(items, out);
                    }
                }
                other => out.push(other),
            }
        }
    }

    /// The name a module is known by, regardless of how it was imported: the
    /// package it belongs to plus its path inside that package.
    fn canonical_name(&self, path: &Path, fallback: &str) -> String {
        // Compare where the file actually is, not how it was spelled. A
        // dependency reached as `../greet` produces a path that literally
        // begins with the *depending* package's directory, so a textual
        // `strip_prefix` claims the wrong owner and names the module
        // `app....greet.hello`.
        let real = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
        let mut best: Option<(usize, String)> = None;

        for pkg in &self.packages.packages {
            let src = std::fs::canonicalize(&pkg.src).unwrap_or_else(|_| pkg.src.clone());
            let Ok(rel) = real.strip_prefix(&src) else { continue };
            // A path that has to climb out of a directory does not belong to
            // the package rooted there, whatever the prefix says.
            if rel.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
                continue;
            }
            let mut segs: Vec<String> = rel
                .with_extension("")
                .components()
                .map(|c| c.as_os_str().to_string_lossy().into_owned())
                .collect();
            if segs.last().map(|s| s.as_str()) == Some("mod") && segs.len() > 1 {
                segs.pop();
            }
            let mut name = pkg.name.clone();
            if !segs.is_empty() {
                name.push('.');
                name.push_str(&segs.join("."));
            }
            // Nested packages are legal, so prefer the innermost one: the
            // longest `src` that still contains the file is its real owner.
            let depth = src.components().count();
            if best.as_ref().is_none_or(|(d, _)| depth > *d) {
                best = Some((depth, name));
            }
        }
        best.map(|(_, name)| name).unwrap_or_else(|| fallback.to_string())
    }

    /// Explain a failed import in terms of what was actually searched.
    fn unresolved(&self, dotted: &str, at: Span, from_source: usize) -> Diag {
        let mut note = String::new();
        let first = dotted.split('.').next().unwrap_or(dotted);

        if let Some(pkg) = self.packages.find(first) {
            let mods = pkg.modules();
            note.push_str(&format!(
                "package `{}` was found at {}, but has no module `{}`.",
                pkg.name,
                pkg.root.display(),
                dotted.strip_prefix(&format!("{first}.")).unwrap_or(dotted)
            ));
            if !mods.is_empty() {
                note.push_str(&format!("\n         It provides: {}", mods.join(", ")));
            }
        } else {
            let names = self.packages.names();
            let mut hosts: Vec<&str> = NATIVE_MODULES.to_vec();
            hosts.extend(self.host_modules.iter().map(String::as_str));
            note.push_str(&format!(
                "no package is named `{first}`, and no file matched.\n         \
                 Host-provided modules: {}.",
                hosts.join(", ")
            ));
            if self.host_modules.is_empty() {
                note.push_str(
                    "\n         An embedder's own host module must be named with \
                     `--host-module <path>` so that importing it compiles.",
                );
            }
            if names.is_empty() {
                note.push_str(
                    "\n         No packages were found. A package is any directory with a \
                     `mind.toml`; point at one with -L.",
                );
            } else {
                note.push_str(&format!("\n         Known packages: {}.", names.join(", ")));
            }
            if let Some(cur) = self.packages.current() {
                note.push_str(&format!(
                    "\n         Current package `{}` provides: {}.",
                    cur.name,
                    {
                        let m = cur.modules();
                        if m.is_empty() { "nothing".to_string() } else { m.join(", ") }
                    }
                ));
            }
        }

        Diag::error(at, format!("cannot find module `{dotted}`"))
            .with_note(note)
            .in_source(from_source)
    }

    /// Register `mod child { .. }` as the module `parent.child`, and return the
    /// import that replaces it in the parent.
    ///
    /// The submodule shares the parent's source file, so its diagnostics point
    /// at the right lines. It is deliberately *not* recorded in `by_path`:
    /// identity there is the file, and the file already belongs to the parent.
    fn hoist_mod(&mut self, parent: &str, source: usize, m: ast::ModDecl) -> ast::Item {
        let full = format!("{parent}.{}", m.name);
        let import = ast::Item::Import(ast::Import {
            path: full.split('.').map(str::to_string).collect(),
            alias: m.name.clone(),
            only: Vec::new(),
            span: m.span,
        });
        if self.set.by_name.contains_key(&full) {
            self.diags.push(
                Diag::error(m.name_span, format!("`{}` is already a module here", m.name))
                    .in_source(source),
            );
            return import;
        }

        // Its own `when`s and nested `mod`s, resolved the way a file's are.
        let mut kept = Vec::with_capacity(m.items.len());
        self.expand_items(m.items, &mut kept);
        let mut items = Vec::with_capacity(kept.len());
        for item in kept {
            match item {
                ast::Item::Mod(inner) => items.push(self.hoist_mod(&full, source, inner)),
                other => items.push(other),
            }
        }

        // A submodule may import, so follow those before it is recorded.
        self.in_progress.push(full.clone());
        let deps: Vec<(String, Span)> = items
            .iter()
            .filter_map(|item| match item {
                ast::Item::Import(i) => Some((i.path.join("."), i.span)),
                ast::Item::Derive(d) => Some((d.path.join("."), d.span)),
                _ => None,
            })
            .collect();
        for (dep, span) in deps {
            // Its own submodule, put there by the hoist above, or a sibling
            // reached by walking outwards: either way already registered.
            if self.set.by_name.contains_key(&dep) || self.local_module(&full, &dep).is_some() {
                continue;
            }
            self.load_named(&dep, span, source);
        }
        self.in_progress.pop();

        let index = self.set.modules.len();
        self.set.modules.push(LoadedModule {
            name: full.clone(),
            path: PathBuf::from(&full),
            source,
            ast: ast::Module { items },
        });
        self.set.by_name.insert(full.clone(), index);
        self.visited.insert(full);
        import
    }

    fn load_file(&mut self, path: &Path, name: &str, imported_at: Option<(Span, usize)>) {
        if self.visited.contains(name) {
            return;
        }
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) => {
                let (span, source) = imported_at.unwrap_or((Span::default(), 0));
                self.diags.push(
                    Diag::error(span, format!("cannot read {}: {e}", path.display()))
                        .in_source(source),
                );
                return;
            }
        };
        self.load_text(text, &path.display().to_string(), name, Some(path.to_path_buf()));
    }

    fn load_text(
        &mut self,
        text: String,
        display_path: &str,
        name: &str,
        path: Option<PathBuf>,
    ) {
        let path = path.unwrap_or_else(|| PathBuf::from(display_path));
        let source = self.set.files.len();
        self.set.files.push(SourceFile { path: display_path.to_string(), text: text.clone() });

        let tokens = match lexer::lex(&text) {
            Ok(t) => t,
            Err(e) => {
                self.diags
                    .push(Diag::error(e.span, "unrecognized character").in_source(source));
                return;
            }
        };
        let mut p = parser::Parser::new(&tokens, text.len());
        let mut module_ast = p.parse_module();
        for d in p.diags {
            self.diags.push(d.in_source(source));
        }

        // Resolve `when` before anything else looks at the module, so the rest
        // of the compiler never sees a conditional item at all.
        let mut kept = Vec::with_capacity(module_ast.items.len());
        self.expand_items(module_ast.items, &mut kept);
        module_ast.items = kept;

        self.in_progress.push(name.to_string());

        // A `mod name { .. }` is a module written inside another. Register it
        // as `parent.name` and leave the parent importing it, so from here on
        // a submodule and a file are the same thing to every later pass.
        // Submodules go into the list first, which keeps the "dependencies
        // first" order the lowerer relies on.
        let mut hoisted = Vec::with_capacity(module_ast.items.len());
        for item in std::mem::take(&mut module_ast.items) {
            match item {
                ast::Item::Mod(m) => hoisted.push(self.hoist_mod(name, source, m)),
                other => hoisted.push(other),
            }
        }
        module_ast.items = hoisted;

        // Follow this module's own imports before recording it, so the module
        // list comes out with dependencies first.
        let mut deps: Vec<(String, Span)> = Vec::new();
        for item in &module_ast.items {
            match item {
                ast::Item::Import(i) => deps.push((i.path.join("."), i.span)),
                ast::Item::Derive(d) => deps.push((d.path.join("."), d.span)),
                _ => {}
            }
        }
        for (dep, span) in deps {
            // `import io.{ .. }` inside a module that declares `mod io { .. }`
            // means *that* submodule, which is registered as `parent.io`. It is
            // already loaded, so there is nothing to search the file system for.
            if self.local_module(name, &dep).is_some() {
                continue;
            }
            self.load_named(&dep, span, source);
        }
        self.in_progress.pop();

        self.visited.insert(name.to_string());
        let index = self.set.modules.len();
        let canonical = std::fs::canonicalize(&path).unwrap_or_else(|_| path.clone());
        self.set.modules.push(LoadedModule {
            name: name.to_string(),
            path,
            source,
            ast: module_ast,
        });
        self.set.by_name.insert(name.to_string(), index);
        self.set.by_path.insert(canonical, index);
    }
}
