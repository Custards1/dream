#!/usr/bin/env python3
"""Drive a recorded LSP conversation, waiting for asynchronous replies."""
import json
import select
from pathlib import Path
import subprocess
import sys

vm, image, input_path, output_path, error_path = sys.argv[1:]
with open(error_path, 'wb') as errors, open(output_path, 'wb') as output:
    process = subprocess.Popen([vm, image], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=errors, bufsize=0)
    def receive():
        assert select.select([process.stdout], [], [], 20)[0], 'server did not reply within 20 seconds'
        header = process.stdout.readline()
        if not header:
            raise AssertionError('server ended before replying')
        blank = process.stdout.readline()
        size = int(header.split(b':')[1])
        body = b''
        while len(body) < size:
            chunk = process.stdout.read(size - len(body))
            assert chunk, 'truncated response'
            body += chunk
        output.write(header + blank + body)
        output.flush()
        return json.loads(body)

    try:
        with open(input_path, 'rb') as messages:
            while header := messages.readline():
                blank = messages.readline()
                body = messages.read(int(header.split(b':')[1]))
                message = json.loads(body)
                process.stdin.write(header + blank + body)
                if 'id' in message:
                    while receive().get('id') != message['id']:
                        pass
                elif message.get('method') in ('textDocument/didOpen', 'textDocument/didChange'):
                    uri = message['params']['textDocument']['uri']
                    while True:
                        reply = receive()
                        if reply.get('method') == 'textDocument/publishDiagnostics' and reply['params']['uri'] == uri:
                            break
        assert process.wait(timeout=10) == 0
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
