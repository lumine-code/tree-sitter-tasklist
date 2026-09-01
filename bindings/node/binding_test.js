const assert = require("node:assert");
const { test } = require("node:test");
const Tasklist = require(".");

test("loads the grammar through the Node-API binding", () => {
  assert.strictEqual(Tasklist.name, "tasklist");
  assert.ok(Tasklist.language);
  assert.ok(Array.isArray(Tasklist.nodeTypeInfo));
});
