#!/usr/bin/env python3
"""Navigation must answer while the compiler checks Lucid's real import graph."""
import json
from pathlib import Path
import select
import subprocess
import sys
import time


def run(vm, image):
    root = Path(__file__).resolve().parents[2]
    source = root / 'lucid' / 'main.dr'
    text = source.read_text() + '\nlet navigation_probe a = analysis.inlay_hints a 0 5;\n'
    line = len(text.splitlines()) - 1
    character = text.splitlines()[line].index('inlay_hints') + 4
    uri = source.as_uri()
    target = root / 'lucid' / 'analysis.dr'
    target_line = next(i for i, s in enumerate(target.read_text().splitlines())
                       if s.startswith('let inlay_hints a'))
    process = subprocess.Popen([vm, image, '-L', str(root / 'mind')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)
    count = 0
    replies = {}
    order = []

    def send(method, params, request=False):
        nonlocal count
        message = dict(jsonrpc='2.0', method=method, params=params)
        if request:
            count += 1
            message['id'] = count
        body = json.dumps(message).encode()
        process.stdin.write(f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)
        return count

    def receive(wanted):
        deadline = time.monotonic() + 20
        while wanted not in replies:
            assert select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))[0], 'response timeout'
            header = process.stdout.readline()
            assert header, 'server exited'
            assert process.stdout.readline() == b'\r\n'
            size = int(header.split(b':')[1])
            body = b''
            while len(body) < size:
                chunk = process.stdout.read(size - len(body))
                assert chunk, 'truncated response'
                body += chunk
            message = json.loads(body)
            if 'id' in message:
                replies[message['id']] = message
                order.append(message['id'])
            elif message.get('method') == 'window/logMessage':
                assert message['params']['type'] != 1, message
        return replies.pop(wanted)

    params = dict(textDocument=dict(uri=uri), position=dict(line=line, character=character))

    def definition():
        started = time.monotonic()
        result = receive(send('textDocument/definition', params, True))
        elapsed = time.monotonic() - started
        assert result.get('result', {}).get('uri') == target.as_uri(), result
        assert result['result']['range']['start']['line'] == target_line, result
        return elapsed

    try:
        receive(send('initialize', dict(rootUri=source.parent.as_uri()), True))
        send('textDocument/didOpen', dict(textDocument=dict(uri=uri, languageId='dream', version=1, text=text)))
        hints = send('textDocument/inlayHint', dict(textDocument=dict(uri=uri),
                     range=dict(start=dict(line=0, character=0), end=dict(line=line + 1, character=0))), True)
        cold = definition()
        assert cold < 2, f'cold navigation blocked for {cold:.3f}s'
        assert hints not in order, 'navigation waited for whole-program hints'
        warm = definition()
        assert warm < .25, f'cached navigation took {warm:.3f}s'
        hovered = receive(send('textDocument/hover', params, True))
        assert 'The hints for this file between two lines' in hovered['result']['contents']['value'], hovered
        send('$/cancelRequest', dict(id=hints))
        assert receive(hints)['error']['code'] == -32800
        for revision in range(2, 12):
            send('textDocument/didChange', dict(textDocument=dict(uri=uri, version=revision),
                                               contentChanges=[dict(text=text + f'// edit {revision}\n')]))
        edited = definition()
        assert edited < 2, f'edit burst blocked navigation for {edited:.3f}s'
        receive(send('shutdown', {}, True))
        send('exit', {})
        assert process.wait(timeout=10) == 0
        assert not process.stderr.read(), 'server wrote errors'
        print(f'Lucid source navigation: cold {cold * 1000:.0f} ms, cached {warm * 1000:.0f} ms, edits {edited * 1000:.0f} ms')
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


if __name__ == '__main__':
    run(*sys.argv[1:])
