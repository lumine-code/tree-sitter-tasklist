const assert = require("node:assert");
const { test } = require("node:test");
const Parser = require("tree-sitter");
const Tasklist = require("..");

function parser() {
  const instance = new Parser();
  instance.setLanguage(Tasklist);
  return instance;
}

function parse(source) {
  return parser().parse(source);
}

test("parses every line form without errors", () => {
  const tree = parse(
    "# Chapter\nHeader:\n▷ urgent\n☐ pending\n✔ finished\n✘ rejected\n• note\nplain\n",
  );
  assert.strictEqual(tree.rootNode.hasError, false);
  assert.strictEqual(tree.rootNode.descendantsOfType("chapter").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("header").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("task").length, 4);
  assert.strictEqual(tree.rootNode.descendantsOfType("note").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("text_line").length, 1);
});

test("preserves TextMate line precedence and edge cases", () => {
  const tree = parse("☐ task:\n• note:\n# Chapter:\n :\n###   \nHeader:\t\n");
  assert.strictEqual(tree.rootNode.hasError, false);
  assert.deepStrictEqual(
    tree.rootNode.descendantsOfType("line").map((node) => node.namedChild(0).type),
    ["task", "note", "chapter", "header", "chapter", "text_line"],
  );
});

test("keeps indentation, separators, and trailing spaces outside task content", () => {
  const tree = parse("  ☐ todo  \n☐\tTODO\t  \n");
  assert.strictEqual(tree.rootNode.hasError, false);
  const tasks = tree.rootNode.descendantsOfType("task");
  assert.deepStrictEqual(
    tasks.map((node) => [node.startIndex, node.endIndex]),
    [
      [2, 8],
      [11, 18],
    ],
  );
  assert.deepStrictEqual(
    tree.rootNode.descendantsOfType("inline").map((node) => node.text),
    ["todo", "\tTODO\t"],
  );
});

test("matches opaque formats and Unicode word boundaries", () => {
  const source = [
    "~~ ** __ $$ ``",
    "****",
    "*****",
    "******",
    "*a*b*",
    "é*x* α*x* 漢*x* é*x*",
    "😀*x*😀 。*x*。",
  ].join("\n");
  const tree = parse(source);
  assert.strictEqual(tree.rootNode.hasError, false);
  assert.deepStrictEqual(
    tree.rootNode.descendantsOfType("bold").map((node) => node.text),
    ["**", "****", "****", "****", "**", "*a*b*", "*x*", "*x*"],
  );
  assert.strictEqual(tree.rootNode.descendantsOfType("strikethrough").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("italic").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("math").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("raw").length, 1);
});

test("keeps done and failed contents opaque", () => {
  const tree = parse("✔ *bold* TODO https://example.com\n✘ _italic_ FIXME\n");
  assert.strictEqual(tree.rootNode.hasError, false);
  assert.strictEqual(tree.rootNode.descendantsOfType("bold").length, 0);
  assert.strictEqual(tree.rootNode.descendantsOfType("italic").length, 0);
  assert.deepStrictEqual(
    tree.rootNode.descendantsOfType("opaque_text").map((node) => node.text),
    ["*bold* TODO https://example.com", "_italic_ FIXME"],
  );
});

test("builds visual layout groups and excludes trailing blank lines", () => {
  const tree = parse("Parent\n  child\n    grandchild\nSibling\n  child\n\n\nAfter\n");
  assert.strictEqual(tree.rootNode.hasError, false);
  const groups = tree.rootNode.descendantsOfType("layout_group");
  assert.deepStrictEqual(
    groups.map((node) => [node.startPosition.row, node.endPosition.row]),
    [
      [0, 2],
      [1, 2],
      [3, 4],
    ],
  );
});

test("parses CRLF and a final line without a newline", () => {
  const tree = parse("Header:\r\n\t☐ child\r\nFinal");
  assert.strictEqual(tree.rootNode.hasError, false);
  assert.strictEqual(tree.rootNode.descendantsOfType("layout_group").length, 1);
  assert.strictEqual(tree.rootNode.descendantsOfType("line").length, 3);
});

test("restores layout scanner state during an incremental reparse", () => {
  const instance = parser();
  const source = "Parent\nchild\nAfter\n";
  const changed = "Parent\n  child\nAfter\n";
  const tree = instance.parse(source);
  const index = Buffer.byteLength("Parent\n");
  tree.edit({
    startIndex: index,
    oldEndIndex: index,
    newEndIndex: index + 2,
    startPosition: { row: 1, column: 0 },
    oldEndPosition: { row: 1, column: 0 },
    newEndPosition: { row: 1, column: 2 },
  });

  const reparsed = instance.parse(changed, tree);
  const fresh = instance.parse(changed);
  assert.strictEqual(reparsed.rootNode.hasError, false);
  assert.strictEqual(reparsed.rootNode.toString(), fresh.rootNode.toString());
  assert.strictEqual(reparsed.rootNode.descendantsOfType("layout_group").length, 1);
});
