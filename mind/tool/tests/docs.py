#!/usr/bin/env python3
"""Build real docs: comments, examples, Markdown, offline assets and diagnostics."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
VM = str(Path(os.environ.get('dream', 'build-dream/bin/dream')).resolve())
MIND = str(Path(os.environ.get('mind', '/tmp/dream-mind-graph.dream')).resolve())
COMPILER = str(Path(os.environ.get('dreams', 'build/dreams.dream')).resolve())
ENV = dict(os.environ, DREAM=VM, DREAMS=COMPILER, MIND_UNITS='off', MIND_STDLIB=str(ROOT / 'mind'))


def invoke(*args, cwd=ROOT, ok=True):
    p = subprocess.run([VM, MIND, *args], cwd=cwd, env=ENV, text=True, capture_output=True, timeout=120)
    assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
    return p.stdout + p.stderr


with tempfile.TemporaryDirectory(prefix='mind-doc-tests-') as scratch:
    project = Path(scratch)
    (project / 'src').mkdir()
    (project / 'docs/nested').mkdir(parents=True)
    (project / 'mind.toml').write_text('[package]\nname = "demo"\nversion = "1.2.3"\n[docs]\ntitle = "Demo reference"\n')
    (project / 'src/lib.dr').write_text('''/// Return a greeting.
///
/// ```dream
/// import demo.lib;
/// import std.console;
/// let main! = console.print! (lib.hello "reader");
/// ```
let hello : :string -> :string;
let hello name = "hello " + name;

/// A compile error is useful documentation too.
/// ```dream fails
/// let main! = unknown_name;
/// ```
let broken = 1;

/// Runtime failures are captured.
/// ```dream fails
/// import std.console;
/// let main! = { console.error! "before failure"; raise! "intentional" };
/// ```
let failure = 2;

/// Deadlines do not stop the build.
/// ```dream fails
/// let rec forever! () = forever! ();
/// let main! = forever! ();
/// ```
let timeout = 3;

/// This helper should not appear in API docs.
priv let secret x = x;

let literal = "
/// This is string content, not documentation.
let fake = 4;
";

when test {
    /// This is local test documentation.
    let hidden_test x = x;
}
''')
    (project / 'docs/README.md').write_text('''# Start here

[Next page](nested/next.md#repeated)

```dream
import std.console;
let main! = console.print! "guide output";
```

<script>alert("unsafe")</script>

[Unsafe](javascript:alert)

| Name | Value |
| --- | --- |
| one | **two** |
''')
    (project / 'docs/nested/next.md').write_text('# Repeated\n\n## Repeated\n\n## Repeated\n\n- First\n- Second\n')
    (project / 'docs/sample.txt').write_text('downloadable asset')
    assert 'mind pack' in invoke('help', 'pack')
    assert 'example-timeout' in invoke('help', 'docs')
    # Discovery from a subdirectory and a global compiler option must work.
    # The deadline bounds each example's compile as well as its run, so it
    # has to leave room for a compile; the endless example still runs out.
    out = invoke('--compiler', COMPILER, 'docs', '--example-timeout', '2000', '--strict-examples', cwd=project / 'src')
    assert 'did not do what' not in out, out
    target = project / 'target/docs'
    assert (target / 'index.html').exists(), out
    api = (target / 'api/demo/lib.html').read_text()
    assert 'Run · exit 0' in api and 'hello reader' in api
    assert 'Compile · exit 1' in api and 'unknown_name' in api
    assert 'before failure' in api and 'intentional' in api
    assert 'Run timed out' in api
    assert 'id="secret"' not in api and 'id="fake"' not in api and 'id="hidden_test"' not in api
    assert 'source/demo/lib.html#L' in api
    guide = (target / 'guides/README.html').read_text()
    assert 'guide output' in guide and 'Run · exit 0' in guide
    assert 'nested/next.html#repeated' in guide
    assert '<script>alert' not in guide and '&lt;script&gt;' in guide
    assert 'href="javascript:' not in guide
    assert '<table>' in guide
    nested = (target / 'guides/nested/next.html').read_text()
    assert 'id="repeated-1"' in nested and 'id="repeated-2"' in nested
    assert '../../assets/docs.css' in nested
    assert (target / 'guides/sample.txt').read_text() == 'downloadable asset'
    entries = json.loads((target / 'search-index.json').read_text())
    assert any('hello' in p['text'] for p in entries)
    for asset in ['docs.css', 'docs.js', 'search.js']:
        assert (target / 'assets' / asset).stat().st_size > 0
    # An example that fails without saying so is reported, and refused under
    # --strict-examples; so is one marked `fails` that works.
    lib = project / 'src/lib.dr'
    lib.write_text(lib.read_text().replace('/// ```dream fails\n/// let main! = unknown_name;', '/// ```dream\n/// let main! = unknown_name;')
                   .replace('/// ```dream\n/// import demo.lib;', '/// ```dream fails\n/// import demo.lib;'))
    out = invoke('--compiler', COMPILER, 'docs', '-C', str(project), '--example-timeout', '2000')
    assert '2 example(s) did not do what' in out and 'warning' in out, out
    assert 'api/demo/lib.html: let main! = unknown_name; (failed)' in out, out
    assert 'marked `fails` but ran cleanly' in out, out
    out = invoke('--compiler', COMPILER, 'docs', '-C', str(project), '--example-timeout', '2000', '--strict-examples', ok=False)
    assert 'did not do what' in out, out
    invoke('docs', '-C', str(project), '-o', str(project), ok=False)
    invoke('docs', '-C', str(project), '--example-timeout', '0', ok=False)
    invoke('pack', '-C', str(project), 'init')
    assert '[ship]' in (project / 'mind.toml').read_text()
    invoke('pack', 'files', 'unexpected', ok=False)
    # Every example in the standard library does what its fence says.
    out = invoke('--compiler', COMPILER, 'docs', '-C', str(ROOT / 'mind/std'), '-o', str(project / 'std-docs'),
                 '--strict-examples')
    assert 'did not do what' not in out, out
    print('Documentation: comments, examples, failures, timeouts, Markdown, offline assets and std examples passed')
