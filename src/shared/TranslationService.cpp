/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/AppendStore.h"
#include "base/Dict.h"

#include "RateLimiter.h"
#include "TranslationCache.h"
#include "ParagraphText.h"
#include "GoogleFreeTranslate.h"
#include "TranslationService.h"

static const Str kProviderName = StrL("google-free");

// while rate-limited the worker sleeps in slices so it notices shutdown
constexpr int kSleepSliceMs = 250;

struct TrPage {
    int pageNo = 0;
    u32 generation = 0;
    TrPriority prio = TrPriority::Prefetch;
    StrVec sources;
    StrVec results;
    Vec<TrState> states;
    // set after a batch reply didn't split back: send one by one
    bool oneByOne = false;
};

struct TranslationService {
    TrServiceConfig cfg;
    TranslationCache cache;
    bool hasCache = false;

    // guards everything below
    Mutex mu;
    ConditionVariable workAvailable;
    Vec<TrPage*> pages;
    u32 generation = 0;
    RateLimiter limiter;
    bool stop = false;

    ThreadHandle thread = nullptr;
};

TranslationService* TrServiceCreate(const TrServiceConfig& cfg) {
    auto svc = new TranslationService();
    svc->cfg = cfg;
    svc->cfg.srcLang = str::Dup(cfg.srcLang);
    svc->cfg.dstLang = str::Dup(cfg.dstLang);
    svc->cfg.cacheDir = str::Dup(cfg.cacheDir);
    svc->limiter.perMinute = cfg.perMinute;
    svc->limiter.perHour = cfg.perHour;
    if (len(cfg.cacheDir) > 0 && dir::CreateAll(cfg.cacheDir)) {
        svc->hasCache = TranslationCacheOpen(&svc->cache, svc->cfg.cacheDir);
    }
    return svc;
}

static TrPage* PickPageLocked(TranslationService* svc);

static void WorkerLoop(TranslationService* svc) {
    while (true) {
        svc->mu.Lock();
        while (!svc->stop && !PickPageLocked(svc)) {
            svc->workAvailable.Wait(&svc->mu);
        }
        bool stop = svc->stop;
        svc->mu.Unlock();
        if (stop) {
            return;
        }
        if (!TrServiceStep(svc)) {
            svc->cfg.sleep(kSleepSliceMs);
        }
    }
}

// no-op without threads (wasm): the owner calls TrServiceStep() instead
void TrServiceStart(TranslationService* svc) {
    if (!kHasThreads) {
        return;
    }
    svc->thread = StartThread(MkFunc0(WorkerLoop, svc), StrL("TranslationService"));
}

void TrServiceDelete(TranslationService* svc) {
    if (!svc) {
        return;
    }
    svc->mu.Lock();
    svc->stop = true;
    svc->workAvailable.WakeAll();
    svc->mu.Unlock();
    if (svc->thread) {
        JoinThread(&svc->thread, -1);
    }

    for (TrPage* p : svc->pages) {
        delete p;
    }
    if (svc->hasCache) {
        TranslationCacheClose(&svc->cache);
    }
    str::Free(svc->cfg.srcLang);
    str::Free(svc->cfg.dstLang);
    str::Free(svc->cfg.cacheDir);
    delete svc;
}

static TrPage* FindPage(TranslationService* svc, int pageNo) {
    for (TrPage* p : svc->pages) {
        if (p->pageNo == pageNo) {
            return p;
        }
    }
    return nullptr;
}

static bool HasPending(TrPage* p) {
    for (TrState s : p->states) {
        if (s == TrState::Pending) {
            return true;
        }
    }
    return false;
}

// Visible pages first, then the oldest request. Prefetch requests from an
// older generation wait until requested again. Caller holds mu.
static TrPage* PickPageLocked(TranslationService* svc) {
    TrPage* best = nullptr;
    for (TrPage* p : svc->pages) {
        bool stale = p->prio == TrPriority::Prefetch && p->generation != svc->generation;
        if (stale || !HasPending(p)) {
            continue;
        }
        if (!best || (p->prio == TrPriority::Visible && best->prio != TrPriority::Visible)) {
            best = p;
        }
    }
    return best;
}

// called when the view moves: prefetch requests from before are dropped
// unless requested again
void TrServiceNewGeneration(TranslationService* svc) {
    AutoUnlockMutex lock(&svc->mu);
    svc->generation++;
}

// Resolves what it can right away (cache hits, paragraphs not worth
// translating) and queues the rest. Re-requesting a known page only
// refreshes its priority and generation.
// forget all pages, e.g. another document was opened. A batch already sent
// still lands in the cache.
void TrServiceClear(TranslationService* svc) {
    AutoUnlockMutex lock(&svc->mu);
    for (TrPage* p : svc->pages) {
        delete p;
    }
    VecReset(svc->pages);
}

void TrServiceRequest(TranslationService* svc, int pageNo, const StrVec& paragraphs, TrPriority prio) {
    AutoUnlockMutex lock(&svc->mu);
    TrPage* p = FindPage(svc, pageNo);
    if (p) {
        p->prio = prio;
        p->generation = svc->generation;
        svc->workAvailable.Wake();
        return;
    }

    p = new TrPage();
    p->pageNo = pageNo;
    p->prio = prio;
    p->generation = svc->generation;
    for (Str src : paragraphs) {
        p->sources.Append(src);
        if (ClassifyParagraph(src) == ParagraphKind::Skip) {
            p->results.Append(src);
            VecAppend(p->states, TrState::Skipped);
            continue;
        }

        Str cached;
        if (svc->hasCache) {
            cached = TranslationCacheGet(&svc->cache, TranslationKeyTemp(kProviderName, svc->cfg.dstLang, src));
        }
        p->results.Append(cached);
        VecAppend(p->states, len(cached) > 0 ? TrState::Done : TrState::Pending);
    }
    VecAppend(svc->pages, p);
    svc->workAvailable.Wake();
}

TrState TrServiceGet(TranslationService* svc, int pageNo, int idx, TempStr* textOut) {
    AutoUnlockMutex lock(&svc->mu);
    TrPage* p = FindPage(svc, pageNo);
    if (!p || idx < 0 || idx >= len(p->states)) {
        return TrState::Pending;
    }
    if (textOut) {
        *textOut = str::DupTemp(p->results.At(idx));
    }
    return p->states[idx];
}

TrServiceStatus TrServiceGetStatus(TranslationService* svc) {
    AutoUnlockMutex lock(&svc->mu);
    TrServiceStatus st;
    for (TrPage* p : svc->pages) {
        for (TrState s : p->states) {
            st.nPendingParagraphs += s == TrState::Pending ? 1 : 0;
        }
    }
    RateCheck rc = RateLimiterCheck(&svc->limiter, svc->cfg.now());
    st.rate = rc.state;
    st.waitMs = rc.waitMs;
    return st;
}

void TrServiceResume(TranslationService* svc) {
    AutoUnlockMutex lock(&svc->mu);
    RateLimiterResume(&svc->limiter);
    svc->workAvailable.Wake();
}

struct TrBatch {
    int pageNo = 0;
    Vec<int> idx;
    StrVec texts;
};

// next run of pending paragraphs of a page that fits one request. Caller holds mu.
static void FillBatchLocked(TranslationService* svc, TrPage* p, TrBatch* b) {
    b->pageNo = p->pageNo;
    StrVec pending;
    Vec<int> pendingIdx;
    for (int i = 0; i < len(p->states); i++) {
        if (p->states[i] == TrState::Pending) {
            pending.Append(p->sources.At(i));
            VecAppend(pendingIdx, i);
        }
    }

    // budget 0 still takes one paragraph
    int budget = p->oneByOne ? 0 : svc->cfg.maxCharsPerRequest;
    int n = GtxBatchEnd(pending, 0, budget);
    for (int i = 0; i < n; i++) {
        VecAppend(b->idx, pendingIdx[i]);
        b->texts.Append(pending.At(i));
    }
}

static void StoreResultsLocked(TranslationService* svc, const TrBatch& b, const StrVec& out) {
    TrPage* p = FindPage(svc, b.pageNo);
    for (int i = 0; i < len(b.idx); i++) {
        Str src = b.texts.At(i);
        Str dst = out.At(i);
        if (svc->hasCache) {
            TranslationCachePut(&svc->cache, kProviderName, svc->cfg.dstLang, src, dst);
        }
        if (!p) {
            continue;
        }
        p->results.SetAt(b.idx[i], dst);
        p->states[b.idx[i]] = TrState::Done;
    }
}

static void MarkFailedLocked(TranslationService* svc, const TrBatch& b) {
    TrPage* p = FindPage(svc, b.pageNo);
    if (!p) {
        return;
    }
    for (int i : b.idx) {
        p->states[i] = TrState::Failed;
    }
}

// Sends one batch. Returns false when there was nothing it could do now
// (no work, rate-limited, paused) so the worker sleeps before retrying.
bool TrServiceStep(TranslationService* svc) {
    TrBatch b;
    {
        AutoUnlockMutex lock(&svc->mu);
        RateCheck rc = RateLimiterCheck(&svc->limiter, svc->cfg.now());
        if (rc.state != RateState::Ready) {
            return false;
        }
        TrPage* p = PickPageLocked(svc);
        if (!p) {
            return false;
        }
        FillBatchLocked(svc, p, &b);
        RateLimiterSent(&svc->limiter, svc->cfg.now());
    }

    // network call without holding the lock
    TempStr body = GtxBodyTemp(svc->cfg.srcLang, svc->cfg.dstLang, b.texts, 0, len(b.texts));
    str::Builder reply;
    int status = svc->cfg.post(kGtxUrl, body, reply);
    StrVec out;
    GtxStatus res = GtxParse(status, ToStrTemp(reply), len(b.texts), out);

    AutoUnlockMutex lock(&svc->mu);
    switch (res) {
        case GtxStatus::Ok:
            RateLimiterSucceeded(&svc->limiter);
            StoreResultsLocked(svc, b, out);
            break;
        case GtxStatus::Blocked:
            // paragraphs stay Pending and are retried after the cooldown
            RateLimiterBlocked(&svc->limiter, svc->cfg.now(), 0);
            return true;
        case GtxStatus::BadResponse:
            if (len(b.idx) > 1) {
                TrPage* p = FindPage(svc, b.pageNo);
                if (p) {
                    p->oneByOne = true;
                }
                return true;
            }
            MarkFailedLocked(svc, b);
            break;
        case GtxStatus::Failed:
            MarkFailedLocked(svc, b);
            break;
    }
    if (svc->cfg.onPageDone.IsValid()) {
        svc->cfg.onPageDone.Call(b.pageNo);
    }
    return true;
}
