/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/GuessFileType.h"

#include "gui/UIModels.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "ParagraphText.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

// one page: a bold heading, a 2-line paragraph, a table row with two cells,
// a 1-word line and a command in a monospaced font. No xref table: MuPDF rebuilds it.
static const Str kPagePdf = StrL(
    "%PDF-1.4\n"
    "1 0 obj <</Type/Catalog/Pages 2 0 R>> endobj\n"
    "2 0 obj <</Type/Pages/Kids[3 0 R]/Count 1>> endobj\n"
    "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 300 300]/Contents 4 0 R/Resources<</Font<</F1 5 0 R/F2 6 0 R/F3 7 "
    "0 R>>>>>> endobj\n"
    "4 0 obj <</Length 296>> stream\n"
    "BT /F2 18 Tf 20 280 Td (Bringup CAN interface) Tj ET\n"
    "BT /F1 12 Tf 14 TL 20 250 Td (The peripheral regis-) Tj T* (ter map is shown.) Tj ET\n"
    "BT /F1 12 Tf 20 150 Td (Mode) Tj 130 0 Td (Macro of FIFO modes.) Tj ET\n"
    "BT /F1 12 Tf 20 100 Td (Reset) Tj ET\n"
    "BT /F3 10 Tf 20 60 Td (ip link set can0 up) Tj ET\n"
    "endstream endobj\n"
    "5 0 obj <</Type/Font/Subtype/Type1/BaseFont/Helvetica>> endobj\n"
    "6 0 obj <</Type/Font/Subtype/Type1/BaseFont/Helvetica-Bold>> endobj\n"
    "7 0 obj <</Type/Font/Subtype/Type1/BaseFont/Courier>> endobj\n"
    "trailer <</Root 1 0 R>>\n"
    "%%EOF\n");

void TextLines_UnitTests() {
    EngineBase* engine = CreateEngineMupdfFromData(kPagePdf, StrL("lines.pdf"), nullptr);
    utassert(engine != nullptr);
    if (!engine) {
        return;
    }

    PageTextLines lines;
    utassert(engine->ExtractTextLines(1, &lines));
    // the table row is split at the wide gap: 2 pieces
    utassert(len(lines.texts) == 7);
    utassert(str::Eq(lines.texts.At(1), StrL("The peripheral regis-")));
    utassert(str::Eq(lines.texts.At(3), StrL("Mode")));
    utassert(str::Eq(lines.texts.At(4), StrL("Macro of FIFO modes.")));

    PageParagraphs paras;
    GroupParagraphs(lines, &paras);
    utassert(len(paras.texts) == 6);
    utassert(str::Eq(paras.texts.At(0), StrL("Bringup CAN interface")));
    utassert(str::Eq(paras.texts.At(1), StrL("The peripheral register map is shown.")));
    utassert(str::Eq(paras.texts.At(2), StrL("Mode")));
    utassert(str::Eq(paras.texts.At(3), StrL("Macro of FIFO modes.")));
    utassert(str::Eq(paras.texts.At(4), StrL("Reset")));
    utassert(str::Eq(paras.texts.At(5), StrL("ip link set can0 up")));

    // commands keep their font: monospaced, so they stay untranslated
    utassert(paras.mono[5]);
    utassert(!paras.mono[4]);

    // size and weight of the heading carry over
    utassert(paras.bold[0]);
    utassert(!paras.bold[1]);
    utassert(paras.fontSizes[0] > 17 && paras.fontSizes[0] < 19);
    utassert(paras.fontSizes[1] > 11 && paras.fontSizes[1] < 13);

    // page coordinates, origin top-left: the cell right of "Mode" starts at x=150
    utassert(paras.boxes[0].y < paras.boxes[1].y);
    utassert(paras.boxes[3].x > 145 && paras.boxes[3].x < 155);

    SafeEngineRelease(&engine);
}
