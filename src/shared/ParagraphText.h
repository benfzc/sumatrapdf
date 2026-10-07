/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Turns the lines of one text block (as laid out on the page) into a single
// paragraph string to translate, and decides whether it's worth translating.
//
//   "The peripheral regis-"      "The peripheral register map is
//   "ter map is shown in"   ->    shown in Table 5."
//   "Table 5."
//
// Blocks without any ordinary word (register names, part numbers, values,
// units such as "GPIOA_MODER", "STM32F407VG", "1.8 V", "400 kHz") are skipped.

enum class ParagraphKind {
    Translate,
    Skip,
};

TempStr JoinParagraphLinesTemp(const StrVec& lines);
ParagraphKind ClassifyParagraph(Str text);

#if IS_DEBUG
void ParagraphText_UnitTests();
#endif
