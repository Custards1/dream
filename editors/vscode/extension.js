// The VS Code side of `lucid`.
//
// There is deliberately almost nothing here. Everything an editor shows about
// a Dream program -- the diagnostics, where a name is defined, what it means,
// the outline -- is answered by the language server out of the compiler's own
// tables. This file's whole job is to find the server, start it, and get out
// of the way. Anything it computed itself would be a second opinion about a
// program the compiler has already read, and a second opinion is how an editor
// comes to disagree with the build.

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

/// Where the server image might be, in the order worth trying.
///
/// `lucid` is a compiled image rather than a native program, so this is looking
/// for a file to hand the VM, not for something on PATH.
function findServer(config, folders) {
  const configured = config.get('server.path');
  if (configured) return configured;
const candidates = [];
  if (process.env.MINDV2_PATH) candidates.push(path.join(process.env.MINDV2_PATH, 'lucid.dream'));
if (process.env.MINDV2_PATH) candidates.push(path.join(process.env.MINDV2_PATH, 'lucid'));

  if (process.env.LUCID_IMAGE) candidates.push(process.env.LUCID_IMAGE);
  
  for (const folder of folders || []) {
    const root = folder.uri.fsPath;
    candidates.push(path.join(root, 'build', 'lucid.dream'));
    candidates.push(path.join(root, 'build-dream', 'bin', 'lucid.dream'));
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
        'or set `dream.server.path` to a `lucid.dream`.'
    );
    return;
  }

  const vm = config.get('vm.path') || 'dream';
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

  context.subscriptions.push(
    vscode.commands.registerCommand('dream.restartServer', async () => {
      if (client) await client.restart();
    })
  );

  client.start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
