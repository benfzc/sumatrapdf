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

void ParagraphText_UnitTests() {
    JoinTest();
    ClassifyTest();
}
