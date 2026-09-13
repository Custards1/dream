// The VS Code side of `lucid`.
//
// There is deliberately almost nothing here. Everything an editor shows about
// a Dream program -- the diagnostics, where a name is defined, what it means,
// the outline -- is answered by the language server out of the compiler's own
// tables. This file's whole job is to find the server, start it, and get out
// of the way. Anything it computed itself would be a second opinion about a
// program the compiler has already read, and a second opinion is how an editor
// comes to disagree with the build.

const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient, State, TransportKind } = require('vscode-languageclient/node');

let client;

/// A path from a setting may be written for a human: `~` and `$HOME` mean what
/// they mean in a shell. The settings say `$MINDV2_PATH`, so a value that names
/// one of those variables literally should not be looked up verbatim. Iterated,
/// because `$MINDV2_PATH` might itself be written as `~/.mindv2`.
function expandPath(s) {
  if (!s) return s;
  for (let i = 0; i < 3; i++) {
    let changed = false;
    s = s.replace(/\$(\w+)|\$\{(\w+)\}/g, (m, bare, braced) => {
      const v = process.env[bare || braced];
      if (v === undefined) return m;
      changed = true;
      return v;
    });
    s = s.replace(/^~(?=\/|$)/, () => {
      changed = true;
      return os.homedir();
    });
    if (!changed) break;
  }
  return s;
}

/// Where the server image might be, in the order worth trying.
///
/// `lucid` is a compiled image rather than a native program, so this is looking
/// for a file to hand the VM, not for something on PATH.
function findServer(config, folders) {
  const configured = expandPath(config.get('server.path'));
  if (configured && fs.existsSync(configured)) return configured;
  const candidates = [];

  // A workspace that is (or contains a build of) the Dream checkout has the
  // freshest image -- the one the developer just built -- so it wins over the
  // installation.
  for (const folder of folders || []) {
    const root = folder.uri.fsPath;
    candidates.push(path.join(root, 'build', 'lucid.dream'));
    candidates.push(path.join(root, 'build-drain', 'bin', 'lucid.dream'));
  }

  if (process.env.LUCID_IMAGE) candidates.push(expandPath(process.env.LUCID_IMAGE));
  if (process.env.MINDV2_PATH) {
    candidates.push(path.join(expandPath(process.env.MINDV2_PATH), 'lucid.dream'));
    candidates.push(path.join(expandPath(process.env.MINDV2_PATH), 'lucid'));
  }

  // Ask the VM itself. A toolchain wrapper -- the nix flake's `dream` -- sets
  // `$MINDV2_PATH` only inside its own process, so an editor cannot read it
  // from its own environment, and the wrapper is where the installation is
  // guaranteed to be right. `dream --mindv2-path` prints it; a VM too old to
  // have the flag, or missing entirely, is caught and simply adds nothing.
  const vm = expandPath(config.get('vm.path')) || 'dream';
  try {
    const out = execFileSync(vm, ['--mindv2-path'], { encoding: 'utf8' });
    for (const dir of out.trim().split(path.delimiter)) {
      if (!dir) continue;
      const root = expandPath(dir);
      candidates.push(path.join(root, 'lucid.dream'));
      candidates.push(path.join(root, 'lucid'));
    }
  } catch (err) {
    // The other candidates still stand; the later spawn failure is a real
    // message instead of a silent nothing.
  }

  return candidates.find((c) => fs.existsSync(c));
}

/// The `-L` roots: every workspace folder, whatever the settings add, and the
/// standard library. A project that cannot see `std` reports every import of it
/// as an error, which looks like the extension is broken rather than unconfigured.
function packagePaths(config, folders) {
  const roots = [];
  for (const folder of folders || []) roots.push(folder.uri.fsPath);
  for (const p of config.get('packagePaths') || []) roots.push(p);

  const stdlib = config.get('stdlib.path') || process.env.MIND_STDLIB;
  if (stdlib) roots.push(stdlib);

  return roots;
}

function activate(context) {
  const config = vscode.workspace.getConfiguration('dream');
  const folders = vscode.workspace.workspaceFolders;

  const server = findServer(config, folders);
  if (!server) {
    vscode.window.showWarningMessage(
      'Dream: no language server image found. Build one with `just lucid`, ' +
        'install it with `just install` (putting `~/.mindv2` on your ' +
        '$MINDV2_PATH), or set `dream.server.path` to a `lucid.dream`.',
      'Settings'
    ).then((pick) => {
      if (pick === 'Settings') vscode.commands.executeCommand('workbench.action.openSettings', '@ext:dream.dream-lang');
    });
    return;
  }

  // The VM has to be found too, and not finding it is the same failure from
  // the user's side: an editor that shows nothing.
  const vm = expandPath(config.get('vm.path')) || 'dream';
  const args = [server];
  for (const root of packagePaths(config, folders)) args.push('-L', root);

  // stdio, because that is what the server speaks: it reads framed JSON-RPC
  // from its standard input and writes it to its standard output.
  const run = { command: vm, args, transport: TransportKind.stdio };

  client = new LanguageClient(
    'dream',
    'Dream Language Server',
    { run, debug: run },
    {
      documentSelector: [{ scheme: 'file', language: 'dream' }],
      synchronize: {
        // A manifest decides which packages a file can see, so a build that
        // changes it changes what every open file means.
        fileEvents: vscode.workspace.createFileSystemWatcher('**/mind.toml'),
      },
    }
  );

  // A server that dies before it ever ran -- the VM was not on PATH, the image
  // was refused, the process crashed on startup -- is indistinguishable from no
  // extension at all unless it says so. The client's start promise rejects on a
  // failed spawn; a process that starts and dies immediately shows up as never
  // reaching `Running`.
  let everRan = false;
  client.onDidChangeState((e) => {
    if (e.newState === State.Running) everRan = true;
    else if (e.newState === State.Stopped && !everRan) {
      vscode.window.showErrorMessage(
        'Dream: the language server exited before starting. Open the "Dream Language Server" output channel and check that `dream.vm.path` names the VM and the image is a build of `lucid`.'
      );
    }
  });

  context.subscriptions.push(
    vscode.commands.registerCommand('dream.restartServer', async () => {
      if (client) await client.restart();
    })
  );

  client.start().catch((err) => {
    vscode.window.showErrorMessage(
      'Dream: could not start the language server: ' + err + '. Check `dream.vm.path` and that `' + vm + '` runs the Dream VM.'
    );
  });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
