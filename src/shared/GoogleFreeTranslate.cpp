/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/JsonParser.h"

#include "GoogleFreeTranslate.h"

const Str kGtxUrl = StrL("https://translate.googleapis.com/translate_a/single");

constexpr int kHttpOk = 200;
constexpr int kHttpTooManyRequests = 429;

static const Str kParaSep = StrL("\n\n");

// Index one past the last paragraph that fits in maxChars, counting the
// separators. Always takes at least one, so an oversized paragraph goes alone.
int GtxBatchEnd(const StrVec& paragraphs, int start, int maxChars) {
    int n = len(paragraphs);
    int total = 0;
    int end = start;
    while (end < n) {
        int add = len(paragraphs.At(end)) + (end > start ? len(kParaSep) : 0);
        if (end > start && total + add > maxChars) {
            break;
        }
        total += add;
        end++;
    }
    return end;
}

// form body for paragraphs[start, end): client=gtx&sl=en&tl=zh-TW&dt=t&q=...
TempStr GtxBodyTemp(Str srcLang, Str dstLang, const StrVec& paragraphs, int start, int end) {
    str::Builder q;
    for (int i = start; i < end; i++) {
        if (i > start) {
            q.Append(kParaSep);
        }
        q.Append(paragraphs.At(i));
    }

    TempStr qEnc = url::EncodeTemp(ToStrTemp(q));
    return fmt("client=gtx&sl=%s&tl=%s&dt=t&q=%s", srcLang, dstLang, qEnc);
}

// translated text is the first string of every segment: [0][i][0]
static void OnGtxValue(str::Builder* b, json::Value* v) {
    if (v->type != json::Type::String) {
        return;
    }
    if (json::PathMatch(v->path, StrL("i0"), StrL("*"), StrL("i0"))) {
        b->Append(v->value);
    }
}

static bool IsBlockedPage(Str body) {
    // when throttling, Google serves an HTML "Sorry..." captcha page
    return str::StartsWith(body, StrL("<")) && str::Contains(body, StrL("Sorry"));
}

GtxStatus GtxParse(int httpStatus, Str body, int nExpected, StrVec& out) {
    if (httpStatus == kHttpTooManyRequests || IsBlockedPage(body)) {
        return GtxStatus::Blocked;
    }
    if (httpStatus != kHttpOk) {
        return GtxStatus::Failed;
    }

    str::Builder joined;
    bool ok = json::Parse(body, MkFunc1(OnGtxValue, &joined));
    if (!ok || len(joined) == 0) {
        return GtxStatus::BadResponse;
    }

    StrVec parts;
    Split(&parts, ToStrTemp(joined), kParaSep);
    if (len(parts) != nExpected) {
        return GtxStatus::BadResponse;
    }
    for (Str p : parts) {
        TempStr t = str::DupTemp(p);
        str::TrimWSInPlace(t, str::TrimOpt::Both);
        out.Append(t);
    }
    return GtxStatus::Ok;
}
