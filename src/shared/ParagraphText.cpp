/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "ParagraphText.h"

// shortest run of letters that counts as an ordinary word ("the", "PLL" is not)
constexpr int kMinWordLen = 3;

static bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool IsLower(char c) {
    return c >= 'a' && c <= 'z';
}

static bool IsUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

static bool IsLetter(char c) {
    return IsLower(c) || IsUpper(c);
}

static Str TrimSpace(Str s) {
    int start = 0;
    int end = s.len;
    while (start < end && IsSpace(s.s[start])) {
        start++;
    }
    while (end > start && IsSpace(s.s[end - 1])) {
        end--;
    }
    return Str(s.s + start, end - start);
}

// append s, collapsing whitespace runs into one space
static void AppendCollapsed(str::Builder& b, Str s) {
    bool lastWasSpace = len(b) > 0 && b.LastChar() == ' ';
    for (int i = 0; i < s.len; i++) {
        char c = s.s[i];
        if (IsSpace(c)) {
            if (!lastWasSpace) {
                b.AppendChar(' ');
            }
            lastWasSpace = true;
            continue;
        }
        b.AppendChar(c);
        lastWasSpace = false;
    }
}

// How to glue the end of one line to the start of the next:
//   "regis-" + "ter"        -> "register"     (word broken by hyphenation)
//   "memory-" + "Mapped"    -> "memory-Mapped" (a real hyphen, keep it)
//   "16-" + "bit"           -> "16-bit"
//   "shown" + "in"          -> "shown in"
TempStr JoinParagraphLinesTemp(const StrVec& lines) {
    str::Builder b;
    for (Str raw : lines) {
        Str line = TrimSpace(raw);
        if (len(line) == 0) {
            continue;
        }

        int n = len(b);
        bool endsWithHyphen = n > 0 && b.LastChar() == '-';
        if (endsWithHyphen) {
            bool brokenWord = n >= 2 && IsLower(b[n - 2]) && IsLower(line.s[0]);
            if (brokenWord) {
                b.RemoveLast();
            }
        } else if (n > 0) {
            b.AppendChar(' ');
        }
        AppendCollapsed(b, line);
    }
    return ToStrTemp(b);
}

// an ordinary word: letters only, at least kMinWordLen of them, lower case
// except maybe the first ("Reset", "the"; not "PLL", "kHz", "GPIOA_MODER")
static bool IsOrdinaryWord(Str w) {
    if (len(w) < kMinWordLen) {
        return false;
    }
    for (int i = 0; i < w.len; i++) {
        char c = w.s[i];
        if (!IsLetter(c)) {
            return false;
        }
        if (i > 0 && !IsLower(c)) {
            return false;
        }
    }
    return true;
}

static bool IsWordPunct(char c) {
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '(' || c == ')' || c == '"' || c == '!' || c == '?';
}

ParagraphKind ClassifyParagraph(Str text) {
    int i = 0;
    while (i < text.len) {
        while (i < text.len && IsSpace(text.s[i])) {
            i++;
        }
        int start = i;
        while (i < text.len && !IsSpace(text.s[i])) {
            i++;
        }

        // "shown," -> "shown"
        int s = start;
        int e = i;
        while (s < e && IsWordPunct(text.s[s])) {
            s++;
        }
        while (e > s && IsWordPunct(text.s[e - 1])) {
            e--;
        }
        if (IsOrdinaryWord(Str(text.s + s, e - s))) {
            return ParagraphKind::Translate;
        }
    }
    return ParagraphKind::Skip;
}
