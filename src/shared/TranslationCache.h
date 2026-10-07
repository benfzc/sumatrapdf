/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Persistent cache of paragraph translations, shared by all documents.
//
// Keyed by the paragraph text, not by the file, so a translation is reused
// when the same paragraph shows up again: the document reopened, renamed or
// moved, or a new revision of the same datasheet.
//
//   key = hex(SHA1(provider \x1f lang \x1f normalized source text))
//
// On disk it's an AppendStore (cache.txt index + cache.bin payloads); every
// record is replayed into memory on open, a later record for the same key
// wins. Payload: provider \x1f lang \x1f source \x1f translation
// (the source is kept so the cache can be exported as bilingual text).
//
// Not thread-safe: callers serialize access. Appends from two processes at
// the same time are not coordinated.

struct TranslationCache {
    AppendStore store;
    dict::MapStrToInt keyToIdx{1024};
    // translations[idx], idx from keyToIdx
    StrVec translations;
    // records seen while opening, payloads are read after AppendStoreOpen()
    Vec<AppendStoreRecord*> loaded;
};

TempStr TranslationKeyTemp(Str provider, Str lang, Str source);

bool TranslationCacheOpen(TranslationCache* c, Str dir);
void TranslationCacheClose(TranslationCache* c);
// translation for key, empty if not cached. Owned by the cache.
Str TranslationCacheGet(TranslationCache* c, Str key);
bool TranslationCachePut(TranslationCache* c, Str provider, Str lang, Str source, Str translation);
int TranslationCacheCount(TranslationCache* c);

#if IS_DEBUG
void TranslationCache_UnitTests();
#endif
