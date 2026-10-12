#!/usr/bin/env python3
"""Live editor regressions: import discovery, escaped paths and cache freshness."""
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


def run(vm, image):
    with tempfile.TemporaryDirectory(prefix='lucid editor ') as tmp:
        root = Path(tmp)
        (root / 'mind.toml').write_text('[package]\nname = "app"\nsrc = "."\n')
        helper = root / 'helper.dr'
        helper.write_text('let double n = n * 2;\n')
        (root / 'nested').mkdir()
        (root / 'nested' / 'leaf.dr').write_text('let value = 1;\n')
        uri = (root / 'main.dr').as_uri()
        proc = subprocess.Popen([vm, image, '-L', str(Path('mind').resolve())],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0)
        count = 0

        def send(method, params, request=False):
            nonlocal count
            msg = dict(jsonrpc='2.0', method=method, params=params)
            if request:
                count += 1
                msg['id'] = count
            body = json.dumps(msg).encode()
            proc.stdin.write(f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)
            return count

        def receive(wanted=None, notification=None):
            deadline = time.monotonic() + 30
            while True:
                if not select.select([proc.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                    raise AssertionError('server response timed out')
                header = proc.stdout.readline()
                assert header, 'server exited unexpectedly'
                size = int(header.split(b':')[1])
                assert proc.stdout.readline() == b'\r\n'
                body = b''
                while len(body) < size:
                    body += proc.stdout.read(size - len(body))
                msg = json.loads(body)
                if wanted is not None and msg.get('id') == wanted:
                    assert 'error' not in msg, msg
                    return msg['result']
                if notification and msg.get('method') == notification:
                    return msg['params']

        def request(method, params):
            return receive(send(method, params, True))

        def change(text):
            send('textDocument/didChange', dict(textDocument=dict(uri=uri, version=count + 1),
                                               contentChanges=[dict(text=text)]))
            receive(notification='textDocument/publishDiagnostics')

        def complete(text):
            change(text)
            items = request('textDocument/completion', dict(textDocument=dict(uri=uri),
                            position=dict(line=0, character=len(text))))
            return {item['label'] for item in items}

        try:
            request('initialize', dict(rootUri=root.as_uri()))
            send('textDocument/didOpen', dict(textDocument=dict(uri=uri, languageId='dream',
                                                                version=1, text='')))
            receive(notification='textDocument/publishDiagnostics')
            assert {'list', 'http', 'vm'} <= complete('import std.')
            assert {'url', 'h2'} <= complete('import std.http.')
            assert {'helper', 'nested'} <= complete('import ')
            assert 'leaf' in complete('import nested.')
            assert 'helper' in complete('import app.')
            text = 'import helper;\nlet answer = helper.double 21;\n'
            change(text)
            params = dict(textDocument=dict(uri=uri), position=dict(line=1, character=22))
            definition = request('textDocument/definition', params)
            assert definition['uri'] == helper.as_uri(), definition
            # A closed dependency changes while the root revision stays put.
            helper.write_text('\n\nlet double n = n * 3;\n')
            updated = request('textDocument/definition', params)
            assert updated['range']['start']['line'] == 2, updated
            helper.unlink()
            assert request('textDocument/definition', params) is None
            helper.write_text('let double n = n * 4;\n')
            assert request('textDocument/definition', params)['uri'] == helper.as_uri()
            # Manifest changes must also invalidate a cached import binding.
            for name in ('first', 'second'):
                dep = root / name
                dep.mkdir()
                (dep / 'mod.dr').write_text('let double n = n;\n')
            manifest = root / 'mind.toml'
            def point_at(name):
                manifest.write_text('[package]\nname = "app"\nsrc = "."\n'
                                    f'[dependencies]\nlib = "{name}"\n')
            point_at('first')
            change('import lib;\nlet answer = lib.double 21;\n')
            params['position']['character'] = 19
            assert request('textDocument/definition', params)['uri'] == (root / 'first/mod.dr').as_uri()
            point_at('second')
            assert request('textDocument/definition', params)['uri'] == (root / 'second/mod.dr').as_uri()
            # Loose files need no manifest for sibling import completion.
            manifest.unlink()
            assert 'helper' in complete('import hel')
            request('shutdown', {})
            send('exit', {})
            assert proc.wait(timeout=10) == 0
            assert not proc.stderr.read()
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    print('editor imports, escaped local paths and cache freshness pass')


if __name__ == '__main__':
    run(sys.argv[1], sys.argv[2])
