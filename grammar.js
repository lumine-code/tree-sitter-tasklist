module.exports = grammar({
  name: "tasklist",

  extras: () => [],

  externals: ($) => [
    $._line_start,
    $._chapter_start,
    $._header_start,
    $._chapter_separator,
    $.layout_end,
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
  ],

  rules: {
    document: ($) =>
      seq(
        repeat($._blank_line),
        optional(seq($._line_start, $._item, repeat(seq($._same, $._item)), $._end)),
        optional($._trailing_space),
      ),

    _item: ($) => choice($.layout_group, $.line),

    layout_group: ($) =>
      seq(
        field("owner", $.line),
        $._indent,
        field("body", $.layout_block),
        field("end", $.layout_end),
        $._dedent,
      ),

    layout_block: ($) => seq($._item, repeat(seq($._same, $._item))),

    line: ($) =>
      seq(
        choice($.chapter, $.task, $.note, $.header, $.text_line),
        optional($._ascii_trailing_space),
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

    inline: ($) => repeat1(choice($.text, $.strikethrough, $.bold, $.italic, $.math, $.raw)),

    chapter_marker: () => /#+/,
    high_marker: () => "▷",
    todo_marker: () => "☐",
    done_marker: () => "✔",
    fail_marker: () => "✘",
    info_marker: () => "•",

    _spaces: () => / +/,
    _ascii_trailing_space: () => / +/,
    _blank_line: () => /[ \t]*\r?\n/,
    _trailing_space: () => /[ \t]+/,
  },
});
