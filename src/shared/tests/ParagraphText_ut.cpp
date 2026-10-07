/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "ParagraphText.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

static TempStr Join(const char** lines, int n) {
    StrVec v;
    for (int i = 0; i < n; i++) {
        v.Append(Str(lines[i]));
    }
    return JoinParagraphLinesTemp(v);
}

#define JOIN(...)                              \
    [] {                                       \
        const char* lines[] = {__VA_ARGS__};   \
        return Join(lines, (int)dimof(lines)); \
    }()

static void JoinTest() {
    utassert(str::Eq(JOIN("The peripheral regis-", "ter map is shown in", "Table 5."),
                     StrL("The peripheral register map is shown in Table 5.")));

    // real hyphens stay
    utassert(str::Eq(JOIN("a memory-", "Mapped region"), StrL("a memory-Mapped region")));
    utassert(str::Eq(JOIN("a 16-", "bit timer"), StrL("a 16-bit timer")));

    // surrounding and repeated whitespace, empty lines
    utassert(str::Eq(JOIN("  Set   the bit ", "", "\tto 1. "), StrL("Set the bit to 1.")));
    utassert(len(JoinParagraphLinesTemp(StrVec())) == 0);
}

static void ClassifyTest() {
    utassert(ClassifyParagraph(StrL("The PLL must be locked.")) == ParagraphKind::Translate);
    utassert(ClassifyParagraph(StrL("Features")) == ParagraphKind::Translate);
    utassert(ClassifyParagraph(StrL("(see Table 5)")) == ParagraphKind::Translate);

    utassert(ClassifyParagraph(StrL("GPIOA_MODER")) == ParagraphKind::Skip);
    utassert(ClassifyParagraph(StrL("STM32F407VG")) == ParagraphKind::Skip);
    utassert(ClassifyParagraph(StrL("1.8 V")) == ParagraphKind::Skip);
    utassert(ClassifyParagraph(StrL("400 kHz")) == ParagraphKind::Skip);
    utassert(ClassifyParagraph(StrL("VDD VSS PA0")) == ParagraphKind::Skip);
    utassert(ClassifyParagraph(StrL("0x4002 0000")) == ParagraphKind::Skip);
    utassert(ClassifyParagraph(StrL("   ")) == ParagraphKind::Skip);
}

static void AddLine(PageTextLines& l, const char* text, float x, float y, float dx, int block) {
    constexpr float kSize = 10;
    l.texts.Append(Str(text));
    VecAppend(l.boxes, RectF(x, y, dx, kSize));
    VecAppend(l.blocks, block);
    VecAppend(l.fontSizes, kSize);
    VecAppend(l.bold, false);
    VecAppend(l.mono, false);
}

static void GroupTest() {
    // a 2-row table in one layout block; the second cell of row 1 wraps:
    //   | PeriphID | Serial engine         |
    //   |          | peripheral.           |
    //   | Mode     | Macro of modes.       |
    // then a separate paragraph (another block) of 2 lines
    PageTextLines l;
    AddLine(l, "PeriphID", 10, 100, 50, 0);
    AddLine(l, "Serial engine", 100, 100, 80, 0);
    AddLine(l, "peripheral.", 100, 111, 70, 0);
    AddLine(l, "Mode", 10, 130, 30, 0);
    AddLine(l, "Macro of modes.", 100, 130, 90, 0);
    AddLine(l, "First line of a", 10, 200, 150, 1);
    AddLine(l, "paragraph.", 10, 211, 60, 1);

    PageParagraphs p;
    GroupParagraphs(l, &p);
    utassert(len(p.texts) == 5);
    utassert(str::Eq(p.texts.At(0), StrL("PeriphID")));
    utassert(str::Eq(p.texts.At(1), StrL("Serial engine peripheral.")));
    utassert(str::Eq(p.texts.At(2), StrL("Mode")));
    utassert(str::Eq(p.texts.At(3), StrL("Macro of modes.")));
    utassert(str::Eq(p.texts.At(4), StrL("First line of a paragraph.")));

    // the wrapped cell's box covers both of its lines
    utassert(p.boxes[1].y == 100 && p.boxes[1].dy == 21);
}

void ParagraphText_UnitTests() {
    JoinTest();
    ClassifyTest();
    GroupTest();
}
