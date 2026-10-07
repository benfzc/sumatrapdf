/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/JsonParser.h"

#include "RateLimiter.h"
#include "TranslationService.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

constexpr i64 kSecMs = 1000;

enum class FakeMode {
    Translate,
    Throttle,
    // joins the paragraphs without the blank-line separators
    MergeParagraphs,
};

// fake Google: translates "x" to "ZH:x" for each paragraph of the request
static FakeMode gFakeMode = FakeMode::Translate;
static int gNPosts = 0;
static i64 gNowMs = 0;
static Vec<int> gDonePages;

static TempStr QueryFromBodyTemp(Str body) {
    int i = str::IndexOf(body, StrL("&q="));
    return url::DecodeTemp(Str(body.s + i + 3, body.len - i - 3));
}

static int FakePost(Str, Str body, str::Builder& reply) {
    gNPosts++;
    if (gFakeMode == FakeMode::Throttle) {
        reply.Append(StrL("<html><title>Sorry...</title></html>"));
        return 429;
    }

    StrVec paras;
    Split(&paras, QueryFromBodyTemp(body), StrL("\n\n"));
    str::Builder dst;
    for (int i = 0; i < len(paras); i++) {
        if (i > 0 && gFakeMode == FakeMode::Translate) {
            dst.Append(StrL("\n\n"));
        }
        dst.Append(StrL("ZH:"));
        dst.Append(paras.At(i));
    }
    reply.Append(fmt("[[[\"%s\",\"src\",null,null,3]],null,\"en\"]", json::EscapeStrTemp(ToStrTemp(dst))));
    return 200;
}

static i64 FakeNow() {
    return gNowMs;
}

static void FakeSleep(int ms) {
    gNowMs += ms;
}

static void OnPageDone(int pageNo) {
    VecAppend(gDonePages, pageNo);
}

static TrServiceConfig FakeConfig(Str cacheDir) {
    TrServiceConfig cfg;
    cfg.cacheDir = cacheDir;
    cfg.post = FakePost;
    cfg.now = FakeNow;
    cfg.sleep = FakeSleep;
    cfg.onPageDone = MkFunc1Void(OnPageDone);
    return cfg;
}

static void ResetFake() {
    gFakeMode = FakeMode::Translate;
    gNPosts = 0;
    gNowMs = 1000 * 60 * kSecMs;
    VecReset(gDonePages);
}

static StrVec Paras(Str a, Str b = {}, Str c = {}) {
    StrVec v;
    Str all[] = {a, b, c};
    for (Str s : all) {
        if (len(s) > 0) {
            v.Append(s);
        }
    }
    return v;
}

static TempStr Get(TranslationService* svc, int pageNo, int idx, TrState expected) {
    TempStr text;
    utassert(TrServiceGet(svc, pageNo, idx, &text) == expected);
    return text;
}

static void BatchAndSkipTest() {
    ResetFake();
    TranslationService* svc = TrServiceCreate(FakeConfig({}));

    // the register name is skipped, the other two go in one request
    TrServiceRequest(svc, 1, Paras(StrL("The PLL must be locked."), StrL("GPIOA_MODER"), StrL("Reset the device.")),
                     TrPriority::Visible);
    Get(svc, 1, 0, TrState::Pending);
    utassert(str::Eq(Get(svc, 1, 1, TrState::Skipped), StrL("GPIOA_MODER")));

    utassert(TrServiceStep(svc));
    utassert(gNPosts == 1);
    utassert(str::Eq(Get(svc, 1, 0, TrState::Done), StrL("ZH:The PLL must be locked.")));
    utassert(str::Eq(Get(svc, 1, 2, TrState::Done), StrL("ZH:Reset the device.")));
    utassert(len(gDonePages) == 1 && gDonePages[0] == 1);

    // nothing left
    utassert(!TrServiceStep(svc));
    utassert(gNPosts == 1);
    TrServiceDelete(svc);
}

static void PriorityTest() {
    ResetFake();
    TranslationService* svc = TrServiceCreate(FakeConfig({}));

    TrServiceRequest(svc, 5, Paras(StrL("Prefetched page text.")), TrPriority::Prefetch);
    TrServiceRequest(svc, 2, Paras(StrL("Visible page text.")), TrPriority::Visible);
    utassert(TrServiceStep(svc));
    Get(svc, 2, 0, TrState::Done);
    Get(svc, 5, 0, TrState::Pending);

    // the view moved: page 5 was not requested again, so it waits
    TrServiceNewGeneration(svc);
    utassert(!TrServiceStep(svc));
    TrServiceRequest(svc, 5, Paras(StrL("Prefetched page text.")), TrPriority::Prefetch);
    utassert(TrServiceStep(svc));
    Get(svc, 5, 0, TrState::Done);
    TrServiceDelete(svc);
}

static void ThrottleTest() {
    ResetFake();
    TranslationService* svc = TrServiceCreate(FakeConfig({}));
    TrServiceRequest(svc, 1, Paras(StrL("The clock is stopped.")), TrPriority::Visible);

    gFakeMode = FakeMode::Throttle;
    utassert(TrServiceStep(svc));
    Get(svc, 1, 0, TrState::Pending);
    TrServiceStatus st = TrServiceGetStatus(svc);
    utassert(st.rate == RateState::Wait);
    utassert(st.waitMs == 30 * kSecMs);
    utassert(st.nPendingParagraphs == 1);

    // cooling down: no request
    utassert(!TrServiceStep(svc));
    utassert(gNPosts == 1);

    gFakeMode = FakeMode::Translate;
    gNowMs += 30 * kSecMs;
    utassert(TrServiceStep(svc));
    Get(svc, 1, 0, TrState::Done);
    TrServiceDelete(svc);
}

static void SplitMismatchTest() {
    ResetFake();
    TranslationService* svc = TrServiceCreate(FakeConfig({}));
    TrServiceRequest(svc, 1, Paras(StrL("First paragraph here."), StrL("Second paragraph here.")), TrPriority::Visible);

    // a batch reply that doesn't split back: retried one paragraph at a time
    gFakeMode = FakeMode::MergeParagraphs;
    utassert(TrServiceStep(svc));
    Get(svc, 1, 0, TrState::Pending);
    utassert(TrServiceStep(svc));
    utassert(TrServiceStep(svc));
    utassert(gNPosts == 3);
    utassert(str::Eq(Get(svc, 1, 0, TrState::Done), StrL("ZH:First paragraph here.")));
    utassert(str::Eq(Get(svc, 1, 1, TrState::Done), StrL("ZH:Second paragraph here.")));
    TrServiceDelete(svc);
}

static void BudgetTest() {
    ResetFake();
    TrServiceConfig cfg = FakeConfig({});
    cfg.maxCharsPerRequest = 30;
    TranslationService* svc = TrServiceCreate(cfg);
    // 21 + 2 + 22 chars > 30: two requests
    TrServiceRequest(svc, 1, Paras(StrL("First paragraph here."), StrL("Second paragraph here.")), TrPriority::Visible);
    utassert(TrServiceStep(svc));
    Get(svc, 1, 0, TrState::Done);
    Get(svc, 1, 1, TrState::Pending);
    utassert(TrServiceStep(svc));
    Get(svc, 1, 1, TrState::Done);
    utassert(gNPosts == 2);
    TrServiceDelete(svc);
}

static void CacheTest() {
    ResetFake();
    Str dir = str::Dup(GetTempFilePathTemp(StrL("trs")));
    file::Delete(dir);

    TranslationService* svc = TrServiceCreate(FakeConfig(dir));
    TrServiceRequest(svc, 1, Paras(StrL("The device enters sleep mode.")), TrPriority::Visible);
    utassert(TrServiceStep(svc));
    TrServiceDelete(svc);

    // reopened: served from the cache without a request
    svc = TrServiceCreate(FakeConfig(dir));
    TrServiceRequest(svc, 9, Paras(StrL("The device enters sleep mode.")), TrPriority::Visible);
    utassert(str::Eq(Get(svc, 9, 0, TrState::Done), StrL("ZH:The device enters sleep mode.")));
    utassert(!TrServiceStep(svc));
    utassert(gNPosts == 1);
    TrServiceDelete(svc);

    utassert(dir::RemoveAll(dir));
    str::Free(dir);
}

void TranslationService_UnitTests() {
    BatchAndSkipTest();
    PriorityTest();
    ThrottleTest();
    SplitMismatchTest();
    BudgetTest();
    CacheTest();
}
