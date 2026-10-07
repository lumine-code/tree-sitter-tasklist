const assert = require("node:assert/strict");

function position(source, index) {
  const prefix = source.slice(0, index);
  return { row: prefix.split("\n").length - 1, column: index - prefix.lastIndexOf("\n") - 1 };
}

function shape(tree) {
  const cursor = tree.walk();
  const nodes = [];
  while (true) {
    const node = cursor.currentNode;
    nodes.push([node.type, node.startIndex, node.endIndex, node.isMissing]);
    if (cursor.gotoFirstChild()) continue;
    while (!cursor.gotoNextSibling()) {
      if (!cursor.gotoParent()) {
        cursor.delete?.();
        return nodes;
      }
    }
  }
}

function edit(parser, source, index, oldLength, inserted) {
  const tree = parser.parse(source);
  const updated = source.slice(0, index) + inserted + source.slice(index + oldLength);
  tree.edit({
    startIndex: index,
    oldEndIndex: index + oldLength,
    newEndIndex: index + inserted.length,
    startPosition: position(source, index),
    oldEndPosition: position(source, index + oldLength),
    newEndPosition: position(updated, index + inserted.length),
  });
  const incremental = parser.parse(updated, tree);
  const fresh = parser.parse(updated);
  assert.deepStrictEqual(shape(incremental), shape(fresh), JSON.stringify(updated.slice(0, 100)));
  assert.equal(incremental.rootNode.hasError, false);
  tree.delete?.();
  fresh.delete?.();
  return incremental;
}

module.exports = (test, createParser) => {
  test("accepts empty inputs, leading blanks, CR lines, and NUL text", () => {
    const parser = createParser();
    try {
      for (const source of ["", "\n", "\r", "\r\n", " \t", "\t\n\ntext\n", "a\0b\n", "☐ a\0b\n"]) {
        const tree = parser.parse(source);
        assert.equal(tree.rootNode.hasError, false, JSON.stringify(source));
        if (source.includes("\0")) {
          assert.equal(tree.rootNode.descendantsOfType("text")[0].text, "a\0b");
        }
        tree.delete?.();
      }
    } finally {
      parser.delete?.();
    }
  });

  test("keeps whitespace before the final header colon out of its title", () => {
    const parser = createParser();
    try {
      for (const source of ["Header a :\n", "Header*a :\n", "Header* :\n", "*bold* :\n"]) {
        const tree = parser.parse(source);
        assert.equal(tree.rootNode.hasError, false, source);
        const header = tree.rootNode.descendantsOfType("header")[0];
        assert.equal(header.childForFieldName("title").text, source.slice(0, -3));
        assert.equal(header.childForFieldName("colon").text, ":");
        tree.delete?.();
      }
    } finally {
      parser.delete?.();
    }
  });

  test("keeps nonfinal colons in header titles and preserves layout during edits", () => {
    const parser = createParser();
    try {
      for (const [source, title] of [
        ["a::", "a:"],
        ["a: :", "a:"],
        ["Heading: : ", "Heading:"],
        ["Time: 12:00:", "Time: 12:00"],
        ["::", ":"],
        ["*bold*: :", "*bold*:"],
        ["Zażółć: 🐱: :", "Zażółć: 🐱:"],
        ["a:\0:", "a:\0"],
      ]) {
        const tree = parser.parse(source + "\r\n  ☐ child\r\n");
        assert.equal(tree.rootNode.hasError, false, source);
        const header = tree.rootNode.descendantsOfType("header")[0];
        assert.equal(header.childForFieldName("title").text, title);
        assert.equal(header.childForFieldName("colon").text, ":");
        assert.equal(header.childForFieldName("colon").startIndex, source.lastIndexOf(":"));
        assert.equal(tree.rootNode.descendantsOfType("layout_group").length, 1);
        tree.delete?.();
      }
      for (const [source, index, removed, inserted] of [
        ["a:\n  ☐ child\n", 1, 0, ":"],
        ["a:x:\n  ☐ child\n", 2, 1, ""],
        ["a::\n  ☐ child\n", 2, 1, ""],
        ["*bold*:text:\n  ☐ child\n", 7, 4, ""],
      ]) {
        const tree = edit(parser, source, index, removed, inserted);
        assert.equal(tree.rootNode.descendantsOfType("layout_group").length, 1);
        tree.delete?.();
      }
    } finally {
      parser.delete?.();
    }
  });

  test("scans unmatched formats linearly and keeps other delimiter types visible", () => {
    const parser = createParser();
    try {
      for (const delimiter of ["~", "*", "_", "$", "`"]) {
        for (const count of [1024, 8192]) {
          const source = `${`${delimiter}a `.repeat(count)}\n`;
          let consumed = 0;
          parser.setLogger((message) => {
            if (message === "consume" || message.startsWith("consume ")) consumed++;
          });
          const tree = parser.parse(source);
          parser.setLogger(null);
          assert.equal(tree.rootNode.hasError, false);
          assert.ok(consumed < source.length * 12, `${delimiter}: ${consumed} consumes`);
          tree.delete?.();
        }
      }
      for (const source of ["* no* _yes_\n", "*a *b _yes_\n", "*a *b\n*yes*\n"]) {
        const tree = parser.parse(source);
        assert.equal(tree.rootNode.hasError, false);
        assert.ok(
          tree.rootNode
            .descendantsOfType(["bold", "italic"])
            .some((node) => node.text.includes("yes")),
        );
        tree.delete?.();
      }
    } finally {
      parser.setLogger(null);
      parser.delete?.();
    }
  });

  test("invalidates a cached missing closer when edits add or remove one", () => {
    const parser = createParser();
    try {
      const source = "*a ".repeat(200) + "\nAfter\n";
      const index = source.indexOf("\n");
      const changed = source.slice(0, index - 1) + "*" + source.slice(index);
      const added = edit(parser, source, index - 1, 1, "*");
      assert.equal(added.rootNode.descendantsOfType("bold").length, 1);
      added.delete?.();
      edit(parser, changed, index - 1, 1, " ").delete?.();
      edit(parser, "*a *b\n*yes*\n", 5, 1, "").delete?.();
    } finally {
      parser.delete?.();
    }
  });

  test("round-trips complete deep layout and chapter stacks during edits", () => {
    const parser = createParser();
    try {
      for (const kind of ["layout", "chapters", "mixed", "wide"]) {
        const count =
          kind === "wide" ? 64 : kind === "mixed" ? 256 : kind === "chapters" ? 512 : 513;
        const rows = Array.from({ length: count }, (_, index) => {
          if (kind === "chapters") return `${"#".repeat(index + 1)} Heading`;
          if (kind === "mixed" && index < 128) return `${"#".repeat(index + 1)} Heading`;
          return `${" ".repeat(kind === "wide" ? index * 997 : index * 2)}item`;
        });
        const source = rows.join("\n") + "\nSibling\n";
        const tree = parser.parse(source);
        assert.equal(tree.rootNode.hasError, false, kind);
        if (kind === "layout")
          assert.equal(tree.rootNode.descendantsOfType("layout_group").length, 512);
        tree.delete?.();
        const index = source.lastIndexOf("item");
        const changed = edit(
          parser,
          source,
          index >= 0 ? index : source.lastIndexOf("Heading"),
          1,
          "I",
        );
        changed.delete?.();
      }
    } finally {
      parser.delete?.();
    }
  });

  test("preserves bounded item groups across edits in lists, layouts and sections", () => {
    const parser = createParser();
    try {
      const count = 8193;
      for (const prefix of ["", "Parent\n", "# Chapter\n"]) {
        const row = prefix === "Parent\n" ? "  ☐ task\n" : "☐ task\n";
        const source = prefix + row.repeat(count);
        const tree = parser.parse(source);
        const index = source.length - 2;
        tree.edit({
          startIndex: index,
          oldEndIndex: index + 1,
          newEndIndex: index + 1,
          startPosition: position(source, index),
          oldEndPosition: position(source, index + 1),
          newEndPosition: position(source, index + 1),
        });
        let processed = 0;
        parser.setLogger((message) => {
          if (message === "process" || message.startsWith("process ")) processed++;
        });
        const incremental = parser.parse(source.slice(0, index) + "x\n", tree);
        parser.setLogger(null);
        assert.equal(incremental.rootNode.hasError, false);
        assert.equal(incremental.rootNode.descendantsOfType("task").length, count);
        assert.ok(processed < count / 8, `${prefix || "flat"}: ${processed} states`);
        incremental.delete?.();
        tree.delete?.();
      }
    } finally {
      parser.setLogger(null);
      parser.delete?.();
    }
  });
};
