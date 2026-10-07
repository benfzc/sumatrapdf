/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/AppendStore.h"
#include "base/Crypto.h"
#include "base/Dict.h"

#include "TranslationCache.h"

// ASCII unit separator: never part of normal text
constexpr char kFieldSep = '\x1f';
constexpr int kPayloadFields = 4;

static const Str kRecordKind = StrL("tr");
static const Str kIndexFileName = StrL("cache.txt");
static const Str kDataFileName = StrL("cache.bin");

TempStr TranslationKeyTemp(Str provider, Str lang, Str source) {
    str::Builder b;
    b.Append(provider);
    b.AppendChar(kFieldSep);
    b.Append(lang);
    b.AppendChar(kFieldSep);
    b.Append(source);

    u8 digest[20];
    CalcSHA1Digest(ToStrTemp(b), digest);
    return str::MemToHexTemp(Str((char*)digest, (int)sizeof(digest)));
}

// remember or replace the translation for key
static void SetTranslation(TranslationCache* c, Str key, Str translation) {
    int idx;
    if (c->keyToIdx.Get(key, &idx)) {
        c->translations.SetAt(idx, translation);
        return;
    }
    idx = len(c->translations);
    c->translations.Append(translation);
    c->keyToIdx.Insert(key, idx);
}

// payload "provider \x1f lang \x1f source \x1f translation" -> translation
static Str TranslationFromPayload(Str payload) {
    int nSep = 0;
    for (int i = 0; i < payload.len; i++) {
        if (payload.s[i] != kFieldSep) {
            continue;
        }
        nSep++;
        if (nSep == kPayloadFields - 1) {
            return Str(payload.s + i + 1, payload.len - i - 1);
        }
    }
    return {};
}

static void OnRecordLoaded(AppendStoreRecord* rec, Str, void* userData) {
    auto c = (TranslationCache*)userData;
    if (str::Eq(rec->kind, kRecordKind)) {
        VecAppend(c->loaded, rec);
    }
}

bool TranslationCacheOpen(TranslationCache* c, Str dir) {
    c->store.dataDir = dir;
    c->store.indexFileName = kIndexFileName;
    c->store.dataFileName = kDataFileName;
    c->store.onRecord = OnRecordLoaded;
    c->store.userData = c;
    if (!AppendStoreOpen(&c->store)) {
        return false;
    }

    // replay; payloads live in the data file
    for (AppendStoreRecord* rec : c->loaded) {
        Str payload = AppendStoreReadPayload(&c->store, rec);
        Str translation = TranslationFromPayload(payload);
        if (len(translation) > 0) {
            SetTranslation(c, rec->meta, translation);
        }
        str::Free(payload);
    }
    VecReset(c->loaded);

    // records appended from now on are already in memory
    c->store.onRecord = nullptr;
    return true;
}

void TranslationCacheClose(TranslationCache* c) {
    AppendStoreClose(&c->store);
}

Str TranslationCacheGet(TranslationCache* c, Str key) {
    int idx;
    if (!c->keyToIdx.Get(key, &idx)) {
        return {};
    }
    return c->translations.At(idx);
}

bool TranslationCachePut(TranslationCache* c, Str provider, Str lang, Str source, Str translation) {
    TempStr key = TranslationKeyTemp(provider, lang, source);

    str::Builder payload;
    Str fields[] = {provider, lang, source, translation};
    for (int i = 0; i < kPayloadFields; i++) {
        if (i > 0) {
            payload.AppendChar(kFieldSep);
        }
        payload.Append(fields[i]);
    }

    AppendStoreAppendOptions opts;
    opts.kind = kRecordKind;
    opts.meta = key;
    opts.data = ToStrTemp(payload);
    if (!AppendStoreAppend(&c->store, opts)) {
        return false;
    }
    SetTranslation(c, key, translation);
    return true;
}

int TranslationCacheCount(TranslationCache* c) {
    return len(c->translations);
}
