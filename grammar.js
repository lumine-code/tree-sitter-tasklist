module.exports = grammar({
  name: "tasklist",

  extras: () => [],

  externals: ($) => [
    $._line_start,
    $._chapter_start,
    $._header_start,
    $._chapter_separator,
    $.layout_end,
    $.section_end,
    $._section_open,
    $._section_close,
    $._indent,
    $._same,
    $._dedent,
    $._end,
    $.text,
    $.opaque_text,
    $.strikethrough,
    $.bold,
    $.italic,
    $.math,
    $.raw,
    $._layout_start,
    $._section_start,
    $._line_end,
    $._error_sentinel,
  ],

  rules: {
    document: ($) =>
      seq(
        repeat($._blank_line),
        optional(seq($._line_start, $._items, $._end)),
        optional($._trailing_space),
      ),

    _item: ($) => choice($.chapter_section, $.layout_group, $.line),

    // Bounded groups retain whole unchanged runs during an incremental edit.
    // The groups are hidden, so every consumer still sees the actual items.
    _items: ($) => seq($._item, repeat($._items_64)),

    _items_64: ($) =>
      prec.right(seq($._items_8, ...Array.from({ length: 7 }, () => optional($._items_8)))),

    _items_8: ($) =>
      prec.right(
        seq($._following_item, ...Array.from({ length: 7 }, () => optional($._following_item))),
      ),

    _following_item: ($) => seq($._same, $._item),

    chapter_section: ($) =>
      seq(
        $._section_start,
        field("heading", $.line),
        $._section_open,
        field("body", $.section_body),
        field("end", $.section_end),
        $._section_close,
      ),

    section_body: ($) => $._items,

    layout_group: ($) =>
      seq(
        $._layout_start,
        field("owner", $.line),
        $._indent,
        field("body", $.layout_block),
        field("end", $.layout_end),
        $._dedent,
      ),

    layout_block: ($) => $._items,

    line: ($) =>
      seq(
        choice($.chapter, $.task, $.note, $.header, $.text_line),
        optional($._ascii_trailing_space),
        $._line_end,
      ),

    chapter: ($) =>
      seq(
        $._chapter_start,
        field("marker", $.chapter_marker),
        $._chapter_separator,
        field("title", $.inline),
      ),

    header: ($) =>
      seq($._header_start, field("title", $.inline), optional($._spaces), field("colon", ":")),

    task: ($) =>
      prec.right(
        choice(
          seq(
            field("marker", $.high_marker),
            optional($._spaces),
            optional(field("content", $.inline)),
          ),
          seq(
            field("marker", $.todo_marker),
            optional($._spaces),
            optional(field("content", $.inline)),
          ),
          seq(
            field("marker", $.done_marker),
            optional($._spaces),
            optional(field("content", $.opaque_text)),
          ),
          seq(
            field("marker", $.fail_marker),
            optional($._spaces),
            optional(field("content", $.opaque_text)),
          ),
        ),
      ),

    note: ($) =>
      prec.right(
        seq(
          field("marker", $.info_marker),
          optional($._spaces),
          optional(field("content", $.inline)),
        ),
      ),

    text_line: ($) => field("content", $.inline),

    inline: ($) => seq($._inline_token, repeat($._inline_tokens_64)),

    _inline_tokens_64: ($) =>
      prec.right(
        seq($._inline_tokens_8, ...Array.from({ length: 7 }, () => optional($._inline_tokens_8))),
      ),

    _inline_tokens_8: ($) =>
      prec.right(
        seq($._inline_token, ...Array.from({ length: 7 }, () => optional($._inline_token))),
      ),

    _inline_token: ($) => choice($.text, $.strikethrough, $.bold, $.italic, $.math, $.raw),

    chapter_marker: () => /#+/,
    high_marker: () => "▷",
    todo_marker: () => "☐",
    done_marker: () => "✔",
    fail_marker: () => "✘",
    info_marker: () => "•",

    _spaces: () => / +/,
    _ascii_trailing_space: () => / +/,
    _blank_line: () => /[ \t]*(?:\r\n|\n|\r)/,
    _trailing_space: () => /[ \t]+/,
  },
});
