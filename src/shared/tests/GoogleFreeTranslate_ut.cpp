/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "GoogleFreeTranslate.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

// real replies from translate.googleapis.com (spike/google-translate-spike.sh)
// t1: one sentence, sl=en tl=zh-TW
static const Str kReplyOne = StrL(
    "[[[\"設備進入睡眠模式。\",\"The device enters sleep mode.\",null,null,3,null,null,[[],[]],["
    "[[\"6ffafab0da7e640be86ac09d0d5e539c\",\"en_zh_2023q1.md\"]],[[\"84b1db8c3c94d5ff25f228b8bdffe536\","
    "\"zh_zh-hant_2023q3.md\"]]]]],null,\"en\",null,null,null,null,[]]");

// t3: 5 paragraphs batched with blank lines
static const Str kReplyBatch = StrL(
    "[[[\"如果軟體停止回應，看門狗計時器將重置系統。\\n\\n每個 DMA 流都可以配"
    "置為記憶體到週邊裝置的傳輸。\\n\\n功耗取決於時脈頻率和啟用的周邊。\\n\\n"
    "引導程式儲存在系統記憶體中，並透過 BOOT0 引腳啟動。\\n\\n有關完整的暫存"
    "器說明，請參閱參考手冊。\",\"The watchdog timer resets the system if software stops resp"
    "onding.\\n\\nEach DMA stream can be configured for memory-to-peripheral transfers.\\n\\nPower consum"
    "ption depends on the clock frequency and the enabled peripherals.\\n\\nThe bootloader is stored in s"
    "ystem memory and is activated through the BOOT0 pin.\\n\\nRefer to the reference manual for the full"
    " register description.\",null,null,3,null,null,[[],[],[],[],[],[],[],[],[],[]],[[[\"6ffafab0da7e640b"
    "e86ac09d0d5e539c\",\"en_zh_2023q1.md\"],true],[[\"6ffafab0da7e640be86ac09d0d5e539c\",\"en_zh_2023q1."
    "md\"],true],[[\"6ffafab0da7e640be86ac09d0d5e539c\",\"en_zh_2023q1.md\"],true],[[\"6ffafab0da7e640be8"
    "6ac09d0d5e539c\",\"en_zh_2023q1.md\"],true],[[\"6ffafab0da7e640be86ac09d0d5e539c\",\"en_zh_2023q1.md"
    "\"],true],[[\"84b1db8c3c94d5ff25f228b8bdffe536\",\"zh_zh-hant_2023q3.md\"],true],[[\"84b1db8c3c94d5f"
    "f25f228b8bdffe536\",\"zh_zh-hant_2023q3.md\"],true],[[\"84b1db8c3c94d5ff25f228b8bdffe536\",\"zh_zh-h"
    "ant_2023q3.md\"],true],[[\"84b1db8c3c94d5ff25f228b8bdffe536\",\"zh_zh-hant_2023q3.md\"],true],[[\"84"
    "b1db8c3c94d5ff25f228b8bdffe536\",\"zh_zh-hant_2023q3.md\"],true]]]],null,\"en\",null,null,null,null,"
    "[]]");

static const Str kSorryPage = StrL("<html><head><title>Sorry...</title></head></html>");

static void ParseTest() {
    StrVec out;
    utassert(GtxParse(200, kReplyOne, 1, out) == GtxStatus::Ok);
    utassert(len(out) == 1);
    utassert(str::Eq(out.At(0), StrL("設備進入睡眠模式。")));

    StrVec batch;
    utassert(GtxParse(200, kReplyBatch, 5, batch) == GtxStatus::Ok);
    utassert(len(batch) == 5);
    utassert(str::Eq(batch.At(0), StrL("如果軟體停止回應，看門狗計時器將重置系統。")));
    utassert(str::Eq(batch.At(3), StrL("引導程式儲存在系統記憶體中，並透過 BOOT0 引腳啟動。")));
    utassert(str::Eq(batch.At(4), StrL("有關完整的暫存器說明，請參閱參考手冊。")));

    // paragraph count mismatch: caller retries one by one
    StrVec mismatch;
    utassert(GtxParse(200, kReplyBatch, 4, mismatch) == GtxStatus::BadResponse);

    StrVec bad;
    utassert(GtxParse(200, StrL("[[], null]"), 1, bad) == GtxStatus::BadResponse);
    utassert(GtxParse(200, StrL("not json"), 1, bad) == GtxStatus::BadResponse);
    utassert(GtxParse(429, kSorryPage, 1, bad) == GtxStatus::Blocked);
    utassert(GtxParse(200, kSorryPage, 1, bad) == GtxStatus::Blocked);
    utassert(GtxParse(500, StrL(""), 1, bad) == GtxStatus::Failed);
    utassert(len(bad) == 0);
}

static void BatchTest() {
    StrVec paras;
    paras.Append(StrL("aaaa"));   // 4
    paras.Append(StrL("bbbb"));   // +2 sep +4 = 10
    paras.Append(StrL("cccccc")); // +2 +6 = 18
    paras.Append(StrL("dd"));

    utassert(GtxBatchEnd(paras, 0, 10) == 2);
    utassert(GtxBatchEnd(paras, 0, 17) == 2);
    utassert(GtxBatchEnd(paras, 0, 18) == 3);
    utassert(GtxBatchEnd(paras, 2, 100) == 4);
    // an oversized paragraph still goes, alone
    utassert(GtxBatchEnd(paras, 2, 3) == 3);
    utassert(GtxBatchEnd(paras, 4, 100) == 4);
}

static void BodyTest() {
    StrVec paras;
    paras.Append(StrL("Set bit 3."));
    paras.Append(StrL("A&B=C"));
    TempStr body = GtxBodyTemp(StrL("en"), StrL("zh-TW"), paras, 0, 2);
    utassert(str::StartsWith(body, StrL("client=gtx&sl=en&tl=zh-TW&dt=t&q=")));
    // separators and form metacharacters are percent-encoded
    utassert(str::Contains(body, StrL("%0A%0A")));
    utassert(!str::Contains(body, StrL("A&B")));
}

void GoogleFreeTranslate_UnitTests() {
    ParseTest();
    BatchTest();
    BodyTest();
}
