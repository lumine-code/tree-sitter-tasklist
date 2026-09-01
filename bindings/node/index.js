const path = require("node:path");
const load = require("node-gyp-build");

const root = path.join(__dirname, "..", "..");
module.exports = load(root);

try {
  module.exports.nodeTypeInfo = require("../../src/node-types.json");
} catch {
  // The generated node types are unavailable before the first grammar build.
}
