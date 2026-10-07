/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Turns the line pieces of a page into paragraphs to translate, and decides
// whether each is worth translating.
//
//   "The peripheral regis-"      "The peripheral register map is
//   "ter map is shown in"   ->    shown in Table 5."
//   "Table 5."
//
// Blocks without any ordinary word (register names, part numbers, values,
// units such as "GPIOA_MODER", "STM32F407VG", "1.8 V", "400 kHz") are skipped.

// Text of a page as laid out, in reading order: one entry per line piece.
// A line with a wide gap in it (table cells side by side) comes as one piece
// per side. Coordinates are page coordinates.
struct PageTextLines {
    StrVec texts;
    Vec<RectF> boxes;
    // layout block (as the engine grouped lines) each piece comes from
    Vec<int> blocks;
    Vec<float> fontSizes;
    Vec<bool> bold;
    // monospaced: commands and code, kept untranslated
    Vec<bool> mono;
};

// line pieces grouped into paragraphs (or table cells), the unit of translation
struct PageParagraphs {
    StrVec texts;
    Vec<RectF> boxes;
    Vec<float> fontSizes;
    Vec<bool> bold;
    Vec<bool> mono;
};

enum class ParagraphKind {
    Translate,
    Skip,
};

TempStr JoinParagraphLinesTemp(const StrVec& lines);
void GroupParagraphs(const PageTextLines& lines, PageParagraphs* out);
ParagraphKind ClassifyParagraph(Str text);

#if IS_DEBUG
void ParagraphText_UnitTests();
#endif
