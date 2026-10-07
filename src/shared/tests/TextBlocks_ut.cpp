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

// one page: a 2-line paragraph near the top, a 1-word block lower down.
// No xref table: MuPDF rebuilds it.
static const Str kTwoBlocksPdf = StrL(
    "%PDF-1.4\n"
    "1 0 obj <</Type/Catalog/Pages 2 0 R>> endobj\n"
    "2 0 obj <</Type/Pages/Kids[3 0 R]/Count 1>> endobj\n"
    "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 300 300]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>> endobj\n"
    "4 0 obj <</Length 122>> stream\n"
    "BT /F1 12 Tf 14 TL 20 260 Td (The peripheral regis-) Tj T* (ter map is shown.) Tj ET\n"
    "BT /F1 12 Tf 20 100 Td (Reset) Tj ET\n"
    "endstream endobj\n"
    "5 0 obj <</Type/Font/Subtype/Type1/BaseFont/Helvetica>> endobj\n"
    "trailer <</Root 1 0 R>>\n"
    "%%EOF\n");

static TempStr BlockTextTemp(const PageTextBlocks& b, int i) {
    int start = b.firstLine[i];
    int end = i + 1 < len(b.firstLine) ? b.firstLine[i + 1] : len(b.lines);
    StrVec lines;
    for (int l = start; l < end; l++) {
        lines.Append(b.lines.At(l));
    }
    return JoinParagraphLinesTemp(lines);
}

void TextBlocks_UnitTests() {
    EngineBase* engine = CreateEngineMupdfFromData(kTwoBlocksPdf, StrL("blocks.pdf"), nullptr);
    utassert(engine != nullptr);
    if (!engine) {
        return;
    }

    PageTextBlocks blocks;
    utassert(engine->ExtractTextBlocks(1, &blocks));
    utassert(len(blocks.boxes) == 2);
    utassert(len(blocks.firstLine) == 2);
    utassert(len(blocks.lines) == 3);

    utassert(str::Eq(blocks.lines.At(0), StrL("The peripheral regis-")));
    utassert(str::Eq(BlockTextTemp(blocks, 0), StrL("The peripheral register map is shown.")));
    utassert(str::Eq(BlockTextTemp(blocks, 1), StrL("Reset")));

    // page coordinates, origin top-left: the first block is above the second
    RectF top = blocks.boxes[0];
    RectF low = blocks.boxes[1];
    utassert(top.y < low.y);
    utassert(top.x > 15 && top.x < 25);
    utassert(low.y > 180 && low.y < 210);

    SafeEngineRelease(&engine);
}
