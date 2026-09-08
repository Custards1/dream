// The `dream` command: load a bytecode image and run it.

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <filesystem>

#include "builtins.hpp"
#include "image.hpp"
#include "interp.hpp"
#include "jit.hpp"
#include "process.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"

using namespace dream;

namespace {

const char* USAGE =
    "dream -- the Dream virtual machine\n"
    "\n"
    "usage: dream <image> [options]\n"
    "\n"
    "<image> is a file, that file with `.dream` added, or either of those in\n"
    "$MINDV2_PATH -- so `dream mind` runs ./mind.dream, or the installed one.\n"
    "\n"
    "options:\n"
    "  -x, --exec <name>    run <name>.dream from $MINDV2_PATH, not from here\n"
    "  -e, --entry <name>   run this global instead of `main!`\n"
    "  -j, --workers <n>    scheduler threads (default: one per core)\n"
    "      --dump           disassemble the image and exit\n"
    "      --stats          print reduction and heap statistics\n"
    "      --profile [n]    count reductions per function and print the hottest\n"
    "      --no-jit         stay in the interpreter\n"
    "      --jit-threshold <n>  calls before a function is compiled\n"
    "      --dump-jit <fn>  print the LLVM IR generated for a function\n"
    "      --mindv2         mindv2 path override\n"
    "  -h, --help           show this message\n";
void dump_node(const Image& img, uint32_t idx, int depth, std::string& out);

void indent(std::string& out, int depth) { out.append(size_t(depth) * 2, ' '); }

void dump_kids(const Image& img, uint32_t off, uint32_t count, int depth, std::string& out) {
    for (uint32_t i = 0; i < count; ++i) dump_node(img, img.kid(off + i), depth, out);
}

void dump_node(const Image& img, uint32_t idx, int depth, std::string& out) {
    if (idx == NO_NODE) {
        indent(out, depth);
        out += "<none>\n";
        return;
    }
    const Node& n = img.node(idx);
    indent(out, depth);
    char buf[128];
    std::snprintf(buf, sizeof buf, "%%%u %s", idx, op_name(Op(n.op)));
    out += buf;

    switch (Op(n.op)) {
        case Op::ConstInt:
            std::snprintf(buf, sizeof buf, " %" PRId64, img.integer(n.a));
            out += buf;
            break;
        case Op::ConstFloat:
            std::snprintf(buf, sizeof buf, " %g", img.real(n.a));
            out += buf;
            break;
        case Op::ConstStr: out += " \"" + img.str(n.a).str() + "\""; break;
        case Op::ConstAtom: out += " :" + img.atom_name(n.a).str(); break;
        case Op::ConstBool: out += n.a ? " true" : " false"; break;
        case Op::ConstChar:
            std::snprintf(buf, sizeof buf, " U+%04X", n.a);
            out += buf;
            break;
        case Op::Local:
        case Op::Capture:
            std::snprintf(buf, sizeof buf, " %u", n.a);
            out += buf;
            break;
        case Op::Global:
            out += " @" + img.str(img.global(n.a).name).str();
            break;
        case Op::Builtin: out += std::string(" ") + builtin_def(n.a).name; break;
        case Op::Field: out += " ." + img.str(n.b).str(); break;
        case Op::MakeClosure:
        case Op::MakeThunk:
            out += " fn#" + std::to_string(n.a) + " " + img.str(img.func(n.a).name).str();
            break;
        case Op::Apply:
            std::snprintf(buf, sizeof buf, "/%u", n.c);
            out += buf;
            break;
        case Op::Block:
        case Op::MakeList:
        case Op::MakeArray:
        case Op::MakeMap:
            std::snprintf(buf, sizeof buf, "/%u", n.b);
            out += buf;
            break;
        case Op::Bind:
            std::snprintf(buf, sizeof buf, " $%u", n.a);
            out += buf;
            break;
        default: break;
    }

    std::string flags;
    if (n.flags & F_STRICT) flags += "strict,";
    if (n.flags & F_IMPURE) flags += "impure,";
    if (n.flags & F_TAIL) flags += "tail,";
    if (!flags.empty()) {
        flags.pop_back();
        out += "  ; " + flags;
    }
    out += "\n";

    switch (Op(n.op)) {
        case Op::Field:
        case Op::Force:
        case Op::Neg:
        case Op::Not:
            dump_node(img, n.a, depth + 1, out);
            break;
        case Op::Apply:
            dump_node(img, n.a, depth + 1, out);
            dump_kids(img, n.b, n.c, depth + 1, out);
            break;
        case Op::If:
            dump_node(img, n.a, depth + 1, out);
            dump_node(img, n.b, depth + 1, out);
            dump_node(img, n.c, depth + 1, out);
            break;
        case Op::Block:
        case Op::MakeList:
        case Op::MakeArray:
            dump_kids(img, n.a, n.b, depth + 1, out);
            break;
        case Op::MakeMap:
            dump_kids(img, n.a, n.b * 2, depth + 1, out);
            break;
        case Op::Bind:
            dump_node(img, n.b, depth + 1, out);
            break;
        case Op::Try:
            dump_node(img, n.a, depth + 1, out);
            dump_node(img, n.b, depth + 1, out);
            break;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::And: case Op::Or:
            dump_node(img, n.a, depth + 1, out);
            dump_node(img, n.b, depth + 1, out);
            break;
        default: break;
    }
}

void dump_image(const Image& img) {
    std::printf("; module \"%s\"  source \"%s\"  version %u.%u%s\n",
                img.module_name().str().c_str(), img.source_name().str().c_str(),
                img.version_major(), img.version_minor(),
                img.has_debug_info() ? "  (debug)" : "");
    if (img.entry() != NO_NODE) {
        std::printf("; entry fn#%u `%s`\n", img.entry(),
                    img.str(img.func(img.entry()).name).str().c_str());
    }
    for (uint32_t i = 0; i < img.import_count(); ++i) {
        const ImportRec& ir = img.import(i);
        std::printf("import %s as %s\n", img.str(ir.path).str().c_str(),
                    img.str(ir.alias).str().c_str());
    }
    for (uint32_t i = 0; i < img.func_count(); ++i) {
        const FuncRec& f = img.func(i);
        std::printf("\nfn#%u %s  arity %u  slots %u  captures %u  [%s%s%s%s]\n", i,
                    img.str(f.name).str().c_str(), f.arity, f.slots, f.n_captures,
                    (f.flags & FN_IMPURE) ? "impure" : "pure",
                    (f.flags & FN_REC) ? " rec" : "",
                    (f.flags & FN_THUNK) ? " thunk" : "",
                    (f.flags & FN_GLOBAL_VALUE) ? " memoized" : "");
        std::string out;
        dump_node(img, f.body, 1, out);
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
}

}  // namespace


bool is_directory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

/// Resolve the image to run.
///
/// `dream mind` should mean what `mind` means when it is typed on its own, so a
/// name is tried four ways, nearest first:
///
///   1. as written, which is what a path is;
///   2. with `.dream` added, so `dream mind` runs `./mind.dream` -- an image is
///      a program, and naming its extension every time is noise;
///   3. and 4., both of those under `$MINDV2_PATH`, where an installation keeps
///      what it ships, the way a shell finds a binary on PATH.
///
/// The working directory comes before the installation, because a project's own
/// build is what someone standing in it means. The installed lookup is for a
/// bare name only: a path with a separator in it is a place, and answering it
/// with a file from somewhere else would be a surprise. `dream -x NAME` is the
/// other half of this -- the installation and nothing else.
std::string skip_file(std::string& path, const std::string& MINDV2_PATH, bool* file_ok) {
    *file_ok = true;

    const bool is_bare_name =
        path.find('/') == std::string::npos && path.find('\\') == std::string::npos;

    std::vector<std::string> candidates{path, path + ".dream"};
    if (is_bare_name && !MINDV2_PATH.empty()) {
        candidates.push_back(MINDV2_PATH + "/" + path);
        candidates.push_back(MINDV2_PATH + "/" + path + ".dream");
    }
    for (const std::string& candidate : candidates) {
        if (std::filesystem::exists(candidate) && !is_directory(candidate)) return candidate;
    }

    *file_ok = false;
    return "";
}

/// `-x name`: the installed image called `name`, and nothing else.
///
/// This is the same lookup `skip_file` falls back to, without the fallback. A
/// bare `dream lucid` prefers a file called `lucid` in the working directory,
/// which is what a path should mean; `-x lucid` says the installation is the
/// only place to look, so a stray file next to the caller cannot shadow an
/// installed program.
std::string installed_image(const std::string& name, const std::string& MINDV2_PATH,
                            bool* file_ok) {
    *file_ok = true;
    if (MINDV2_PATH.empty()) {
        std::fprintf(stderr,
                     "dream: -x needs $MINDV2_PATH (or ~/.mindv2) to look in, to find `%s`\n",
                     name.c_str());
        *file_ok = false;
        return "";
    }
    const std::string candidates[] = {
        MINDV2_PATH + "/" + name,
        MINDV2_PATH + "/" + name + ".dream",
    };
    for (const std::string& candidate : candidates) {
        if (std::filesystem::exists(candidate) && !is_directory(candidate)) return candidate;
    }
    std::fprintf(stderr, "dream: no installed image `%s` in %s\n", name.c_str(),
                 MINDV2_PATH.c_str());
    *file_ok = false;
    return "";
}

/// Where installed images live: `$MINDV2_PATH`, or `~/.mindv2` when that
/// directory exists.
///
/// Returned by value throughout. The obvious way to write this -- keep a
/// `const char*` and point it at a local string's `c_str()` -- leaves the
/// pointer dangling the moment that string goes out of scope, which it does
/// before the return. It survived only because a short string lives in the
/// object itself and the stack slot happened to still hold the bytes.
std::string home_dir() {
    #if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
    #else
    const char* home = std::getenv("HOME");
    #endif
    // `getenv` answers null for a variable that is not set, and constructing a
    // `std::string` from null is undefined rather than empty.
    return home ? std::string(home) : std::string();
}

/// A leading `~` means the home directory -- here, rather than only in a shell.
///
/// The shell expands a tilde it can see, and `export MINDV2_PATH="~/.mindv2"`
/// hides it inside quotes, so what arrives is a literal `~`. Nothing on disk is
/// called that, so every lookup under it silently found nothing, which reads as
/// "the image is not installed" when it is sitting right there. A path is not
/// text to this program, so expanding it is this program's job.
std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    if (path.size() > 1 && path[1] != '/' && path[1] != '\\') return path;  // `~other`, a user
    const std::string home = home_dir();
    if (home.empty()) return path;
    return home + path.substr(1);
}

std::string get_mindv2_path() {
    if (const char* from_env = std::getenv("MINDV2_PATH")) {
        return expand_home(std::string(from_env));
    }
    const std::string home = home_dir();
    if (home.empty()) return "";
    std::string candidate = home + "/.mindv2";
    if (std::filesystem::exists(candidate)) return candidate;
    return "";
}

int main(int argc, char** argv) {
    std::string path, entry;
    std::string dump_jit_fn;
    unsigned workers = 0;
    bool dump = false, stats = false, use_jit = true;
    size_t profile_top = 0;
    uint32_t jit_threshold = 0;

    std::vector<std::string> program_args;
    bool past_image = false;
    // `-x` has already resolved the image, so the working directory must not be
    // consulted for it again.
    bool installed = false;
    std::string MINDV2_PATH = get_mindv2_path(); 
    
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // Once the image is named, stop interpreting anything as a VM option.
        if (past_image) {
            program_args.push_back(a);
            continue;
        }
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dream: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(USAGE, stdout);
            return 0;
        } else if (a == "-x" || a == "--exec") {
            bool ok = true;
            path = installed_image(next("--exec"), MINDV2_PATH, &ok);
            if (!ok) return 1;
            installed = true;
            past_image = true;
        } else if (a == "-e" || a == "--entry") {
            entry = next("--entry");
        } else if (a == "-j" || a == "--workers") {
            workers = unsigned(std::stoul(next("--workers")));
        } else if (a == "--dump") {
            dump = true;
        } else if (a == "--stats") {
            stats = true;
        } else if (a == "--profile") {
            // The count is optional: `--profile` alone shows a screenful.
            profile_top = 25;
            if (i + 1 < argc && argv[i + 1][0] != '-' && std::isdigit(argv[i + 1][0])) {
                profile_top = size_t(std::stoul(argv[++i]));
            }
        } else if (a == "--no-jit") {
            use_jit = false;
        }else if (a == "-m" || a == "--mindv2") {
            MINDV2_PATH = expand_home(next("--mindv2"));
        } else if (a == "--jit-threshold") {
            jit_threshold = uint32_t(std::stoul(next("--jit-threshold")));
        } else if (a == "--dump-jit") {
            dump_jit_fn = next("--dump-jit");
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "dream: unknown option `%s`\n", a.c_str());
            return 2;
        } else if (path.empty()) {
            path = a;
            past_image = true;
        } else {
            // Everything after the image belongs to the program, not to the
            // VM -- including anything that looks like an option, which is why
            // this branch is reached before the `-` check can complain about
            // it. `std.os.args!` is where it arrives.
            program_args.push_back(a);
        }
    }

    if (path.empty()) {
        std::fputs(USAGE, stderr);
        return 2;
    }
    bool file_ok = true;
    // The name as it was typed, because that is the one worth reporting: what
    // the lookup answers on a miss is nothing at all.
    const std::string asked = path;
    if (!installed) path = skip_file(path, MINDV2_PATH, &file_ok);
    if (!file_ok) {
        std::fprintf(stderr, "dream: no such image `%s`\n", asked.c_str());
        return 1;
    }

    Runtime rt;
    rt.set_program_args(std::move(program_args));
    std::string error;
    if (!rt.load_image_file(path, error)) {
        std::fprintf(stderr, "dream: %s\n", error.c_str());
        return 1;
    }

    if (profile_top) rt.enable_profile(profile_top);

    if (dump) {
        dump_image(rt.image());
        return 0;
    }

    // Constructing a Jit registers it with the runtime, so `--no-jit` simply
    // never builds one and every call stays in the interpreter.
    std::unique_ptr<Jit> jit_owner;
    if (use_jit && Jit::available()) jit_owner = std::make_unique<Jit>(rt);
    if (jit_threshold && jit_owner) jit_owner->set_threshold(jit_threshold);

    if (!dump_jit_fn.empty()) {
        if (!Jit::available()) {
            std::fprintf(stderr, "dream: this build has no JIT\n");
            return 1;
        }
        if (!jit_owner) jit_owner = std::make_unique<Jit>(rt);
        for (uint32_t i = 0; i < rt.image().func_count(); ++i) {
            if (rt.image().str(rt.image().func(i).name).equals(dump_jit_fn.c_str())) {
                std::fputs(jit_owner->dump_ir(i).c_str(), stdout);
                return 0;
            }
        }
        std::fprintf(stderr, "dream: no function named `%s`\n", dump_jit_fn.c_str());
        return 1;
    }

    uint32_t func = NO_NODE;
    if (!entry.empty()) {
        int g = rt.image().find_global(entry.c_str());
        if (g < 0) {
            std::fprintf(stderr, "dream: no global named `%s`\n", entry.c_str());
            return 1;
        }
        const GlobalRec& gr = rt.image().global(uint32_t(g));
        if (gr.kind != GLOBAL_FUNCTION) {
            std::fprintf(stderr, "dream: `%s` is not a function\n", entry.c_str());
            return 1;
        }
        func = gr.target;
    } else {
        func = rt.image().entry();
        if (func == NO_NODE) {
            std::fprintf(stderr,
                         "dream: this image has no `main!`; pass --entry to name one\n");
            return 1;
        }
    }

    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 1;
    }

    Scheduler sched(rt, workers);
    auto root = sched.create_process();
    Value cl = root->heap().make_closure(func, 0);
    prime_apply(*root, cl, 0);

    sched.start();
    sched.enqueue(root);
    bool clean = sched.wait_for_all();
    sched.stop();

    int status = 0;
    if (!clean) {
        std::fprintf(stderr,
                     "dream: deadlock -- every process is waiting for a message that "
                     "cannot arrive\n");
        status = 1;
    } else if (root->failed) {
        std::string text;
        stringify(*root, root->exit_value, &text);
        std::fprintf(stderr, "dream: uncaught error: %s\n", text.c_str());
        status = 1;
    }

    for (const std::string& f : sched.take_failures()) {
        if (f.rfind("process " + std::to_string(root->id()) + ":", 0) == 0) continue;
        std::fprintf(stderr, "dream: uncaught error in %s\n", f.c_str());
        status = 1;
    }

    rt.print_profile();

    if (stats) {
        std::fprintf(stderr,
                     "; %" PRIu64 " reductions, %" PRIu64 " collections in the root process, "
                     "%u workers, jit %s\n",
                     sched.total_reductions(), root->heap().collections(), workers,
                     jit_owner ? "on" : "off");
        if (jit_owner) {
            std::fprintf(stderr, "; %" PRIu64 " functions compiled\n",
                         jit_owner->compiled_count());
        }
    }
    return status;
}
