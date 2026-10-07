/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Translates the paragraphs of document pages in the background.
//
//   UI (bilingual pane)
//     │ TrServiceRequest(page, paragraphs, priority)    TrServiceGet(page, i)
//     ▼                                                 ▲
//   TranslationService ── worker thread: TrServiceStep() in a loop
//     │  pick page (Visible before Prefetch) → cache hits → batch the rest
//     │  → RateLimiter → post → GtxParse → cache + results → onPageDone
//     ▼
//   TrPostFn (HTTP)       TranslationCache (disk)
//
// No engine or UI code here: the caller extracts paragraphs and repaints.
// Network, clock and sleep are injected so tests run without either.

enum class TrState : u8 {
    Pending,
    Done,
    // nothing to translate (register names, values); show the source
    Skipped,
    Failed,
};

enum class TrPriority : u8 {
    Visible,
    Prefetch,
};

// HTTP POST of a form body; returns the HTTP status, or -1 if no response
using TrPostFn = int (*)(Str url, Str body, str::Builder& reply);
using TrNowFn = i64 (*)();
using TrSleepFn = void (*)(int ms);

struct TrServiceConfig {
    Str srcLang = StrL("en");
    Str dstLang = StrL("zh-TW");
    int maxCharsPerRequest = 5000;
    int perMinute = 20;
    int perHour = 300;
    // empty: no disk cache
    Str cacheDir;

    TrPostFn post = nullptr;
    TrNowFn now = nullptr;
    TrSleepFn sleep = nullptr;
    // called on the worker thread when a page has new results
    Func1<int> onPageDone;
};

struct TrServiceStatus {
    int nPendingParagraphs = 0;
    RateState rate = RateState::Ready;
    i64 waitMs = 0;
};

struct TranslationService;

TranslationService* TrServiceCreate(const TrServiceConfig& cfg);
void TrServiceStart(TranslationService* svc);
void TrServiceDelete(TranslationService* svc);

void TrServiceNewGeneration(TranslationService* svc);
void TrServiceClear(TranslationService* svc);
void TrServiceRequest(TranslationService* svc, int pageNo, const StrVec& paragraphs, TrPriority prio);
TrState TrServiceGet(TranslationService* svc, int pageNo, int idx, TempStr* textOut);
TrServiceStatus TrServiceGetStatus(TranslationService* svc);
void TrServiceResume(TranslationService* svc);

bool TrServiceStep(TranslationService* svc);

#if IS_DEBUG
void TranslationService_UnitTests();
#endif
