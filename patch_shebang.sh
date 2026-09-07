
file="$1"
shebang='#!/usr/bin/env dream'

if [ ! -f "$file" ]; then
  echo "File not found: $file"
  exit 1
fi

if ! head -n 1 "$file" | grep -q '^#!'; then
  tmp=$(mktemp)
  printf '%s\n' "$shebang" > "$tmp"
  cat "$file" >> "$tmp"
  mv "$tmp" "$file"
fi
