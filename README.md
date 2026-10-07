# tree-sitter-tasklist

Parses tasklist files with Tree-sitter.

## Features

- **Grammars**: provides Tree-sitter grammars.
- **Task states**: recognizes high, todo, done, failed, and informational markers.
- **Text formatting**: parses strike, bold, italic, math, and raw spans.
- **Chapters**: nests chapter sections by marker depth.
- **Layout**: exposes nested headers and tasks through indentation.
- **Bindings**: supports Node-API, source, and WebAssembly builds.

## Installation

```sh
npm install tree-sitter @lumine-code/tree-sitter-tasklist
```

## Usage

```js
const Parser = require("tree-sitter");
const Tasklist = require("@lumine-code/tree-sitter-tasklist");

const parser = new Parser();
parser.setLanguage(Tasklist);
const tree = parser.parse("☐ Write the parser\n");
```

Indentation and chapter nesting support up to 512 combined active layout groups and chapter sections. The scanner stores every active level; input beyond this explicit limit produces a parse error instead of silently losing parent state.

## Building

```sh
npm install
npm test
npm run build:wasm
```

## Contributing

Got ideas to make this package better, found a bug, or want to help add new features? Just drop your thoughts on GitHub. Any feedback is welcome!
