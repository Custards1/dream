//! Packages: the grouping layer above modules.
//!
//! A module is one file. A package is a named group of them, marked by a
//! `mind.toml` at its root. The point is that grouping should not depend on
//! where a directory happens to sit relative to whoever imports it: a package
//! declares its own name, so `import std.list` means "the module `list` in the
//! package called `std`", wherever that package lives.
//!
//! Anything can be a package -- dropping a manifest into a directory is the
//! whole ceremony -- and a project is itself a package, so its own modules are
//! reachable both as `mypkg.util` and, from inside, as plain `util`.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// Names accepted for a package manifest, in order of preference.
pub const MANIFEST_NAMES: &[&str] = &["mind.toml", "dusk.toml"];

/// Extensions a module file may have.
pub const MODULE_EXTENSIONS: &[&str] = &["dr"];

#[derive(Debug, Clone)]
pub struct Dep {
    pub name: String,
    /// Where to find it, relative to the depending package's root.
    pub path: Option<PathBuf>,
}

#[derive(Debug, Clone)]
pub struct Package {
    pub name: String,
    pub version: String,
    /// Directory holding the manifest.
    pub root: PathBuf,
    /// Where this package's modules live.
    pub src: PathBuf,
    pub deps: Vec<Dep>,
    pub manifest: PathBuf,
}

impl Package {
    /// The file for a module path inside this package, if it exists.
    pub fn module_file(&self, segments: &[&str]) -> Option<PathBuf> {
        if segments.is_empty() {
            return None;
        }
        let rel: PathBuf = segments.iter().collect();
        for ext in MODULE_EXTENSIONS {
            let f = self.src.join(&rel).with_extension(ext);
            if f.is_file() {
                return Some(f);
            }
        }
        // A module may grow into a directory without its importers changing.
        for ext in MODULE_EXTENSIONS {
            let f = self.src.join(&rel).join("mod").with_extension(ext);
            if f.is_file() {
                return Some(f);
            }
        }
        None
    }

    /// Every module this package provides, as dotted paths. Used for error
    /// messages and by the build system.
    pub fn modules(&self) -> Vec<String> {
        let mut out = Vec::new();
        collect_modules(&self.src, &self.src, &mut out);
        out.sort();
        out
    }
}

fn collect_modules(base: &Path, dir: &Path, out: &mut Vec<String>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            // Skip the usual build noise rather than reporting it as modules.
            let name = path.file_name().and_then(|s| s.to_str()).unwrap_or("");
            if name.starts_with('.') || name == "target" || name.starts_with("build") {
                continue;
            }
            collect_modules(base, &path, out);
        } else if path.extension().and_then(|s| s.to_str()).is_some_and(|e| {
            MODULE_EXTENSIONS.contains(&e)
        }) {
            if let Ok(rel) = path.strip_prefix(base) {
                let mut segs: Vec<String> = rel
                    .with_extension("")
                    .components()
                    .map(|c| c.as_os_str().to_string_lossy().into_owned())
                    .collect();
                if segs.last().map(|s| s.as_str()) == Some("mod") && segs.len() > 1 {
                    segs.pop();
                }
                out.push(segs.join("."));
            }
        }
    }
}

#[derive(Debug, Default)]
pub struct PackageSet {
    pub packages: Vec<Package>,
    by_name: HashMap<String, usize>,
    /// The package the root file belongs to.
    pub current: Option<usize>,
    /// Problems found while discovering, reported as warnings.
    pub notes: Vec<String>,
}

/// Whether two paths name the same directory. Falls back to comparing the
/// paths as written when either cannot be canonicalized, which is what happens
/// for a directory that does not exist.
fn same_directory(a: &Path, b: &Path) -> bool {
    match (std::fs::canonicalize(a), std::fs::canonicalize(b)) {
        (Ok(x), Ok(y)) => x == y,
        _ => a == b,
    }
}

impl PackageSet {
    pub fn find(&self, name: &str) -> Option<&Package> {
        self.by_name.get(name).map(|i| &self.packages[*i])
    }

    pub fn current(&self) -> Option<&Package> {
        self.current.map(|i| &self.packages[i])
    }

    pub fn names(&self) -> Vec<&str> {
        let mut n: Vec<&str> = self.packages.iter().map(|p| p.name.as_str()).collect();
        n.sort_unstable();
        n
    }

    fn add(&mut self, pkg: Package) -> Option<usize> {
        if let Some(&existing) = self.by_name.get(&pkg.name) {
            // Compare where the directories actually are, not how they were
            // spelled. The same package is routinely reached two ways -- once
            // as a path dependency (`../textstats`) and once under a `-L` root
            // -- and reporting that as a name clash would be a false alarm.
            let same = same_directory(&self.packages[existing].root, &pkg.root);
            if !same {
                self.notes.push(format!(
                    "two packages are named `{}`: {} and {}; using the first",
                    pkg.name,
                    self.packages[existing].root.display(),
                    pkg.root.display()
                ));
            }
            return Some(existing);
        }
        let i = self.packages.len();
        self.by_name.insert(pkg.name.clone(), i);
        self.packages.push(pkg);
        Some(i)
    }

    /// Find the package containing `file`, its dependencies, and any packages
    /// reachable from the extra roots.
    pub fn discover(file: &Path, extra_roots: &[PathBuf]) -> PackageSet {
        let mut set = PackageSet::default();

        // The package the root file lives in, found by walking up.
        let start = file.parent().map(|p| p.to_path_buf()).unwrap_or_default();
        let mut dir = Some(start);
        while let Some(d) = dir {
            if let Some(m) = manifest_in(&d) {
                match load_manifest(&m) {
                    Ok(Some(pkg)) => {
                        let i = set.add(pkg);
                        set.current = i;
                        break;
                    }
                    // A grouping directory is not this file's package; keep
                    // walking up in case a real one encloses it.
                    Ok(None) => {}
                    Err(e) => {
                        set.notes.push(e);
                        break;
                    }
                }
            }
            dir = d.parent().map(|p| p.to_path_buf());
        }

        let mut roots: Vec<PathBuf> = extra_roots.to_vec();
        if let Ok(env) = std::env::var("DREAM_PACKAGES") {
            roots.extend(env.split(':').filter(|s| !s.is_empty()).map(PathBuf::from));
        }
        for r in &roots {
            set.add_from_root(r);
        }

        // Dependencies, transitively. Indices are stable because `add` only
        // ever appends.
        let mut i = 0;
        while i < set.packages.len() {
            let deps = set.packages[i].deps.clone();
            let base = set.packages[i].root.clone();
            for dep in deps {
                let Some(rel) = dep.path else {
                    set.notes.push(format!(
                        "package `{}` depends on `{}` but gives no path; \
                         only path dependencies are supported so far",
                        set.packages[i].name, dep.name
                    ));
                    continue;
                };
                let path = if rel.is_absolute() { rel } else { base.join(rel) };
                match manifest_in(&path) {
                    Some(m) => match load_manifest(&m) {
                        Ok(Some(pkg)) => {
                            set.add(pkg);
                        }
                        Ok(None) => set.notes.push(format!(
                            "package `{}` depends on `{}` at {}, whose manifest names no package",
                            set.packages[i].name,
                            dep.name,
                            path.display()
                        )),
                        Err(e) => set.notes.push(e),
                    },
                    None => set.notes.push(format!(
                        "package `{}` depends on `{}` at {}, which has no manifest",
                        set.packages[i].name,
                        dep.name,
                        path.display()
                    )),
                }
            }
            i += 1;
        }
        set
    }

    /// A root may be a package itself, or a directory holding several.
    fn add_from_root(&mut self, root: &Path) {
        if let Some(m) = manifest_in(root) {
            match load_manifest(&m) {
                Ok(Some(pkg)) => {
                    self.add(pkg);
                    return;
                }
                // Names no package: it groups them, so look inside.
                Ok(None) => {}
                Err(e) => {
                    self.notes.push(e);
                    return;
                }
            }
        }
        let Ok(entries) = std::fs::read_dir(root) else { return };
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_dir() {
                continue;
            }
            if let Some(m) = manifest_in(&path) {
                match load_manifest(&m) {
                    Ok(Some(pkg)) => {
                        self.add(pkg);
                    }
                    Ok(None) => {}
                    Err(e) => self.notes.push(e),
                }
            }
        }
    }
}

pub fn manifest_in(dir: &Path) -> Option<PathBuf> {
    for name in MANIFEST_NAMES {
        let p = dir.join(name);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

/// Load a manifest.
///
/// `Ok(None)` means the file is there but names no package: the directory
/// groups packages rather than being one. That is a normal layout -- a
/// workspace root -- not a mistake, so it is not reported as one.
pub fn load_manifest(path: &Path) -> Result<Option<Package>, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    let doc = parse_toml(&text)
        .map_err(|e| format!("{}: {e}", path.display()))?;

    let root = path.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    let Some(name) = doc.get_str("package", "name").map(str::to_string) else {
        return Ok(None);
    };
    if name.contains('.') {
        return Err(format!(
            "{}: package name `{name}` cannot contain a dot; dots separate a \
             package from the modules inside it",
            path.display()
        ));
    }
    let version = doc.get_str("package", "version").unwrap_or("0.0.0").to_string();

    // `src` defaults to a `src` directory when there is one, so both flat and
    // conventional layouts work without configuration.
    let src = match doc.get_str("package", "src") {
        Some(s) => root.join(s),
        None if root.join("src").is_dir() => root.join("src"),
        None => root.clone(),
    };

    let mut deps = Vec::new();
    for (key, value) in doc.section("dependencies") {
        let path = match value {
            TomlValue::Str(s) => Some(PathBuf::from(s)),
            TomlValue::Table(t) => t.get("path").map(PathBuf::from),
        };
        deps.push(Dep { name: key.clone(), path });
    }

    Ok(Some(Package { name, version, root, src, deps, manifest: path.to_path_buf() }))
}

// ---------------------------------------------------------------------------
// A very small TOML reader
//
// Manifests are a handful of keys, so this reads the subset they use --
// sections, string values, and inline tables -- rather than pulling in a
// parser. Anything it does not understand is reported rather than ignored, so
// a manifest using more TOML than this fails loudly instead of silently
// dropping a dependency.
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub enum TomlValue {
    Str(String),
    Table(HashMap<String, String>),
}

#[derive(Debug, Default)]
pub struct TomlDoc {
    sections: Vec<(String, Vec<(String, TomlValue)>)>,
}

impl TomlDoc {
    pub fn get_str(&self, section: &str, key: &str) -> Option<&str> {
        self.sections
            .iter()
            .find(|(s, _)| s == section)?
            .1
            .iter()
            .find(|(k, _)| k == key)
            .and_then(|(_, v)| match v {
                TomlValue::Str(s) => Some(s.as_str()),
                TomlValue::Table(_) => None,
            })
    }

    pub fn section(&self, name: &str) -> Vec<(String, TomlValue)> {
        self.sections
            .iter()
            .find(|(s, _)| s == name)
            .map(|(_, kv)| kv.clone())
            .unwrap_or_default()
    }
}

pub fn parse_toml(text: &str) -> Result<TomlDoc, String> {
    let mut doc = TomlDoc::default();
    let mut current = String::new();

    for (lineno, raw) in text.lines().enumerate() {
        let line = strip_comment(raw).trim();
        if line.is_empty() {
            continue;
        }
        let at = || format!("line {}", lineno + 1);

        if let Some(rest) = line.strip_prefix('[') {
            let Some(name) = rest.strip_suffix(']') else {
                return Err(format!("{}: unterminated section header", at()));
            };
            current = name.trim().to_string();
            if !doc.sections.iter().any(|(s, _)| *s == current) {
                doc.sections.push((current.clone(), Vec::new()));
            }
            continue;
        }

        let Some(eq) = line.find('=') else {
            return Err(format!("{}: expected `key = value`", at()));
        };
        let key = line[..eq].trim().trim_matches('"').to_string();
        let value_text = line[eq + 1..].trim();
        let value = parse_value(value_text).ok_or_else(|| {
            format!("{}: `{value_text}` is not a string or an inline table", at())
        })?;

        if current.is_empty() {
            doc.sections.push((String::new(), vec![(key, value)]));
        } else {
            let entry = doc
                .sections
                .iter_mut()
                .find(|(s, _)| *s == current)
                .expect("section was created above");
            entry.1.push((key, value));
        }
    }
    Ok(doc)
}

fn strip_comment(line: &str) -> &str {
    let mut in_string = false;
    for (i, c) in line.char_indices() {
        match c {
            '"' => in_string = !in_string,
            '#' if !in_string => return &line[..i],
            _ => {}
        }
    }
    line
}

fn parse_value(text: &str) -> Option<TomlValue> {
    if let Some(inner) = text.strip_prefix('{').and_then(|t| t.strip_suffix('}')) {
        let mut table = HashMap::new();
        for part in inner.split(',') {
            let part = part.trim();
            if part.is_empty() {
                continue;
            }
            let eq = part.find('=')?;
            let k = part[..eq].trim().trim_matches('"').to_string();
            let v = unquote(part[eq + 1..].trim())?;
            table.insert(k, v);
        }
        return Some(TomlValue::Table(table));
    }
    unquote(text).map(TomlValue::Str)
}

fn unquote(text: &str) -> Option<String> {
    let t = text.trim();
    if t.len() >= 2 && ((t.starts_with('"') && t.ends_with('"')) || (t.starts_with('\'') && t.ends_with('\''))) {
        Some(t[1..t.len() - 1].to_string())
    } else if !t.is_empty() && !t.contains(['{', '}', '[', ']']) {
        // Bare values (numbers, booleans) are kept as text; nothing here needs
        // to interpret them further.
        Some(t.to_string())
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_a_manifest() {
        let doc = parse_toml(
            "# a package\n\
             [package]\n\
             name = \"std\"   # the standard library\n\
             version = \"0.1.0\"\n\
             \n\
             [dependencies]\n\
             core = { path = \"../core\" }\n\
             other = \"../other\"\n",
        )
        .unwrap();
        assert_eq!(doc.get_str("package", "name"), Some("std"));
        assert_eq!(doc.get_str("package", "version"), Some("0.1.0"));
        let deps = doc.section("dependencies");
        assert_eq!(deps.len(), 2);
        assert_eq!(
            deps[0].1,
            TomlValue::Table([("path".to_string(), "../core".to_string())].into_iter().collect())
        );
        assert_eq!(deps[1].1, TomlValue::Str("../other".into()));
    }

    #[test]
    fn a_hash_inside_a_string_is_not_a_comment() {
        let doc = parse_toml("[package]\nname = \"a#b\"\n").unwrap();
        assert_eq!(doc.get_str("package", "name"), Some("a#b"));
    }

    #[test]
    fn a_malformed_line_is_reported_with_its_number() {
        let err = parse_toml("[package]\nthis is not toml\n").unwrap_err();
        assert!(err.contains("line 2"), "{err}");
    }

    #[test]
    fn an_unterminated_section_is_reported() {
        assert!(parse_toml("[package\n").is_err());
    }
}
