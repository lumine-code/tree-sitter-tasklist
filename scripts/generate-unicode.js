const fs = require("node:fs");
const path = require("node:path");

const output = process.argv[2];
if (!output) {
  process.stderr.write("Usage: node scripts/generate-unicode.js <output>\n");
  process.exit(1);
}

const word = /[\p{Letter}\p{Mark}\p{Number}\p{Connector_Punctuation}]/u;
const whitespace = /\p{White_Space}/u;

function rangesFor(pattern) {
  const ranges = [];
  let start = null;
  let previous = null;
  for (let codePoint = 0; codePoint <= 0x10ffff; codePoint++) {
    if (codePoint >= 0xd800 && codePoint <= 0xdfff) continue;
    const matches = pattern.test(String.fromCodePoint(codePoint));
    if (matches && start === null) start = codePoint;
    if (!matches && start !== null) {
      ranges.push([start, previous]);
      start = null;
    }
    previous = codePoint;
  }
  if (start !== null) ranges.push([start, previous]);
  return ranges;
}

function renderRanges(name, ranges) {
  const entries = ranges
    .map(([start, end]) => `  {0x${start.toString(16)}, 0x${end.toString(16)}},`)
    .join("\n");
  return `static const TasklistCodePointRange ${name}[] = {\n${entries}\n};`;
}

const source = `#ifndef TREE_SITTER_TASKLIST_UNICODE_H_\n#define TREE_SITTER_TASKLIST_UNICODE_H_\n\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\ntypedef struct {\n  uint32_t start;\n  uint32_t end;\n} TasklistCodePointRange;\n\n${renderRanges("TASKLIST_WORD_RANGES", rangesFor(word))}\n\n${renderRanges("TASKLIST_WHITESPACE_RANGES", rangesFor(whitespace))}\n\nstatic bool tasklist_code_point_in_ranges(\n  uint32_t code_point,\n  const TasklistCodePointRange *ranges,\n  size_t count\n) {\n  size_t low = 0;\n  size_t high = count;\n  while (low < high) {\n    size_t middle = low + (high - low) / 2;\n    TasklistCodePointRange range = ranges[middle];\n    if (code_point < range.start) {\n      high = middle;\n    } else if (code_point > range.end) {\n      low = middle + 1;\n    } else {\n      return true;\n    }\n  }\n  return false;\n}\n\nstatic bool tasklist_is_word(uint32_t code_point) {\n  return tasklist_code_point_in_ranges(\n    code_point,\n    TASKLIST_WORD_RANGES,\n    sizeof(TASKLIST_WORD_RANGES) / sizeof(TASKLIST_WORD_RANGES[0])\n  );\n}\n\nstatic bool tasklist_is_whitespace(uint32_t code_point) {\n  return tasklist_code_point_in_ranges(\n    code_point,\n    TASKLIST_WHITESPACE_RANGES,\n    sizeof(TASKLIST_WHITESPACE_RANGES) / sizeof(TASKLIST_WHITESPACE_RANGES[0])\n  );\n}\n\n#endif\n`;

fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, source);
