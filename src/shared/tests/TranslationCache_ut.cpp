/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/AppendStore.h"
#include "base/Dict.h"

#include "TranslationCache.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

static const Str kProvider = StrL("google-free");
static const Str kLang = StrL("zh-TW");

static Str Lookup(TranslationCache* c, Str source) {
    return TranslationCacheGet(c, TranslationKeyTemp(kProvider, kLang, source));
}

void TranslationCache_UnitTests() {
    // key depends on every part
    Str src = StrL("The PLL must be locked.");
    TempStr key = TranslationKeyTemp(kProvider, kLang, src);
    utassert(len(key) == 40);
    utassert(!str::Eq(key, TranslationKeyTemp(kProvider, StrL("ja"), src)));
    utassert(!str::Eq(key, TranslationKeyTemp(StrL("deepl"), kLang, src)));

    Str dir = str::Dup(GetTempFilePathTemp(StrL("trc")));
    file::Delete(dir);
    utassert(dir::Create(dir));

    {
        TranslationCache c;
        utassert(TranslationCacheOpen(&c, dir));
        utassert(TranslationCacheCount(&c) == 0);
        utassert(len(Lookup(&c, src)) == 0);

        // a translation with a newline and the field separator's neighbors
        utassert(TranslationCachePut(&c, kProvider, kLang, src, StrL("PLL 必須鎖定。\n第二行")));
        utassert(TranslationCachePut(&c, kProvider, kLang, StrL("Reset"), StrL("重置")));
        utassert(str::Eq(Lookup(&c, src), StrL("PLL 必須鎖定。\n第二行")));

        // a later put for the same key replaces the old one
        utassert(TranslationCachePut(&c, kProvider, kLang, StrL("Reset"), StrL("重設")));
        utassert(TranslationCacheCount(&c) == 2);
        utassert(str::Eq(Lookup(&c, StrL("Reset")), StrL("重設")));
        TranslationCacheClose(&c);
    }

    // reopening replays the records, the latest one wins
    {
        TranslationCache c;
        utassert(TranslationCacheOpen(&c, dir));
        utassert(TranslationCacheCount(&c) == 2);
        utassert(str::Eq(Lookup(&c, src), StrL("PLL 必須鎖定。\n第二行")));
        utassert(str::Eq(Lookup(&c, StrL("Reset")), StrL("重設")));
        TranslationCacheClose(&c);
    }

    utassert(dir::RemoveAll(dir));
    str::Free(dir);
}
