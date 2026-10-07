const path = require("node:path");
const { before, test } = require("node:test");
const { Parser, Language } = require("web-tree-sitter");

let language;
before(async () => {
  await Parser.init();
  language = await Language.load(path.join(__dirname, "../../tree-sitter-tasklist.wasm"));
});

require("../regression-cases")(test, () => {
  const parser = new Parser();
  parser.setLanguage(language);
  return parser;
});
