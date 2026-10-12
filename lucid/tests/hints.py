#!/usr/bin/env python3
"""Cold inlay requests must return real compiler hints within one second."""
import json
from pathlib import Path
import select
import subprocess
import sys
import time


def check(vm, image, relative, marker, size):
    root = Path(__file__).resolve().parents[2]
    source = root / relative
    text = source.read_text()
    first = 0 if size is None else next(i for i, line in enumerate(text.splitlines()) if line.startswith(marker))
    if size is None:
        first = 0
        # A signed stdlib call must retain its useful type even when the
        # surrounding buffer imports the whole compiler.
        text += '\nimport std.num as hint_num;\nlet hint_probe x = {\n    let parsed = hint_num.parse_int x;\n    parsed\n};\n'
        size = len(text.splitlines())
    process = subprocess.Popen([vm, image, '-L', str(root / 'mind'), '-L', str(root)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)

    def send(method, params, request_id=None):
        message = dict(jsonrpc='2.0', method=method, params=params)
        if request_id is not None:
            message['id'] = request_id
        body = json.dumps(message).encode()
        process.stdin.write(f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)

    def receive(request_id):
        deadline = time.monotonic() + 20
        while True:
            assert select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))[0], 'hint response timed out'
            header = process.stdout.readline()
            assert header, 'server exited before returning hints'
            assert process.stdout.readline() == b'\r\n'
            length = int(header.split(b':')[1])
            body = b''
            while len(body) < length:
                chunk = process.stdout.read(length - len(body))
                assert chunk, 'truncated hint response'
                body += chunk
            message = json.loads(body)
            if message.get('id') == request_id:
                return message
            if message.get('method') == 'window/logMessage':
                assert message['params']['type'] != 1, message

    try:
        send('initialize', dict(rootUri=source.parent.as_uri()), 1)
        receive(1)
        send('textDocument/didOpen', dict(textDocument=dict(uri=source.as_uri(), version=1,
                                                           languageId='dream', text=text)))
        elapsed = []
        for request_id in (2, 3):
            started = time.monotonic()
            send('textDocument/inlayHint', dict(textDocument=dict(uri=source.as_uri()),
                 range=dict(start=dict(line=first, character=0), end=dict(line=first + size, character=0))), request_id)
            reply = receive(request_id)
            duration = time.monotonic() - started
            assert 'error' not in reply, reply
            hints = reply['result']
            assert hints, f'{relative}: returned no hints'
            assert all(first <= hint['position']['line'] <= first + size for hint in hints), hints
            if marker == 'full':
                probe_line = next(i for i, line in enumerate(text.splitlines()) if 'let parsed = hint_num' in line)
                labels = [hint['label'] for hint in hints if hint['position']['line'] == probe_line]
                assert any(':integer' in label and ':unit' in label for label in labels), labels
            assert duration < 1, f'{relative}: hints took {duration:.3f}s (target <1s)'
            elapsed.append(round(duration * 1000))
        send('shutdown', {}, 4)
        receive(4)
        send('exit', {})
        assert process.wait(timeout=10) == 0
        assert not process.stderr.read(), 'server wrote errors'
        print(f'{relative} ({size} lines): hints cold {elapsed[0]} ms, repeated {elapsed[1]} ms')
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


if __name__ == '__main__':
    vm, image = sys.argv[1:]
    for relative, marker, size in [
        ('lucid/main.dr', 'let handle!', 25),
        ('lucid/main.dr', 'full', None),
        ('lucid/analysis.dr', 'let inlay_hints a', 25),
        ('dreams/typecheck.dr', 'let hints l r', 25),
    ]:
        check(vm, image, relative, marker, size)
