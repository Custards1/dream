#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
python3 "$root/mind/tool/tests/doc_coverage.py"
python3 "$root/mind/tool/tests/docs.py"
