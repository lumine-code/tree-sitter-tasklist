#include "tree_sitter/parser.h"
#include "tree_sitter/array.h"
#include "tree_sitter/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "unicode.h"

enum TokenType {
  LINE_START,
  CHAPTER_START,
  HEADER_START,
  CHAPTER_SEPARATOR,
  LAYOUT_END,
  SECTION_END,
  SECTION_OPEN,
  SECTION_CLOSE,
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
  LAYOUT_START,
  SECTION_START,
  LINE_END,
  ERROR_SENTINEL,
};

// Both stacks are nondecreasing uint16 sequences. This bound lets their
// complete delta-coded state fit in Tree-sitter's 1024-byte serialization buffer.
enum { MAX_NESTING_DEPTH = 512 };

typedef Array(uint16_t) LevelStack;

typedef struct {
  LevelStack owners;
  LevelStack sections;
  uint16_t current_indent;
  uint16_t next_indent;
  uint16_t current_chapter_level;
  uint16_t next_chapter_level;
  bool next_exists;
  bool chapter_valid;
  bool header_valid;
  bool in_header;
  bool header_space_title;
  bool chapter_space_title;
  bool at_content_start;
  uint8_t failed_formats;
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
  uint32_t increment = character == '\t' ? 2 - column % 2 : 1;
  uint32_t next = (uint32_t)column + increment;
  return next > UINT16_MAX ? UINT16_MAX : (uint16_t)next;
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
  uint16_t chapter_level;
} LineKind;

// Reads the current line from its content start. The caller has already marked
// the token end at that start, so this lookahead never changes the returned
// range of LINE_START/SAME/INDENT.
static LineKind analyze_line(TSLexer *lexer, uint16_t indent, bool special_header_prefix) {
  LineKind kind = {false, special_header_prefix, 0};
  if (special_header_prefix) {
    while (!lexer->eof(lexer) && !is_line_ending(lexer->lookahead)) {
      lexer->advance(lexer, false);
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

  while (!lexer->eof(lexer) && !is_line_ending(lexer->lookahead)) {
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
    lexer->advance(lexer, false);
  }

  kind.chapter =
    indent == 0 && hash_count > 0 && chapter_spaces > 0 &&
    (chapter_title_characters > 0 || chapter_spaces > 1);
  kind.chapter_level = kind.chapter
    ? (hash_count > UINT16_MAX ? UINT16_MAX : (uint16_t)hash_count)
    : 0;
  kind.header =
    !task_marker && !kind.chapter && last_non_space == ':' && last_non_space_index > 0;
  return kind;
}

static void analyze_next_line(Scanner *scanner, TSLexer *lexer) {
  scanner->next_exists = false;
  scanner->next_indent = 0;
  scanner->next_chapter_level = 0;

  if (is_line_ending(lexer->lookahead)) {
    advance_line_ending(lexer);
  } else if (lexer->eof(lexer)) {
    return;
  } else {
    return;
  }

  while (true) {
    uint16_t indent = 0;
    size_t prefix_count = 0;
    while (is_indent_character(lexer->lookahead)) {
      indent = advance_indent(indent, lexer->lookahead);
      prefix_count++;
      lexer->advance(lexer, false);
    }
    if (is_line_ending(lexer->lookahead)) {
      advance_line_ending(lexer);
      continue;
    }
    if (lexer->eof(lexer)) return;
    scanner->next_exists = true;
    scanner->next_indent = indent;
    LineKind kind = analyze_line(
      lexer,
      indent,
      prefix_count > 0 && lexer->lookahead == ':'
    );
    scanner->next_chapter_level = kind.chapter_level;
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
  scanner->current_chapter_level = kind.chapter_level;
  scanner->chapter_valid = kind.chapter;
  scanner->header_valid = kind.header;
  scanner->in_header = false;
  scanner->header_space_title = prefix.special_header_prefix;
  scanner->chapter_space_title = false;
  scanner->at_content_start = true;
  scanner->failed_formats = 0;
  analyze_next_line(scanner, lexer);
}

static bool scan_line_start(Scanner *scanner, TSLexer *lexer) {
  Prefix prefix = consume_prefix(lexer);
  if (lexer->eof(lexer) || is_line_ending(lexer->lookahead)) return false;
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

  if (is_line_ending(lexer->lookahead) || lexer->eof(lexer)) {
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

static bool transition_opens_section(const Scanner *scanner) {
  return scanner->current_chapter_level > 0 && scanner->next_exists &&
         (scanner->next_chapter_level == 0 ||
          scanner->next_chapter_level > scanner->current_chapter_level);
}

static bool transition_closes_section(const Scanner *scanner) {
  if (scanner->sections.size == 0) return false;
  uint16_t level = *array_back(&scanner->sections);
  return !scanner->next_exists ||
         (scanner->next_chapter_level > 0 && scanner->next_chapter_level <= level);
}

// Consumes the already-analyzed transition to the next nonblank line. Blank
// lines and the next line's indentation stay in the anonymous transition token.
static bool consume_transition(Scanner *scanner, TSLexer *lexer, enum TokenType symbol) {
  while (is_ascii_space(lexer->lookahead)) {
    lexer->advance(lexer, false);
  }

  if (lexer->eof(lexer)) {
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
    if (lexer->eof(lexer)) {
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
  if (!lexer->eof(lexer) && !is_line_ending(lexer->lookahead)) return false;
  if (valid_symbols[LAYOUT_END] && transition_closes_group(scanner)) {
    return scan_zero_width(lexer, LAYOUT_END);
  }
  if (valid_symbols[DEDENT] && transition_closes_group(scanner)) {
    (void)array_pop(&scanner->owners);
    return scan_zero_width(lexer, DEDENT);
  }
  if (valid_symbols[SECTION_END] && transition_closes_section(scanner)) {
    return scan_zero_width(lexer, SECTION_END);
  }
  if (valid_symbols[SECTION_CLOSE] && transition_closes_section(scanner)) {
    (void)array_pop(&scanner->sections);
    return scan_zero_width(lexer, SECTION_CLOSE);
  }

  if (!scanner->next_exists) {
    if (scanner->owners.size == 0 && valid_symbols[END]) {
      return consume_transition(scanner, lexer, END);
    }
    return false;
  }

  if (
    valid_symbols[SECTION_OPEN] && transition_opens_section(scanner)
  ) {
    if (scanner->owners.size + scanner->sections.size >= MAX_NESTING_DEPTH) return false;
    if (scanner->sections.size && scanner->current_chapter_level < *array_back(&scanner->sections)) {
      return false;
    }
    array_push(&scanner->sections, scanner->current_chapter_level);
    return consume_transition(scanner, lexer, SECTION_OPEN);
  }
  if (
    valid_symbols[INDENT] && scanner->next_indent > scanner->current_indent
  ) {
    if (scanner->owners.size + scanner->sections.size >= MAX_NESTING_DEPTH) return false;
    if (scanner->owners.size && scanner->current_indent < *array_back(&scanner->owners)) {
      return false;
    }
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
static bool scan_format(Scanner *scanner, TSLexer *lexer, int32_t delimiter) {
  enum TokenType symbol = format_symbol(delimiter);
  uint8_t bit = (uint8_t)(1u << (symbol - STRIKETHROUGH));
  lexer->advance(lexer, false);
  lexer->mark_end(lexer); // Invalid opener falls back to one text character.

  // A whitespace-leading opener cannot match. Any other failed scan proves
  // there is no eligible closer of this kind later in this physical line: a
  // later valid span would also have closed this opener. Remember that proof
  // rather than repeatedly scanning the same suffix. The first failed token's
  // lookahead invalidates the proof if an incremental edit adds a closer.
  if ((scanner->failed_formats & bit) || tasklist_is_whitespace((uint32_t)lexer->lookahead)) {
    return false;
  }

  size_t content_length = 0;
  bool all_non_whitespace = true;
  bool first_non_whitespace = false;
  bool previous_non_whitespace = false;
  bool short_match = false;

  while (!lexer->eof(lexer) && !is_line_ending(lexer->lookahead)) {
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
  scanner->failed_formats |= bit;
  return false;
}

static bool scan_final_header_colon(TSLexer *lexer, int32_t *last_character) {
  lexer->advance(lexer, false);
  // A nonfinal colon belongs to the title. If it proves final, the caller
  // returns false and the internal lexer handles the delimiter instead.
  lexer->mark_end(lexer);
  *last_character = ':';
  while (is_ascii_space(lexer->lookahead)) {
    *last_character = ' ';
    lexer->advance(lexer, false);
  }
  return is_line_ending(lexer->lookahead) || lexer->eof(lexer);
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
  while (!lexer->eof(lexer) && !is_line_ending(lexer->lookahead)) {
    int32_t character = lexer->lookahead;

    if (scanner->in_header && character == ':') {
      // Finish preceding text before probing a colon. The probe's mark must
      // not replace the last non-space title end when this is the delimiter.
      if (consumed && marked) {
        lexer->result_symbol = TEXT;
        return true;
      }
      int32_t last_character = ':';
      if (scan_final_header_colon(lexer, &last_character)) {
        return false;
      }
      consumed = true;
      marked = true;
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
      if (scan_format(scanner, lexer, character)) return true;
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
  while (!lexer->eof(lexer) && !is_line_ending(lexer->lookahead)) {
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
  Scanner *scanner = ts_calloc(1, sizeof(Scanner));
  if (!scanner) return NULL;
  array_init(&scanner->owners);
  array_init(&scanner->sections);
  return scanner;
}

bool tree_sitter_tasklist_external_scanner_scan(
  void *payload,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  Scanner *scanner = payload;

  if (!scanner || valid_symbols[ERROR_SENTINEL]) return false;

  // Finish the line before lexing its transition. A transition lexed while
  // reducing inline content has a different external lex mode and cannot be
  // reused after a complete line is reused during an incremental parse.
  if (valid_symbols[LINE_END] && (lexer->eof(lexer) || is_line_ending(lexer->lookahead))) {
    return scan_zero_width(lexer, LINE_END);
  }
  if (valid_symbols[LINE_START]) return scan_line_start(scanner, lexer);
  // Select a container before its shared line prefix. Otherwise every line
  // retains competing line/layout/section parses, making reusable groups fragile.
  if (valid_symbols[SECTION_START] && transition_opens_section(scanner)) {
    return scan_zero_width(lexer, SECTION_START);
  }
  if (
    valid_symbols[LAYOUT_START] && scanner->next_exists &&
    scanner->next_indent > scanner->current_indent && !transition_opens_section(scanner)
  ) {
    return scan_zero_width(lexer, LAYOUT_START);
  }
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
    valid_symbols[LAYOUT_END] || valid_symbols[SECTION_END] ||
    valid_symbols[SECTION_OPEN] || valid_symbols[SECTION_CLOSE] ||
    valid_symbols[INDENT] || valid_symbols[SAME] || valid_symbols[DEDENT] ||
    valid_symbols[END]
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

static void write_u16(char *buffer, size_t *size, uint16_t value) {
  buffer[(*size)++] = (char)(value & 0xff);
  buffer[(*size)++] = (char)(value >> 8);
}

static uint16_t read_u16(const char *buffer, size_t *offset) {
  uint16_t value =
    (uint16_t)(uint8_t)buffer[*offset] |
    (uint16_t)((uint8_t)buffer[*offset + 1] << 8);
  *offset += 2;
  return value;
}

static void write_bit(char *buffer, size_t *position, bool value) {
  size_t byte = *position / 8;
  unsigned bit = (unsigned)(*position % 8);
  if (bit == 0) buffer[byte] = 0;
  if (value) buffer[byte] = (char)((uint8_t)buffer[byte] | (uint8_t)(1u << bit));
  (*position)++;
}

static bool read_bit(const char *buffer, size_t length, size_t *position, bool *value) {
  if (*position >= length * 8) return false;
  *value = ((uint8_t)buffer[*position / 8] & (1u << (*position % 8))) != 0;
  (*position)++;
  return true;
}

// Rice-code the nonnegative deltas of a nondecreasing stack. Choose the
// remainder width from the average delta, bounding total unary bits by the
// number of entries. Even a split 512-entry nesting stack fits comfortably in
// the serialization buffer; no parent or section is ever silently discarded.
static void write_stack(char *buffer, size_t *size, const LevelStack *stack) {
  write_u16(buffer, size, (uint16_t)stack->size);
  uint32_t span = stack->size ? (uint32_t)*array_back(stack) + 1 : 0;
  unsigned width = 0;
  while (stack->size && ((uint32_t)stack->size << width) < span) width++;
  buffer[(*size)++] = (char)width;
  size_t position = *size * 8;
  uint32_t previous = 0;
  for (uint32_t index = 0; index < stack->size; index++) {
    uint32_t current = (uint32_t)*array_get(stack, index) + 1;
    assert(current >= previous);
    uint32_t delta = current - previous;
    for (uint32_t quotient = delta >> width; quotient > 0; quotient--) {
      write_bit(buffer, &position, true);
    }
    write_bit(buffer, &position, false);
    for (unsigned bit = 0; bit < width; bit++) {
      write_bit(buffer, &position, (delta & (1u << bit)) != 0);
    }
    previous = current;
  }
  *size = (position + 7) / 8;
}

static bool read_stack(
  const char *buffer,
  size_t length,
  size_t *offset,
  LevelStack *stack,
  uint32_t remaining_depth
) {
  if (*offset + 3 > length) return false;
  uint16_t count = read_u16(buffer, offset);
  unsigned width = (uint8_t)buffer[(*offset)++];
  if (count > remaining_depth || width > 16) return false;
  size_t position = *offset * 8;
  uint32_t previous = 0;
  for (uint16_t index = 0; index < count; index++) {
    uint32_t quotient = 0;
    bool bit;
    do {
      if (!read_bit(buffer, length, &position, &bit)) return false;
      if (bit && ++quotient > (((uint32_t)UINT16_MAX + 1) >> width)) return false;
    } while (bit);
    uint32_t delta = quotient << width;
    for (unsigned shift = 0; shift < width; shift++) {
      if (!read_bit(buffer, length, &position, &bit)) return false;
      if (bit) delta |= 1u << shift;
    }
    uint32_t current = previous + delta;
    if (current == 0 || current > (uint32_t)UINT16_MAX + 1) return false;
    array_push(stack, (uint16_t)(current - 1));
    previous = current;
  }
  *offset = (position + 7) / 8;
  return true;
}

static void reset_scanner(Scanner *scanner) {
  array_clear(&scanner->owners);
  array_clear(&scanner->sections);
  LevelStack owners = scanner->owners;
  LevelStack sections = scanner->sections;
  memset(scanner, 0, sizeof(*scanner));
  scanner->owners = owners;
  scanner->sections = sections;
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
  write_u16(buffer, &size, scanner->current_indent);
  write_u16(buffer, &size, scanner->next_indent);
  write_u16(buffer, &size, scanner->current_chapter_level);
  write_u16(buffer, &size, scanner->next_chapter_level);
  buffer[size++] = (char)scanner->failed_formats;
  write_stack(buffer, &size, &scanner->owners);
  write_stack(buffer, &size, &scanner->sections);
  return (unsigned)size;
}

void tree_sitter_tasklist_external_scanner_deserialize(
  void *payload,
  const char *buffer,
  unsigned length
) {
  Scanner *scanner = payload;
  reset_scanner(scanner);
  if (length < 16) return;

  uint8_t flags = (uint8_t)buffer[0];
  scanner->next_exists = (flags & 1u) != 0;
  scanner->chapter_valid = (flags & 2u) != 0;
  scanner->header_valid = (flags & 4u) != 0;
  scanner->in_header = (flags & 8u) != 0;
  scanner->chapter_space_title = (flags & 16u) != 0;
  scanner->header_space_title = (flags & 32u) != 0;
  scanner->at_content_start = (flags & 64u) != 0;
  size_t offset = 1;
  scanner->current_indent = read_u16(buffer, &offset);
  scanner->next_indent = read_u16(buffer, &offset);
  scanner->current_chapter_level = read_u16(buffer, &offset);
  scanner->next_chapter_level = read_u16(buffer, &offset);
  scanner->failed_formats = (uint8_t)buffer[offset++];
  if (
    !read_stack(buffer, length, &offset, &scanner->owners, MAX_NESTING_DEPTH) ||
    !read_stack(
      buffer,
      length,
      &offset,
      &scanner->sections,
      MAX_NESTING_DEPTH - scanner->owners.size
    ) || offset != length
  ) {
    reset_scanner(scanner);
  }
}

void tree_sitter_tasklist_external_scanner_destroy(void *payload) {
  Scanner *scanner = payload;
  if (!scanner) return;
  array_delete(&scanner->owners);
  array_delete(&scanner->sections);
  ts_free(scanner);
}
