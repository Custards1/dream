# Console

`std.console` is implemented in Dream. `std.io` supplies the handles and system
calls; rendering, formatting, prompts, colors and logging live in the library.

```dream
import std.console;

let main! = {
    console.printf! "Hello, {}!" ["world"]
    match console.prompt! "Your name? " {
        () => console.error! "Input ended",
        name => console.printf! "Welcome, {}" [name],
    }
};
```

## Output and formatting

`print! value` writes a newline on stdout; `write! value` omits the newline.
`line!` aliases `print!`. `error!` and `error_write!` do the same on stderr.
Each accepts any value, uses `to_string`, and returns `()`. Strings remain
unquoted. `text ["count=", 42]` concatenates multiple rendered pieces.

`print_to! handle value` and `write_to! handle value` use an explicit `std.io`
handle. They finish short writes, propagate I/O errors, and never close handles.
There is no user-space output buffer to flush. A zero-progress write raises
`:io_error` rather than looping forever. Each message is assembled before its
first write; large messages can still interleave if the OS requires short writes.

```dream
console.printf! "{}: {} items" ["queue", 12]  // newline
console.writef! "progress: {}%" [75]           // no newline
console.errorf! "cannot open {}" [path]
```

`{}` consumes one value, `{{` writes `{`, and `}}` writes `}`. Values are never
interpreted as format strings. Missing values, extra values and unmatched braces
are errors. `format template values` is pure and returns `[:ok, text]` or
`[:error, explanation]`; `format!` returns the text or raises `:bad_argument`.
The output helpers use `format!`, so a malformed template writes no partial text.

## Input and prompts

`read_line! ()` returns a line without LF/CRLF, `""` for an empty line, and `()`
at EOF. An unterminated final line is returned once before EOF. Whitespace is
preserved. `prompt! question` first writes the question to stderr, then reads
stdin. This leaves stdout available for redirected program results.

`confirm! question default` accepts `y`, `yes`, `n`, and `no`, ignoring case and
surrounding whitespace. An empty line chooses the Boolean default. Invalid input
prints help and retries. EOF returns `()` even when the default is `true`.

```dream
match console.confirm! "Overwrite?" false {
    true => save! (),
    false => (),
    () => console.error! "Input ended",
}
```

`prompt_int! question` retries until input is an integer or reaches EOF.
`ask! question parser` handles custom validation: the pure parser returns
`[:ok, value]` or `[:error, help]`. For example:

```dream
let port line = match console.integer line {
    [:ok, n] if n >= 1 && n <= 65535 => [:ok, n],
    _ => [:error, "Enter a port from 1 to 65535."],
};
let chosen = console.ask! "Port? " port;
```

For tests, files, or alternate streams, construct a session:

```dream
let session = console.Session.new input_handle output_handle
              |> console.Session.set_max_line 4096;
let name = console.prompt_with! session "Name? ";
let yes = console.confirm_with! session "Continue?" false;
let port = console.ask_with! session "Port? " port;
```

`session! ()` creates the default stdin/stderr session. The default line limit is
1 MiB, counting bytes before LF (including a CR in CRLF). A nonpositive limit
raises `:bad_argument`; an oversized line raises `:line_too_long` and leaves the
remaining input unread. A call reads exactly through its newline, never consuming
the next line. `read_line_from! limit handle` exposes that reader directly.
A session should have one reader at a time. Handles remain the caller's property.

## Colors

Styles are `:bold`, `:dim`, `:italic`, `:underline`, and the eight foreground
colors `:black`, `:red`, `:green`, `:yellow`, `:blue`, `:magenta`, `:cyan`, `:white`.

```dream
console.styled! [:bold, :green] "Build succeeded"
```

`styled! styles value` prints a line using automatic color. `paint! mode handle
styles value` returns styled text for a particular destination. The modes are:

- `:auto`: color only on an ANSI-capable terminal, respecting nonempty `NO_COLOR`
  and `TERM=dumb`. Files and pipes receive plain text.
- `:always`: emit ANSI regardless of the destination or environment.
- `:off`: return plain text.

`style styles value` is the pure, explicit ANSI constructor. It resets styles at
the end and restores an outer style after a nested style's reset. It does not
sanitize arbitrary terminal control sequences in input. `color_enabled! mode
handle` answers the destination policy. The native `io.is_terminal! handle`
checks terminal status; `io.enable_ansi! handle` enables Windows virtual-terminal
output when needed and reports whether ANSI output is supported.

## Logging

`info! value`, `warn! value`, and `log! level value` print labeled messages to
stderr. Levels are `:debug`, `:info`, `:warn`, `:error`; `:off` disables output.
`logf! level template values` formats a log message. The default minimum is
`:info`, with automatic color.

```dream
let log = console.logger! ()
          |> console.Logger.set_minimum :debug
          |> console.Logger.set_prefix "worker"
          |> console.Logger.set_color :off;
console.log_with! log :debug "starting"
console.log_with! log :error "request failed"
```

`Logger.new handle` routes logging to another stream. `Logger.set_output`
changes an existing logger. Configuration is an ordinary immutable value,
so concurrent tasks can use different levels and destinations. Suppressed
messages are not forced. Every line of a multiline message gets its level and
prefix. The preserved `error!` function is raw stderr output; use `log! :error`
when a labeled error is wanted. Logging failures propagate like other writes.
