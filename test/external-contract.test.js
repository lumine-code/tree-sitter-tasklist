const assert = require("node:assert");
const fs = require("node:fs");
const path = require("node:path");
const { test } = require("node:test");

test("keeps grammar externals aligned with scanner token IDs", () => {
  const grammar = fs.readFileSync(path.join(__dirname, "..", "grammar.js"), "utf8");
  const scanner = fs.readFileSync(path.join(__dirname, "..", "src", "scanner.c"), "utf8");
  const externalBlock = grammar.match(/externals:\s*\([^)]*\)\s*=>\s*\[([\s\S]*?)\],/)[1];
  const enumBlock = scanner.match(/enum TokenType\s*\{([\s\S]*?)\};/)[1];
  const grammarNames = [...externalBlock.matchAll(/\$\.(_?[a-z0-9_]+)/g)].map((match) =>
    match[1].replace(/^_/, "").toUpperCase(),
  );
  const scannerNames = [...enumBlock.matchAll(/^\s*([A-Z][A-Z0-9_]*),?\s*$/gm)].map(
    (match) => match[1],
  );

  assert.deepStrictEqual(grammarNames, scannerNames);
});
