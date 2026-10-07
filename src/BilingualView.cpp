/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Bilingual view: the canvas is split in two. The left half shows the
// document as usual (DisplayModel's viewport is half the canvas width, see
// MainWindow::GetViewPortSize()); the right half is a "mirror page" where the
// translation of each text block is drawn at the block's position.
//
//   ┌──────── canvas ─────────────────────────────┐
//   │  page (DisplayModel)  │  mirror page (here)  │
//   │  ┌─────┐  ┌─────┐     │  ┌─────┐  ┌─────┐    │
//   │  │Para1│  │Para3│     │  │段落1│  │段落3│    │
//   │  └─────┘  └─────┘     │  └─────┘  └─────┘    │
//   └───────────────────────────────────────────────┘
//
// Both halves scroll together because they are one window with one scroll
// position. Translations come from TranslationService on a worker thread;
// when a page is done it posts a repaint to the UI thread.

#include "base/Base.h"
#include "base/File.h"
#include "base/AppendStore.h"
#include "base/Dict.h"
#include "base/Http.h"
#include "base/UITask.h"
#include "base/Win.h"

#include "gui/UIModels.h"
#include "gui/Dpi.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "AppTools.h"
#include "Theme.h"
#include "MainWindow.h"
#include "RateLimiter.h"
#include "ParagraphText.h"
#include "TranslationService.h"
#include "BilingualView.h"

static const Str kCacheDirName = StrL("translations");
static const Str kFormContentType = StrL("application/x-www-form-urlencoded;charset=UTF-8");
static const WCHAR* kFontName = L"Microsoft JhengHei UI";

// pages after the last visible one translated ahead of time
constexpr int kPrefetchPages = 2;
// Chinese is denser than the Latin text it replaces: it gets at least this
// size (at 96 dpi) and may take the blank space below its paragraph
constexpr int kMinReadablePx = 13;
// smallest font a size computation may produce
constexpr int kMinFontPx = 8;
// a translation's first font size: the source font size times this
constexpr float kFontScale = 1.0f;
// space kept between a translation and whatever is below it
constexpr int kGapPx = 2;
// the box hiding the English is this much larger on every side
constexpr int kCoverPadPx = 1;
// sum of RGB differences still treated as the page background
constexpr int kBlankTolerance = 24;
// a translation may grow to this many times its paragraph's height
constexpr int kMaxSpillFactor = 3;
constexpr int kStatusMargin = 6;
constexpr int kStatusFontPx = 12;

constexpr COLORREF kStatusColor = RGB(0x80, 0x80, 0x80);

// paragraphs of one page, extracted once
struct BvPage {
    int pageNo = 0;
    PageParagraphs paras;
};

// one service for the whole process: one cache file, one rate limit
static TranslationService* gService = nullptr;
// the document the service and gPages belong to
static Str gDocPath;
static Vec<BvPage*> gPages;

static int HttpPostForm(Str url, Str body, str::Builder& reply) {
    HttpRsp rsp;
    HttpPostUrl(url, kFormContentType, {}, body, &rsp);
    reply.Append(ToStrTemp(rsp.data));
    if (rsp.httpStatusCode == (DWORD)-1) {
        return -1;
    }
    return (int)rsp.httpStatusCode;
}

static i64 NowMs() {
    return UnixTimeMsNow();
}

static void RepaintBilingualWindows() {
    for (MainWindow* win : gWindows) {
        if (win->bilingualView) {
            HwndInvalidate(win->hwndCanvas);
        }
    }
}

// worker thread: hop to the UI thread to repaint
static void OnPageDone(int) {
    uitask::Post(MkFunc0Void(RepaintBilingualWindows));
}

static void FreePages() {
    for (BvPage* p : gPages) {
        delete p;
    }
    VecReset(gPages);
}

static void EnsureService() {
    if (gService) {
        return;
    }
    TrServiceConfig cfg;
    cfg.cacheDir = path::JoinTemp(GetAppDataDirTemp(), kCacheDirName);
    cfg.post = HttpPostForm;
    cfg.now = NowMs;
    cfg.sleep = SleepInMs;
    cfg.onPageDone = MkFunc1Void(OnPageDone);
    gService = TrServiceCreate(cfg);
    TrServiceStart(gService);
}

// switching to another document drops the old pages and queued requests
static void BindToDocument(DisplayModel* dm) {
    Str path = dm->GetEngine()->FilePath();
    if (str::Eq(path, gDocPath)) {
        return;
    }
    str::ReplaceWithCopy(&gDocPath, path);
    FreePages();
    if (gService) {
        TrServiceClear(gService);
    }
}

static BvPage* GetPage(DisplayModel* dm, int pageNo) {
    for (BvPage* p : gPages) {
        if (p->pageNo == pageNo) {
            return p;
        }
    }

    auto p = new BvPage();
    p->pageNo = pageNo;
    PageTextLines lines;
    if (dm->GetEngine()->ExtractTextLines(pageNo, &lines)) {
        GroupParagraphs(lines, &p->paras);
    }
    VecAppend(gPages, p);
    return p;
}

bool BilingualViewIsOn(MainWindow* win) {
    return win && win->bilingualView;
}

void BilingualViewToggle(MainWindow* win) {
    if (!win->AsFixed()) {
        return;
    }
    win->bilingualView = !win->bilingualView;
    if (win->bilingualView) {
        EnsureService();
        // turning it on again is also how the user resumes after a pause
        TrServiceResume(gService);
    }

    // the viewport width changes: half the canvas or all of it
    win->ctrl->SetViewPortSize(win->GetViewPortSize());
    win->RedrawAll(true);
}

void BilingualViewShutdown() {
    TrServiceDelete(gService);
    gService = nullptr;
    FreePages();
    str::FreePtr(&gDocPath);
}

constexpr int kMaxFontPx = 64;

enum class FontWeight {
    Normal,
    Bold,
};

// fonts by weight and pixel size, created on first use, kept for the process
// lifetime
static HFONT gFonts[2][kMaxFontPx + 1];

static HFONT TranslationFont(int px, FontWeight weight = FontWeight::Normal) {
    px = limitValue(px, kMinFontPx, kMaxFontPx);
    int w = weight == FontWeight::Bold ? 1 : 0;
    if (!gFonts[w][px]) {
        int fw = weight == FontWeight::Bold ? FW_BOLD : FW_NORMAL;
        gFonts[w][px] = CreateFontW(-px, 0, 0, 0, fw, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFontName);
    }
    return gFonts[w][px];
}

// Google sometimes skips its zh -> zh-Hant step and returns Simplified
// characters ("电源" instead of "電源"); map them per character
static TempWStr ToTraditionalTemp(WStr s) {
    int n = LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, s.s, len(s), nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) {
        return s;
    }
    WCHAR* buf = AllocArrayTemp<WCHAR>(n + 1);
    LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, s.s, len(s), buf, n, nullptr, nullptr, 0);
    return WStr(buf, n);
}

constexpr UINT kTextFlags = DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL;

static int TextHeight(HDC hdc, WStr ws, int dx, int px, FontWeight weight) {
    SelectObject(hdc, TranslationFont(px, weight));
    RECT r{0, 0, dx, 0};
    DrawTextW(hdc, ws.s, len(ws), &r, kTextFlags | DT_CALCRECT);
    return r.bottom - r.top;
}

// longest prefix of ws that, with an ellipsis, fits in dx x maxDy
static TempWStr TruncateToFitTemp(HDC hdc, WStr ws, int dx, int maxDy, int px, FontWeight weight) {
    int lo = 0;
    int hi = len(ws);
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        TempWStr cand = str::JoinTemp(WStr(ws.s, mid), WStrL(L"…"));
        if (TextHeight(hdc, cand, dx, px, weight) <= maxDy) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return str::JoinTemp(WStr(ws.s, lo), WStrL(L"…"));
}

static void DrawStatus(HDC hdc, Str text, Rect rc) {
    TempWStr ws = ToWStrTemp(text);
    RECT r = ToRECT(rc);
    HGDIOBJ prev = SelectObject(hdc, TranslationFont(DpiScale(kStatusFontPx)));
    SetTextColor(hdc, kStatusColor);
    DrawTextW(hdc, ws.s, len(ws), &r, kTextFlags);
    SelectObject(hdc, prev);
}

static TempStr StatusTextTemp() {
    TrServiceStatus st = TrServiceGetStatus(gService);
    if (st.rate == RateState::Paused) {
        return str::DupTemp(StrL("Google is refusing requests. Turn the bilingual view off and on to retry."));
    }
    if (st.rate == RateState::Wait && st.nPendingParagraphs > 0) {
        return fmt("Waiting %d s before the next request (%d paragraphs left)", (int)(st.waitMs / 1000),
                   st.nPendingParagraphs);
    }
    if (st.nPendingParagraphs > 0) {
        return fmt("Translating... %d paragraphs left", st.nPendingParagraphs);
    }
    return {};
}

// Copy of the left half (the document as rendered). The mirror pages are
// copied from it, and it tells where the page is blank below a paragraph.
struct LeftSnapshot {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ prevBmp = nullptr;
    // top-down 0xAARRGGBB rows of dx pixels
    u32* bits = nullptr;
    int dx = 0;
    int dy = 0;
};

static bool TakeSnapshot(HDC hdc, int dx, int dy, LeftSnapshot* snap) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = dx;
    bmi.bmiHeader.biHeight = -dy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    snap->bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!snap->bmp) {
        return false;
    }
    snap->dc = CreateCompatibleDC(hdc);
    snap->prevBmp = SelectObject(snap->dc, snap->bmp);
    BitBlt(snap->dc, 0, 0, dx, dy, hdc, 0, 0, SRCCOPY);
    GdiFlush();
    snap->bits = (u32*)bits;
    snap->dx = dx;
    snap->dy = dy;
    return true;
}

static void FreeSnapshot(LeftSnapshot* snap) {
    if (snap->dc) {
        SelectObject(snap->dc, snap->prevBmp);
        DeleteDC(snap->dc);
    }
    if (snap->bmp) {
        DeleteObject(snap->bmp);
    }
}

static COLORREF SnapshotPixel(const LeftSnapshot& snap, int x, int y) {
    u32 v = snap.bits[y * snap.dx + x];
    return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

static bool IsNearColor(COLORREF a, COLORREF b) {
    int d = abs((int)GetRValue(a) - (int)GetRValue(b)) + abs((int)GetGValue(a) - (int)GetGValue(b)) +
            abs((int)GetBValue(a) - (int)GetBValue(b));
    return d <= kBlankTolerance;
}

// how far down from rc's bottom the page stays blank across rc's width,
// stopping at text, a table line or a figure (or at maxY)
static int BlankBelow(const LeftSnapshot& snap, Rect rc, COLORREF pageBg, int maxY) {
    int x0 = std::max(rc.x, 0);
    int x1 = std::min(rc.x + rc.dx, snap.dx);
    int yEnd = std::min(maxY, snap.dy);
    int y = std::max(rc.y + rc.dy, 0);
    for (; y < yEnd; y++) {
        for (int x = x0; x < x1; x++) {
            if (!IsNearColor(SnapshotPixel(snap, x, y), pageBg)) {
                return std::max(y - kGapPx - (rc.y + rc.dy), 0);
            }
        }
    }
    return std::max(yEnd - (rc.y + rc.dy), 0);
}

// Replaces one paragraph's English with its translation. The translation may
// use the blank space below the paragraph, so Chinese can be drawn at a
// readable size; if it still doesn't fit it's cut with an ellipsis.
static void PaintTranslation(HDC hdc, const LeftSnapshot& snap, Str text, Rect rc, int fontPx, FontWeight weight,
                             int pageBottom, COLORREF pageBg, COLORREF txtCol) {
    if (rc.dx <= 0 || rc.dy <= 0) {
        return;
    }
    TempWStr ws = ToTraditionalTemp(ToWStrTemp(text));
    int spillLimit = std::min(pageBottom, rc.y + rc.dy * kMaxSpillFactor);
    int maxDy = rc.dy + BlankBelow(snap, rc, pageBg, spillLimit);

    // start at the source's own size (headings stay big), shrink to fit, but
    // not below a readable minimum
    int minPx = DpiScale(kMinReadablePx);
    int px = limitValue(std::max(fontPx, minPx), kMinFontPx, kMaxFontPx);
    HGDIOBJ prevFont = SelectObject(hdc, TranslationFont(px, weight));
    int textDy = TextHeight(hdc, ws, rc.dx, px, weight);
    while (textDy > maxDy && px > minPx) {
        px--;
        textDy = TextHeight(hdc, ws, rc.dx, px, weight);
    }
    if (textDy > maxDy) {
        ws = TruncateToFitTemp(hdc, ws, rc.dx, maxDy, px, weight);
        textDy = maxDy;
    }

    // hide the English it replaces, and the area the translation spills into
    int dx = snap.dx;
    Rect cover(rc.x + dx - kCoverPadPx, rc.y - kCoverPadPx, rc.dx + 2 * kCoverPadPx,
               std::max(rc.dy, textDy) + 2 * kCoverPadPx);
    RECT coverR = ToRECT(cover);
    HBRUSH brush = CreateSolidBrush(pageBg);
    FillRect(hdc, &coverR, brush);
    DeleteObject(brush);

    RECT r = ToRECT(Rect(rc.x + dx, rc.y, rc.dx, maxDy));
    SelectObject(hdc, TranslationFont(px, weight));
    SetTextColor(hdc, txtCol);
    DrawTextW(hdc, ws.s, len(ws), &r, kTextFlags);
    SelectObject(hdc, prevFont);
}

// The mirror page is the rendered page itself, so figures, tables and
// diagrams show as they are; only translated paragraphs are replaced.
static void PaintMirrorPage(HDC hdc, const LeftSnapshot& snap, DisplayModel* dm, PageInfo* pi, int pageNo,
                            COLORREF txtCol) {
    int dx = snap.dx;
    Rect visible = pi->pageOnScreen.Intersect(Rect(0, 0, snap.dx, snap.dy));
    if (visible.IsEmpty()) {
        return;
    }
    BitBlt(hdc, visible.x + dx, visible.y, visible.dx, visible.dy, snap.dc, visible.x, visible.y, SRCCOPY);
    COLORREF pageBg = SnapshotPixel(snap, visible.x + 1, visible.y + 1);
    int pageBottom = visible.y + visible.dy;

    BvPage* p = GetPage(dm, pageNo);
    const PageParagraphs& paras = p->paras;
    for (int i = 0; i < len(paras.boxes); i++) {
        TempStr text;
        // pending, skipped (values, register names) and failed paragraphs
        // keep the original text
        if (TrServiceGet(gService, pageNo, i, &text) != TrState::Done) {
            continue;
        }
        Rect rc = dm->CvtToScreen(pageNo, paras.boxes[i]);
        if (rc.Intersect(visible).IsEmpty()) {
            continue;
        }
        // the source font size in screen pixels at the current zoom
        RectF sizeBox = paras.boxes[i];
        sizeBox.dy = paras.fontSizes[i];
        int fontPx = (int)((float)dm->CvtToScreen(pageNo, sizeBox).dy * kFontScale);
        FontWeight weight = paras.bold[i] ? FontWeight::Bold : FontWeight::Normal;
        PaintTranslation(hdc, snap, text, rc, fontPx, weight, pageBottom, pageBg, txtCol);
    }
}

// Paints the right half of the canvas and queues translation of the visible
// pages (and a few after them).
void BilingualViewPaint(MainWindow* win, HDC hdc) {
    DisplayModel* dm = win->AsFixed();
    if (!win->bilingualView || !dm) {
        return;
    }
    EnsureService();
    BindToDocument(dm);

    Color bgCol;
    Color txtCol = ThemeDocumentColors(bgCol);
    int dx = dm->GetViewPort().dx;
    int dy = win->canvasRc.dy;
    LeftSnapshot snap;
    if (dx <= 0 || dy <= 0 || !TakeSnapshot(hdc, dx, dy, &snap)) {
        return;
    }

    // the right half: canvas background, also hiding a zoomed-in page that
    // spilled over from the left
    RECT right = ToRECT(Rect(dx, 0, win->canvasRc.dx - dx, dy));
    HBRUSH brush = CreateSolidBrush(bgCol);
    FillRect(hdc, &right, brush);
    DeleteObject(brush);

    int oldBkMode = SetBkMode(hdc, TRANSPARENT);
    TrServiceNewGeneration(gService);
    int lastVisible = 0;
    for (int pageNo = 1; pageNo <= dm->PageCount(); pageNo++) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || pi->visibleRatio == 0.0f) {
            continue;
        }
        lastVisible = pageNo;
        TrServiceRequest(gService, pageNo, GetPage(dm, pageNo)->paras.texts, TrPriority::Visible);
        PaintMirrorPage(hdc, snap, dm, pi, pageNo, txtCol);
    }
    for (int pageNo = lastVisible + 1; pageNo <= lastVisible + kPrefetchPages && pageNo <= dm->PageCount(); pageNo++) {
        TrServiceRequest(gService, pageNo, GetPage(dm, pageNo)->paras.texts, TrPriority::Prefetch);
    }

    TempStr status = StatusTextTemp();
    if (len(status) > 0) {
        int margin = DpiScale(kStatusMargin);
        DrawStatus(hdc, status, Rect(dx + margin, margin, dx - 2 * margin, DpiScale(kStatusFontPx) * 3));
    }
    SetBkMode(hdc, oldBkMode);
    FreeSnapshot(&snap);
}
