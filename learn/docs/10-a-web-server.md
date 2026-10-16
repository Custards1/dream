# A small web server

This chapter puts the book together: functions, records, results and
processes, as an HTTP service. It uses `std.http.server`, and each program
here starts a real server, sends it real requests, and stops it, so what you
see printed came over a socket.

## A handler is a function

To `std.http.server`, a web application is a function from a request to a
response. A **router** is a table of them, by method and path:

```dream
import std.console;
import std.http;
import std.http.server;

let hello req = http.text "hello, world";
let greet req = match http.param "name" req {
    () => http.respond 400 "whom should I greet?",
    name => http.text ("hello, " + name),
};

let app = server.router
    |> server.get "/" hello
    |> server.get "/hello/:name" greet;

let main! = {
    let s = server.serve! 0 app;
    let base = server.local_url s;
    console.print! (to_string (http.get_text! (base + "/")))
    console.print! (to_string (http.get_text! (base + "/hello/Ada")))
    server.stop! s
};
```

Port `0` asks the system for any free port, and `server.local_url` says which
it got. `http.param` answers `()` when the route has no such parameter, and
its signature says so, so the compiler insists `greet` handle it: past the
`()` arm, `name` is known to be a string. The handlers are pure: they take a request and answer a response.
The server does the waiting, the reading and the writing, one process per
connection, so a slow client holds up nobody else.

## JSON, and answers that can fail

Return a map and the server answers it as JSON. A handler that cannot find
what it was asked for answers a 404, the same way it answers anything else:

```dream
import std.console;
import std.http;
import std.http.server;

let books = %{
    "1" => %{ "title" => "Structure and Interpretation", "year" => 1985 },
    "2" => %{ "title" => "The Little Schemer", "year" => 1974 },
};

let book req = match books.[http.param "id" req else ()] {
    () => http.respond 404 "no such book",
    found => http.json found,
};

let app = server.router |> server.get "/books/:id" book;

let main! = {
    let s = server.serve! 0 app;
    let ask! path = match http.get! (server.local_url s + path) {
        Result.ok(resp) => to_string (http.status resp) + " " + http.body resp,
        Result.error(e) => "failed: " + to_string e,
    };
    console.print! (ask! "/books/2")
    console.print! (ask! "/books/9")
    server.stop! s
};
```

`http.get!` answers a `Result`. A request can fail for reasons that are not
the server's answer, such as a refused connection or a timeout, and the
caller is made to say what happens then.

## State lives in a process

A pure handler cannot remember anything between requests, and should not:
any number of them run at once. State belongs to a process, and handlers
send it messages. `std.agent` is such a process, holding one value:

```dream
import std.agent;
import std.console;
import std.http;
import std.http.server;

let main! = {
    let hits = agent.start! 0;
    let count! req = {
        let n = agent.get_and_update! hits (fn n -> [n + 1, n + 1]);
        http.text ("visit number " + to_string n)
    };
    let s = server.serve! 0 (server.router |> server.get "/" count!);
    let base = server.local_url s;
    http.get_text! base
    http.get_text! base
    console.print! (to_string (http.get_text! base))
    server.stop! s
    agent.stop! hits
};
```

`get_and_update!` counts and reads in one step, inside the agent. Two calls,
an `update!` and then a read, would let two requests arriving together both
read the same number. A handler with effects is named with a `!`, as
everything with effects is. A process that is no longer needed is stopped:
a program whose processes are all left waiting for messages that cannot come
is reported as deadlocked.

## Serving files

`server.files! dir` is a handler that serves a directory. It answers only
`GET` and `HEAD`, refuses any path that would leave the directory, never
serves dotfiles or follows a symbolic link, and sends large files a piece at
a time. `mind serve` is that handler with sensible limits:

```sh
mind serve public          # http://127.0.0.1:8000/, this machine only
```

Next: [Where to go from here](11-where-next.md).
