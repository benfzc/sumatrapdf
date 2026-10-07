/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Request building and response parsing for the free Google Translate
// endpoint (translate.googleapis.com, client=gtx). No networking here: the
// caller POSTs GtxBodyTemp() to kGtxUrl and hands the reply to GtxParse().
//
// Several paragraphs go in one request, separated by a blank line; Google
// keeps the separators, so the reply splits back into the same paragraphs:
//
//   q = "Para one.\n\nPara two."
//   reply [[["第一段。\n\n第二段。", "Para one.\n\nPara two.", ...], ...], ...]

extern const Str kGtxUrl;

enum class GtxStatus {
    Ok,
    // throttled or captcha page: back off (RateLimiterBlocked)
    Blocked,
    // HTTP error other than throttling
    Failed,
    // unexpected JSON, or paragraph count differs: retry one by one
    BadResponse,
};

int GtxBatchEnd(const StrVec& paragraphs, int start, int maxChars);
TempStr GtxBodyTemp(Str srcLang, Str dstLang, const StrVec& paragraphs, int start, int end);
GtxStatus GtxParse(int httpStatus, Str body, int nExpected, StrVec& out);

#if IS_DEBUG
void GoogleFreeTranslate_UnitTests();
#endif
