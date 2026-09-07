// The grammar, tokenized for real.
//
// A TextMate grammar is a pile of regular expressions with no type checker and
// no compiler; the usual way to find out it is wrong is to look at colours in
// an editor and squint. So it is loaded here the way VS Code loads it, run over
// Dream that exercises each rule, and checked for the scope that should have
// won at a given character.
const fs = require('fs');
const path = require('path');
const oniguruma = require('vscode-oniguruma');
const textmate = require('vscode-textmate');

const wasm = fs.readFileSync(
  path.join(__dirname, '..', 'node_modules', 'vscode-oniguruma', 'release', 'onig.wasm')
);

const onigLib = oniguruma.loadWASM(wasm.buffer).then(() => ({
  createOnigScanner: (s) => new oniguruma.OnigScanner(s),
  createOnigString: (s) => new oniguruma.OnigString(s),
}));

const registry = new textmate.Registry({
  onigLib,
  loadGrammar: () =>
    Promise.resolve(
      textmate.parseRawGrammar(
        fs.readFileSync(path.join(__dirname, '..', 'syntaxes', 'dream.tmLanguage.json'), 'utf8'),
        'dream.tmLanguage.json'
      )
    ),
});

// [line, the text to find in it, the scope that must apply to it]
const cases = [
  ['/// A doc comment.', '///', 'comment.block.documentation.dream'],
  ['// An ordinary one.', '//', 'comment.line.double-slash.dream'],
  ['let greeting = "hi";', 'greeting', 'entity.name.function.dream'],
  ['let greeting = "hi";', '"', 'string.quoted.double.dream'],
  ['let s = "a\\u{1F600}b";', '\\u{1F600}', 'constant.character.escape.dream'],
  ['let a = :ok;', ':ok', 'constant.other.symbol.dream'],
  ['let n = 0xFF;', '0xFF', 'constant.numeric.hex.dream'],
  ['let f = 1.5;', '1.5', 'constant.numeric.float.dream'],
  ['let i = 42;', '42', 'constant.numeric.integer.dream'],
  ['if x { 1 } else { 2 }', 'if', 'keyword.control.dream'],
  ['match x { _ => 1 }', 'match', 'keyword.control.dream'],
  ['let f = fn x -> x;', 'fn', 'keyword.declaration.dream'],
  ['import std.list;', 'import', 'keyword.declaration.dream'],
  ['let t = true;', 'true', 'constant.language.boolean.dream'],
  ['let p = spawn! t;', 'spawn!', 'support.function.builtin.dream'],
  ['console.print! x', 'print!', 'entity.name.function.impure.dream'],
  ['let r = a |> f;', '|>', 'keyword.operator.pipe.dream'],
  ['fn x -> x', '->', 'keyword.operator.arrow.dream'],
  ['let t = $( 1 );', '$(', 'punctuation.definition.thunk.dream'],
  ['let m = %{ :a => 1 };', '%{', 'punctuation.definition.map.dream'],
  ['let a = #[1, 2];', '#[', 'punctuation.definition.array.dream'],
  ["let c = 'x';", "'x'", 'string.quoted.single.dream'],
  ['let r = try! { 1 } catch e { 2 };', 'try!', 'keyword.control.dream'],
  ['let v = comp! f;', 'comp!', 'keyword.control.dream'],
  ['let x = comp f;', 'comp', 'keyword.declaration.dream'],
  ['let a = 5 % 2;', '%', 'keyword.operator.arithmetic.dream'],
  ['let rec go n = n;', 'go', 'entity.name.function.dream'],
];

registry.loadGrammar('source.dream').then((grammar) => {
  let failed = 0;
  let ruleState = textmate.INITIAL;
  for (const [line, needle, want] of cases) {
    const result = grammar.tokenizeLine(line, textmate.INITIAL);
    const column = line.indexOf(needle);
    if (column < 0) {
      console.log(`FAIL  ${JSON.stringify(needle)} is not in ${JSON.stringify(line)}`);
      failed++;
      continue;
    }
    const token = result.tokens.find((t) => t.startIndex <= column && column < t.endIndex);
    const scopes = token ? token.scopes : [];
    if (!scopes.includes(want)) {
      console.log(`FAIL  ${JSON.stringify(needle)} in ${JSON.stringify(line)}`);
      console.log(`        want ${want}`);
      console.log(`        got  ${scopes.join(' ')}`);
      failed++;
    }
  }
  const total = cases.length;
  if (failed === 0) console.log(`${total} scopes are what the grammar promises`);
  else console.log(`${total - failed} passed, ${failed} FAILED`);
  process.exit(failed === 0 ? 0 : 1);
});
