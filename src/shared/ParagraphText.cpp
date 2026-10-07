/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "ParagraphText.h"

// shortest run of letters that counts as an ordinary word ("the", "PLL" is not)
constexpr int kMinWordLen = 3;
// a piece continues the paragraph above it if the vertical gap is below this
// many font sizes: line spacing in a paragraph, not cell padding in a table
constexpr float kMaxLineGapEm = 0.6f;

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

static bool OverlapsX(const RectF& a, const RectF& b) {
    return a.x < b.x + b.dx && b.x < a.x + a.dx;
}

// Groups line pieces into paragraphs. A piece joins an open paragraph of the
// same layout block that it horizontally overlaps and sits right below;
// otherwise it starts a new one. Table cells (pieces side by side) thus stay
// apart, while the lines of one cell are joined:
//
//   | PeriphID | Serial engine peripheral |   -> "PeriphID", "Serial engine
//   |          | to configure.            |       peripheral to configure."
//   | Mode     | Macro of FIFO modes.     |   -> "Mode", "Macro of FIFO modes."
void GroupParagraphs(const PageTextLines& lines, PageParagraphs* out) {
    int n = len(lines.texts);
    Vec<int> paraOf; // paragraph index of each piece
    Vec<int> paraBlock;
    for (int i = 0; i < n; i++) {
        const RectF& box = lines.boxes[i];
        float maxGap = lines.fontSizes[i] * kMaxLineGapEm;

        int found = -1;
        for (int p = len(out->boxes) - 1; p >= 0 && found < 0; p--) {
            if (paraBlock[p] != lines.blocks[i]) {
                break;
            }
            const RectF& pb = out->boxes[p];
            float gap = box.y - (pb.y + pb.dy);
            if (OverlapsX(box, pb) && gap >= -maxGap && gap <= maxGap) {
                found = p;
            }
        }

        if (found < 0) {
            VecAppend(paraBlock, lines.blocks[i]);
            VecAppend(out->boxes, box);
            VecAppend(out->fontSizes, lines.fontSizes[i]);
            VecAppend(out->bold, lines.bold[i]);
            out->texts.Append(Str());
            VecAppend(paraOf, len(out->boxes) - 1);
            continue;
        }
        out->boxes[found] = out->boxes[found].Union(box);
        if (lines.fontSizes[i] > out->fontSizes[found]) {
            out->fontSizes[found] = lines.fontSizes[i];
        }
        out->bold[found] = out->bold[found] && lines.bold[i];
        VecAppend(paraOf, found);
    }

    // join each paragraph's lines in reading order
    for (int p = 0; p < len(out->boxes); p++) {
        StrVec paraLines;
        for (int i = 0; i < n; i++) {
            if (paraOf[i] == p) {
                paraLines.Append(lines.texts.At(i));
            }
        }
        out->texts.SetAt(p, JoinParagraphLinesTemp(paraLines));
    }
}
