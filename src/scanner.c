#include "tree_sitter/parser.h"
#include "tree_sitter/array.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "unicode.h"

enum TokenType {
  LINE_START,
  CHAPTER_START,
  HEADER_START,
  CHAPTER_SEPARATOR,
  LAYOUT_END,
  INDENT,
  SAME,
  DEDENT,
  END,
  TEXT,
  OPAQUE_TEXT,
  STRIKETHROUGH,
  BOLD,
  ITALIC,
  MATH,
  RAW,
};

typedef struct {
  Array(uint16_t) owners;
  uint16_t current_indent;
  uint16_t next_indent;
  bool next_exists;
  bool chapter_valid;
  bool header_valid;
  bool in_header;
  bool header_space_title;
  bool chapter_space_title;
  bool at_content_start;
} Scanner;

static bool is_ascii_space(int32_t character) {
  return character == ' ';
}

static bool is_indent_character(int32_t character) {
  return character == ' ' || character == '\t';
}

static bool is_line_ending(int32_t character) {
  return character == '\r' || character == '\n';
}

static bool is_task_marker(int32_t character) {
  return character == 0x25B7 || character == 0x2610 || character == 0x2714 ||
         character == 0x2718 || character == 0x2022;
}

static uint16_t advance_indent(uint16_t column, int32_t character) {
  if (character == '\t') {
    return (uint16_t)(column + (2 - column % 2));
  }
  return column == UINT16_MAX ? UINT16_MAX : (uint16_t)(column + 1);
}

static void advance_line_ending(TSLexer *lexer) {
  if (lexer->lookahead == '\r') {
    lexer->advance(lexer, false);
  }
  if (lexer->lookahead == '\n') {
    lexer->advance(lexer, false);
  }
}

typedef struct {
  bool special_header_prefix;
  uint16_t indent;
} Prefix;

// Consumes a line prefix and leaves the token end at the content. A line made
// only of indentation plus ':' is the TextMate grammar's odd backtracking case:
// its final whitespace character is the header title, not indentation.
static Prefix consume_prefix(TSLexer *lexer) {
  Prefix prefix = {false, 0};
  size_t count = 0;
  while (is_indent_character(lexer->lookahead)) {
    lexer->mark_end(lexer);
    prefix.indent = advance_indent(prefix.indent, lexer->lookahead);
    count++;
    lexer->advance(lexer, false);
  }
  prefix.special_header_prefix = count > 0 && lexer->lookahead == ':';
  if (!prefix.special_header_prefix) {
    lexer->mark_end(lexer);
  }
  return prefix;
}

typedef struct {
  bool chapter;
  bool header;
} LineKind;

// Reads the current line from its content start. The caller has already marked
// the token end at that start, so this lookahead never changes the returned
// range of LINE_START/SAME/INDENT.
static LineKind analyze_line(TSLexer *lexer, uint16_t indent, bool special_header_prefix) {
  LineKind kind = {false, special_header_prefix};
  if (special_header_prefix) {
    while (lexer->lookahead && !is_line_ending(lexer->lookahead)) {
      lexer->advance(lexer, true);
    }
    return kind;
  }

  bool first = true;
  bool task_marker = false;
  size_t character_count = 0;
  size_t last_non_space_index = 0;
  int32_t last_non_space = 0;
  size_t hash_count = 0;
  size_t chapter_spaces = 0;
  size_t chapter_title_characters = 0;
  enum { CHAPTER_HASHES, CHAPTER_SPACES, CHAPTER_TITLE } chapter_state = CHAPTER_HASHES;

  while (lexer->lookahead && !is_line_ending(lexer->lookahead)) {
    int32_t character = lexer->lookahead;
    if (first) {
      task_marker = is_task_marker(character);
      first = false;
    }

    if (!is_ascii_space(character)) {
      last_non_space = character;
      last_non_space_index = character_count;
    }

    if (chapter_state == CHAPTER_HASHES) {
      if (character == '#') {
        hash_count++;
      } else if (character == ' ' && hash_count > 0) {
        chapter_state = CHAPTER_SPACES;
        chapter_spaces++;
      } else {
        chapter_state = CHAPTER_TITLE;
      }
    } else if (chapter_state == CHAPTER_SPACES) {
      if (character == ' ') {
        chapter_spaces++;
      } else {
        chapter_state = CHAPTER_TITLE;
        chapter_title_characters++;
      }
    } else {
      chapter_title_characters++;
    }

    character_count++;
    lexer->advance(lexer, true);
  }

  kind.chapter =
    indent == 0 && hash_count > 0 && chapter_spaces > 0 &&
    (chapter_title_characters > 0 || chapter_spaces > 1);
  kind.header =
    !task_marker && !kind.chapter && last_non_space == ':' && last_non_space_index > 0;
  return kind;
}

static void analyze_next_line(Scanner *scanner, TSLexer *lexer) {
  scanner->next_exists = false;
  scanner->next_indent = 0;

  if (is_line_ending(lexer->lookahead)) {
    advance_line_ending(lexer);
  } else if (!lexer->lookahead) {
    return;
  } else {
    return;
  }

  while (true) {
    uint16_t indent = 0;
    while (is_indent_character(lexer->lookahead)) {
      indent = advance_indent(indent, lexer->lookahead);
      lexer->advance(lexer, true);
    }
    if (is_line_ending(lexer->lookahead)) {
      advance_line_ending(lexer);
      continue;
    }
    if (!lexer->lookahead) return;
    scanner->next_exists = true;
    scanner->next_indent = indent;
    return;
  }
}

static void analyze_current_and_next(
  Scanner *scanner,
  TSLexer *lexer,
  Prefix prefix
) {
  LineKind kind = analyze_line(
    lexer,
    prefix.indent,
    prefix.special_header_prefix
  );
  scanner->current_indent = prefix.indent;
  scanner->chapter_valid = kind.chapter;
  scanner->header_valid = kind.header;
  scanner->in_header = false;
  scanner->header_space_title = prefix.special_header_prefix;
  scanner->chapter_space_title = false;
  scanner->at_content_start = true;
  analyze_next_line(scanner, lexer);
}

static bool scan_line_start(Scanner *scanner, TSLexer *lexer) {
  Prefix prefix = consume_prefix(lexer);
  analyze_current_and_next(scanner, lexer, prefix);
  lexer->result_symbol = LINE_START;
  return true;
}

static bool scan_zero_width(TSLexer *lexer, enum TokenType symbol) {
  lexer->mark_end(lexer);
  lexer->result_symbol = symbol;
  return true;
}

static bool scan_chapter_separator(Scanner *scanner, TSLexer *lexer) {
  size_t count = 0;
  while (is_ascii_space(lexer->lookahead)) {
    lexer->mark_end(lexer);
    lexer->advance(lexer, false);
    count++;
  }
  if (count == 0) return false;

  if (is_line_ending(lexer->lookahead) || !lexer->lookahead) {
    if (count < 2) return false;
    scanner->chapter_space_title = true;
    // The mark left by the loop is before the last space, which becomes title.
  } else {
    lexer->mark_end(lexer);
  }
  lexer->result_symbol = CHAPTER_SEPARATOR;
  return true;
}

static bool transition_closes_group(const Scanner *scanner) {
  if (scanner->owners.size == 0) return false;
  uint16_t owner = *array_back(&scanner->owners);
  return !scanner->next_exists || scanner->next_indent <= owner;
}

// Consumes the already-analyzed transition to the next nonblank line. Blank
// lines and the next line's indentation stay in the anonymous transition token.
static bool consume_transition(Scanner *scanner, TSLexer *lexer, enum TokenType symbol) {
  while (is_ascii_space(lexer->lookahead)) {
    lexer->advance(lexer, false);
  }

  if (!lexer->lookahead) {
    lexer->mark_end(lexer);
    lexer->result_symbol = symbol;
    return true;
  }
  if (!is_line_ending(lexer->lookahead)) return false;
  advance_line_ending(lexer);

  while (true) {
    Prefix prefix = consume_prefix(lexer);
    if (is_line_ending(lexer->lookahead)) {
      advance_line_ending(lexer);
      continue;
    }
    if (!lexer->lookahead) {
      lexer->mark_end(lexer);
      lexer->result_symbol = symbol;
      return true;
    }
    analyze_current_and_next(scanner, lexer, prefix);
    lexer->result_symbol = symbol;
    return true;
  }
}

static bool scan_transition(
  Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (lexer->lookahead && !is_line_ending(lexer->lookahead)) return false;
  if (valid_symbols[LAYOUT_END] && transition_closes_group(scanner)) {
    return scan_zero_width(lexer, LAYOUT_END);
  }
  if (valid_symbols[DEDENT] && transition_closes_group(scanner)) {
    array_pop(&scanner->owners);
    return scan_zero_width(lexer, DEDENT);
  }

  if (!scanner->next_exists) {
    if (scanner->owners.size == 0 && valid_symbols[END]) {
      return consume_transition(scanner, lexer, END);
    }
    return false;
  }

  if (
    valid_symbols[INDENT] && scanner->next_indent > scanner->current_indent
  ) {
    array_push(&scanner->owners, scanner->current_indent);
    return consume_transition(scanner, lexer, INDENT);
  }
  if (valid_symbols[SAME] && !transition_closes_group(scanner)) {
    return consume_transition(scanner, lexer, SAME);
  }
  return false;
}

static enum TokenType format_symbol(int32_t delimiter) {
  switch (delimiter) {
    case '~': return STRIKETHROUGH;
    case '*': return BOLD;
    case '_': return ITALIC;
    case '$': return MATH;
    case '`': return RAW;
    default: return TEXT;
  }
}

static bool is_format_delimiter(int32_t character) {
  return character == '~' || character == '*' || character == '_' ||
         character == '$' || character == '`';
}

// Implements `DELIM(\S{,2}|\S.+?\S)DELIM` exactly: the short alternative is
// greedy up to two characters, while the long alternative takes the earliest
// valid closing delimiter.
static bool scan_format(TSLexer *lexer, int32_t delimiter) {
  enum TokenType symbol = format_symbol(delimiter);
  lexer->advance(lexer, false);
  lexer->mark_end(lexer); // Invalid opener falls back to one text character.

  size_t content_length = 0;
  bool all_non_whitespace = true;
  bool first_non_whitespace = false;
  bool previous_non_whitespace = false;
  bool short_match = false;

  while (lexer->lookahead && !is_line_ending(lexer->lookahead)) {
    int32_t character = lexer->lookahead;
    if (character == delimiter) {
      lexer->advance(lexer, false);
      bool right_boundary = !tasklist_is_word((uint32_t)lexer->lookahead);
      if (
        content_length <= 2 && all_non_whitespace && right_boundary
      ) {
        short_match = true;
        lexer->mark_end(lexer);
        if (content_length == 2) {
          lexer->result_symbol = symbol;
          return true;
        }
      } else if (
        !short_match && content_length >= 3 && first_non_whitespace &&
        previous_non_whitespace && right_boundary
      ) {
        lexer->mark_end(lexer);
        lexer->result_symbol = symbol;
        return true;
      }

      content_length++;
      if (content_length == 1) first_non_whitespace = true;
      previous_non_whitespace = true;
      if (short_match && content_length > 2) {
        lexer->result_symbol = symbol;
        return true;
      }
      continue;
    }

    bool non_whitespace = !tasklist_is_whitespace((uint32_t)character);
    lexer->advance(lexer, false);
    content_length++;
    if (content_length == 1) first_non_whitespace = non_whitespace;
    all_non_whitespace = all_non_whitespace && non_whitespace;
    previous_non_whitespace = non_whitespace;
    if (short_match && content_length > 2) {
      lexer->result_symbol = symbol;
      return true;
    }
  }

  if (short_match) {
    lexer->result_symbol = symbol;
    return true;
  }
  return false;
}

static bool scan_final_header_colon(TSLexer *lexer, int32_t *last_character) {
  lexer->advance(lexer, false);
  *last_character = ':';
  while (is_ascii_space(lexer->lookahead)) {
    *last_character = ' ';
    lexer->advance(lexer, false);
  }
  return is_line_ending(lexer->lookahead) || !lexer->lookahead;
}

static bool scan_text(Scanner *scanner, TSLexer *lexer) {
  scanner->at_content_start = false;
  if (scanner->header_space_title && is_indent_character(lexer->lookahead)) {
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    scanner->header_space_title = false;
    lexer->result_symbol = TEXT;
    return true;
  }
  if (scanner->chapter_space_title && is_ascii_space(lexer->lookahead)) {
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    scanner->chapter_space_title = false;
    lexer->result_symbol = TEXT;
    return true;
  }

  bool consumed = false;
  bool marked = false;
  int32_t previous = 0;
  while (lexer->lookahead && !is_line_ending(lexer->lookahead)) {
    int32_t character = lexer->lookahead;

    if (scanner->in_header && character == ':') {
      int32_t last_character = ':';
      if (scan_final_header_colon(lexer, &last_character)) {
        if (!consumed) return false;
        lexer->result_symbol = TEXT;
        return true;
      }
      consumed = true;
      previous = last_character;
      continue;
    }

    if (
      is_format_delimiter(character) &&
      (!consumed || !tasklist_is_word((uint32_t)previous))
    ) {
      if (consumed) {
        lexer->mark_end(lexer); // Include internal spaces before the delimiter.
        marked = true;
        lexer->result_symbol = TEXT;
        return true;
      }
      if (scan_format(lexer, character)) return true;
      lexer->result_symbol = TEXT;
      return true;
    }

    lexer->advance(lexer, false);
    consumed = true;
    previous = character;
    if (!is_ascii_space(character)) {
      lexer->mark_end(lexer);
      marked = true;
    }
  }

  if (!consumed || !marked) return false;
  lexer->result_symbol = TEXT;
  return true;
}

static bool scan_opaque_text(TSLexer *lexer) {
  bool consumed = false;
  while (lexer->lookahead && !is_line_ending(lexer->lookahead)) {
    int32_t character = lexer->lookahead;
    lexer->advance(lexer, false);
    consumed = true;
    if (!is_ascii_space(character)) lexer->mark_end(lexer);
  }
  if (!consumed) return false;
  lexer->result_symbol = OPAQUE_TEXT;
  return true;
}

void *tree_sitter_tasklist_external_scanner_create(void) {
  Scanner *scanner = calloc(1, sizeof(Scanner));
  if (!scanner) return NULL;
  array_init(&scanner->owners);
  return scanner;
}

bool tree_sitter_tasklist_external_scanner_scan(
  void *payload,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  Scanner *scanner = payload;

  if (valid_symbols[LINE_START]) return scan_line_start(scanner, lexer);
  if (valid_symbols[CHAPTER_START] && scanner->chapter_valid) {
    scanner->at_content_start = false;
    return scan_zero_width(lexer, CHAPTER_START);
  }
  if (valid_symbols[HEADER_START] && scanner->header_valid) {
    scanner->in_header = true;
    scanner->at_content_start = false;
    return scan_zero_width(lexer, HEADER_START);
  }
  if (valid_symbols[CHAPTER_SEPARATOR]) {
    return scan_chapter_separator(scanner, lexer);
  }

  if (
    valid_symbols[LAYOUT_END] || valid_symbols[INDENT] || valid_symbols[SAME] ||
    valid_symbols[DEDENT] || valid_symbols[END]
  ) {
    bool transitioned = scan_transition(scanner, lexer, valid_symbols);
    if (transitioned) return true;
  }

  if (scanner->at_content_start && is_task_marker(lexer->lookahead)) {
    return false;
  }
  if (scanner->at_content_start && is_ascii_space(lexer->lookahead)) {
    return false;
  }
  if (valid_symbols[OPAQUE_TEXT]) {
    scanner->at_content_start = false;
    return scan_opaque_text(lexer);
  }
  if (
    valid_symbols[TEXT] || valid_symbols[STRIKETHROUGH] ||
    valid_symbols[BOLD] || valid_symbols[ITALIC] || valid_symbols[MATH] ||
    valid_symbols[RAW]
  ) {
    return scan_text(scanner, lexer);
  }
  return false;
}

unsigned tree_sitter_tasklist_external_scanner_serialize(
  void *payload,
  char *buffer
) {
  Scanner *scanner = payload;
  size_t size = 0;
  uint8_t flags =
    (scanner->next_exists ? 1u : 0u) |
    (scanner->chapter_valid ? 2u : 0u) |
    (scanner->header_valid ? 4u : 0u) |
    (scanner->in_header ? 8u : 0u) |
    (scanner->chapter_space_title ? 16u : 0u) |
    (scanner->header_space_title ? 32u : 0u) |
    (scanner->at_content_start ? 64u : 0u);
  buffer[size++] = (char)flags;
  buffer[size++] = (char)(scanner->current_indent & 0xff);
  buffer[size++] = (char)(scanner->current_indent >> 8);
  buffer[size++] = (char)(scanner->next_indent & 0xff);
  buffer[size++] = (char)(scanner->next_indent >> 8);

  for (
    size_t index = 0;
    index < scanner->owners.size && size + 2 <= TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
    index++
  ) {
    uint16_t value = *array_get(&scanner->owners, index);
    buffer[size++] = (char)(value & 0xff);
    buffer[size++] = (char)(value >> 8);
  }
  return (unsigned)size;
}

void tree_sitter_tasklist_external_scanner_deserialize(
  void *payload,
  const char *buffer,
  unsigned length
) {
  Scanner *scanner = payload;
  array_clear(&scanner->owners);
  scanner->current_indent = 0;
  scanner->next_indent = 0;
  scanner->next_exists = false;
  scanner->chapter_valid = false;
  scanner->header_valid = false;
  scanner->in_header = false;
  scanner->header_space_title = false;
  scanner->chapter_space_title = false;
  scanner->at_content_start = false;
  if (length < 5) return;

  uint8_t flags = (uint8_t)buffer[0];
  scanner->next_exists = (flags & 1u) != 0;
  scanner->chapter_valid = (flags & 2u) != 0;
  scanner->header_valid = (flags & 4u) != 0;
  scanner->in_header = (flags & 8u) != 0;
  scanner->chapter_space_title = (flags & 16u) != 0;
  scanner->header_space_title = (flags & 32u) != 0;
  scanner->at_content_start = (flags & 64u) != 0;
  scanner->current_indent =
    (uint16_t)(uint8_t)buffer[1] | (uint16_t)((uint8_t)buffer[2] << 8);
  scanner->next_indent =
    (uint16_t)(uint8_t)buffer[3] | (uint16_t)((uint8_t)buffer[4] << 8);
  for (size_t index = 5; index + 1 < length; index += 2) {
    uint16_t value =
      (uint16_t)(uint8_t)buffer[index] |
      (uint16_t)((uint8_t)buffer[index + 1] << 8);
    array_push(&scanner->owners, value);
  }
}

void tree_sitter_tasklist_external_scanner_destroy(void *payload) {
  Scanner *scanner = payload;
  array_delete(&scanner->owners);
  free(scanner);
}
