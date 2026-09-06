//! The bridge from the compiler to the VM.
//!
//! `comp!` evaluates an expression while compiling and is allowed to perform
//! effects. Rather than growing the compiler's own evaluator until it can do
//! that -- and drifting away from what the same code means at run time -- the
//! compiler stages the expression into a small image and runs it on the real
//! VM, then reads the answer back.
//!
//! The VM is loaded at run time rather than linked, so the compiler still
//! builds and runs on its own. Without it, `comp` still works (the compiler
//! evaluates pure expressions itself) and `comp!` reports that it needs the VM.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint, c_void};
use std::path::{Path, PathBuf};

use crate::consteval::CValue;

type DreamValue = u64;
const DREAM_OK: c_int = 1;

/// Mirrors `dream_type` in include/dream/core.h.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
enum DreamType {
    Integer = 0,
    Float,
    Char,
    Bool,
    Unit,
    String,
    Atom,
    List,
    Array,
    Map,
    PureFn,
    ImpureFn,
    Module,
    Error,
    Process,
    Unknown,
}

impl DreamType {
    fn from_u32(v: u32) -> DreamType {
        if v <= DreamType::Unknown as u32 {
            // Safe: the enum is repr(u32) and densely numbered.
            unsafe { std::mem::transmute::<u32, DreamType>(v) }
        } else {
            DreamType::Unknown
        }
    }
}

/// Look up one symbol and copy the function pointer out. The `Symbol` borrows
/// the library only for the lookup; a bare function pointer carries no
/// lifetime, and the library is kept alive in the same struct that holds it.
macro_rules! sym {
    ($lib:expr, $name:literal, $ty:ty) => {
        unsafe {
            *$lib
                .get::<$ty>(concat!($name, "\0").as_bytes())
                .map_err(|e| format!("libdream is missing `{}`: {e}", $name))?
        }
    };
}

pub struct Vm {
    _lib: libloading::Library,
    path: PathBuf,

    vm_new: unsafe extern "C" fn() -> *mut c_void,
    vm_free: unsafe extern "C" fn(*mut c_void),
    vm_load_bytes:
        unsafe extern "C" fn(*mut c_void, *const u8, usize, *mut c_char, usize) -> c_int,
    vm_set_workers: unsafe extern "C" fn(*mut c_void, c_uint),
    vm_run_value: unsafe extern "C" fn(*mut c_void, *const c_char, *mut DreamValue) -> c_int,
    vm_result_text: unsafe extern "C" fn(*mut c_void) -> *const c_char,
    vm_atom_name: unsafe extern "C" fn(*mut c_void, DreamValue) -> *const c_char,

    value_type: unsafe extern "C" fn(DreamValue) -> u32,
    value_integer: unsafe extern "C" fn(DreamValue) -> i64,
    value_float: unsafe extern "C" fn(DreamValue) -> f64,
    value_bool: unsafe extern "C" fn(DreamValue) -> c_int,
    value_char: unsafe extern "C" fn(DreamValue) -> u32,
    value_string: unsafe extern "C" fn(DreamValue, *mut u32) -> *const c_char,
    list_next: unsafe extern "C" fn(DreamValue, *mut DreamValue, *mut DreamValue) -> c_int,
    array_len: unsafe extern "C" fn(DreamValue) -> u32,
    array_at: unsafe extern "C" fn(DreamValue, u32) -> DreamValue,
    map_next: unsafe extern "C" fn(DreamValue, *mut u32, *mut DreamValue, *mut DreamValue) -> c_int,
}

impl Vm {
    /// Where to look for the VM, in order: an explicit `DREAM_VM_LIB`, then the
    /// build trees a checkout produces, then the loader's own search path.
    pub fn candidate_paths(exe_dir: Option<&Path>) -> Vec<PathBuf> {
        let mut out = Vec::new();
        if let Ok(p) = std::env::var("DREAM_VM_LIB") {
            out.push(PathBuf::from(p));
        }
        let relative = [
            "build-dream/lib/libdream.so",
            "build/lib/libdream.so",
            "dawn/build/lib/libdream.so",
            "../build-dream/lib/libdream.so",
            "../dawn/build/lib/libdream.so",
            "../../build-dream/lib/libdream.so",
        ];
        if let Some(dir) = exe_dir {
            // target/debug/dreamc -> walk up to the project root.
            for up in [dir.to_path_buf(), dir.join(".."), dir.join("../.."), dir.join("../../..")] {
                for r in relative {
                    out.push(up.join(r));
                }
            }
        }
        for r in relative {
            out.push(PathBuf::from(r));
        }
        out.push(PathBuf::from("libdream.so"));
        out
    }

    pub fn open_default() -> Result<Vm, String> {
        let exe = std::env::current_exe().ok();
        let exe_dir = exe.as_deref().and_then(|p| p.parent()).map(|p| p.to_path_buf());
        let mut last = String::from("no candidate paths");
        for c in Vm::candidate_paths(exe_dir.as_deref()) {
            if c.as_os_str().is_empty() {
                continue;
            }
            match Vm::open(&c) {
                Ok(vm) => return Ok(vm),
                Err(e) => last = format!("{}: {e}", c.display()),
            }
        }
        Err(last)
    }

    pub fn open(path: &Path) -> Result<Vm, String> {
        // Loading an arbitrary shared object is inherently unsafe; the safety
        // argument is that we only ever call the symbols below, with the
        // signatures the header declares.
        let lib = unsafe { libloading::Library::new(path) }.map_err(|e| e.to_string())?;
        let vm = Vm {
            vm_new: sym!(lib, "dream_vm_new", unsafe extern "C" fn() -> *mut c_void),
            vm_free: sym!(lib, "dream_vm_free", unsafe extern "C" fn(*mut c_void)),
            vm_load_bytes: sym!(
                lib,
                "dream_vm_load_bytes",
                unsafe extern "C" fn(*mut c_void, *const u8, usize, *mut c_char, usize) -> c_int
            ),
            vm_set_workers: sym!(
                lib,
                "dream_vm_set_workers",
                unsafe extern "C" fn(*mut c_void, c_uint)
            ),
            vm_run_value: sym!(
                lib,
                "dream_vm_run_value",
                unsafe extern "C" fn(*mut c_void, *const c_char, *mut DreamValue) -> c_int
            ),
            vm_result_text: sym!(
                lib,
                "dream_vm_result_text",
                unsafe extern "C" fn(*mut c_void) -> *const c_char
            ),
            vm_atom_name: sym!(
                lib,
                "dream_vm_atom_name",
                unsafe extern "C" fn(*mut c_void, DreamValue) -> *const c_char
            ),
            value_type: sym!(lib, "dream_value_type", unsafe extern "C" fn(DreamValue) -> u32),
            value_integer: sym!(lib, "dream_value_integer", unsafe extern "C" fn(DreamValue) -> i64),
            value_float: sym!(lib, "dream_value_float", unsafe extern "C" fn(DreamValue) -> f64),
            value_bool: sym!(lib, "dream_value_bool", unsafe extern "C" fn(DreamValue) -> c_int),
            value_char: sym!(lib, "dream_value_char", unsafe extern "C" fn(DreamValue) -> u32),
            value_string: sym!(
                lib,
                "dream_value_string",
                unsafe extern "C" fn(DreamValue, *mut u32) -> *const c_char
            ),
            list_next: sym!(
                lib,
                "dream_value_list_next",
                unsafe extern "C" fn(DreamValue, *mut DreamValue, *mut DreamValue) -> c_int
            ),
            array_len: sym!(lib, "dream_value_array_len", unsafe extern "C" fn(DreamValue) -> u32),
            array_at: sym!(
                lib,
                "dream_value_array_at",
                unsafe extern "C" fn(DreamValue, u32) -> DreamValue
            ),
            map_next: sym!(
                lib,
                "dream_value_map_next",
                unsafe extern "C" fn(DreamValue, *mut u32, *mut DreamValue, *mut DreamValue) -> c_int
            ),
            path: path.to_path_buf(),
            _lib: lib,
        };
        Ok(vm)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Run `entry` in `image` and read the result back as a compile-time value.
    pub fn eval(&self, image: &[u8], entry: &str) -> Result<CValue, String> {
        unsafe {
            let vm = (self.vm_new)();
            if vm.is_null() {
                return Err("could not create a VM".into());
            }
            // One worker: compile-time evaluation should be reproducible, and
            // nothing here benefits from parallelism.
            (self.vm_set_workers)(vm, 1);

            let mut err = [0i8; 256];
            if (self.vm_load_bytes)(vm, image.as_ptr(), image.len(), err.as_mut_ptr(), err.len())
                != DREAM_OK
            {
                let msg = CStr::from_ptr(err.as_ptr()).to_string_lossy().into_owned();
                (self.vm_free)(vm);
                return Err(format!("staging image rejected: {msg}"));
            }

            let entry_c = CString::new(entry).map_err(|_| "bad entry name")?;
            let mut value: DreamValue = 0;
            let ok = (self.vm_run_value)(vm, entry_c.as_ptr(), &mut value) == DREAM_OK;
            if !ok {
                let text = CStr::from_ptr((self.vm_result_text)(vm)).to_string_lossy().into_owned();
                (self.vm_free)(vm);
                return Err(if text.is_empty() { "evaluation failed".into() } else { text });
            }

            let converted = self.convert(vm, value, 0);
            (self.vm_free)(vm);
            converted
        }
    }

    unsafe fn convert(&self, vm: *mut c_void, v: DreamValue, depth: u32) -> Result<CValue, String> {
        if depth > 256 {
            return Err("`comp!` produced a value too deeply nested to bake in".into());
        }
        unsafe {
            match DreamType::from_u32((self.value_type)(v)) {
                DreamType::Integer => Ok(CValue::Int((self.value_integer)(v))),
                DreamType::Float => Ok(CValue::Float((self.value_float)(v))),
                DreamType::Bool => Ok(CValue::Bool((self.value_bool)(v) != 0)),
                DreamType::Unit => Ok(CValue::Unit),
                DreamType::Char => Ok(CValue::Char(
                    char::from_u32((self.value_char)(v)).unwrap_or('\u{fffd}'),
                )),
                DreamType::String => {
                    let mut len: u32 = 0;
                    let p = (self.value_string)(v, &mut len);
                    if p.is_null() {
                        return Ok(CValue::Str(String::new()));
                    }
                    let bytes = std::slice::from_raw_parts(p as *const u8, len as usize);
                    Ok(CValue::Str(String::from_utf8_lossy(bytes).into_owned()))
                }
                DreamType::Atom => {
                    let p = (self.vm_atom_name)(vm, v);
                    if p.is_null() {
                        return Err("`comp!` produced an unnamed atom".into());
                    }
                    Ok(CValue::Atom(CStr::from_ptr(p).to_string_lossy().into_owned()))
                }
                DreamType::List => {
                    let mut items = Vec::new();
                    let mut cur = v;
                    loop {
                        let mut head: DreamValue = 0;
                        let mut tail: DreamValue = 0;
                        if (self.list_next)(cur, &mut head, &mut tail) != DREAM_OK {
                            break;
                        }
                        items.push(self.convert(vm, head, depth + 1)?);
                        cur = tail;
                    }
                    Ok(CValue::List(items))
                }
                DreamType::Array => {
                    let n = (self.array_len)(v);
                    let mut items = Vec::with_capacity(n as usize);
                    for i in 0..n {
                        items.push(self.convert(vm, (self.array_at)(v, i), depth + 1)?);
                    }
                    Ok(CValue::Array(items))
                }
                DreamType::Map => {
                    let mut pairs = Vec::new();
                    let mut cursor: u32 = 0;
                    loop {
                        let mut k: DreamValue = 0;
                        let mut val: DreamValue = 0;
                        if (self.map_next)(v, &mut cursor, &mut k, &mut val) != DREAM_OK {
                            break;
                        }
                        pairs.push((
                            self.convert(vm, k, depth + 1)?,
                            self.convert(vm, val, depth + 1)?,
                        ));
                    }
                    Ok(CValue::Map(pairs))
                }
                other => Err(format!(
                    "`comp!` produced {other:?}, which cannot be baked into an image"
                )),
            }
        }
    }
}

impl Drop for Vm {
    fn drop(&mut self) {}
}
