const { test } = require("node:test");
const Parser = require("tree-sitter");
const Tasklist = require("..");

require("./regression-cases")(test, () => {
  const parser = new Parser();
  parser.setLanguage(Tasklist);
  return parser;
});
