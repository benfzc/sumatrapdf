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
#include "base/Pixmap.h"
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
// All translations of a page are scaled from their source font size by one
// factor, so headings, body text and tables keep their relative sizes. It
// starts where body text is this size (at 96 dpi)...
constexpr int kStartBodyPx = 13;
// ...and shrinks a step at a time until this share of paragraphs fit, but body
// text never gets smaller than kMinBodyPx
constexpr int kFitPercent = 70;
constexpr float kShrinkStep = 0.95f;
constexpr int kMinBodyPx = 11;
// smallest font a size computation may produce
constexpr int kMinFontPx = 8;
// zoom at most this high for the page image searched for blank space
constexpr float kMaxMapZoom = 2.0f;
// space kept between a translation and whatever is below it
constexpr int kGapPx = 2;
// the box hiding the English is this much larger on every side
constexpr int kCoverPadPx = 1;
// sum of RGB differences still treated as the page background
constexpr int kBlankTolerance = 24;
// a translation may grow into blank space this many font sizes right and down
constexpr int kMaxGrowEm = 3;
constexpr int kStatusMargin = 6;
constexpr int kStatusFontPx = 12;

constexpr COLORREF kStatusColor = RGB(0x80, 0x80, 0x80);

// Translations of a page as laid out at one zoom. Rectangles are screen
// pixels relative to the page's top-left corner.
struct BvLayout {
    bool valid = false;
    float zoom = 0;
    int rotation = 0;
    // English to hide: the paragraph and the space its translation takes
    Vec<Rect> covers;
    Vec<Rect> boxes;
    Vec<int> fontPx;
    Vec<bool> bold;
    StrVec texts;
    // the whole translation of a text cut with an ellipsis, else empty
    StrVec fullTexts;
};

// paragraphs of one page, extracted once
struct BvPage {
    int pageNo = 0;
    PageParagraphs paras;
    // what is sent for translation: empty for paragraphs kept as they are
    // (commands and code in a monospaced font)
    StrVec toTranslate;
    BvLayout layout;
};

// one service for the whole process: one cache file, one rate limit
static TranslationService* gService = nullptr;
// the document the service and gPages belong to
static Str gDocPath;
static Vec<BvPage*> gPages;

// translations cut with an ellipsis in the last paint: hovering one shows it
// in full
static MainWindow* gHoverWin = nullptr;
static Vec<Rect> gHoverRects;
static StrVec gHoverTexts;

static void ResetHover(MainWindow* win) {
    gHoverWin = win;
    VecReset(gHoverRects);
    gHoverTexts.Reset();
}

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
    for (int i = 0; i < len(p->paras.texts); i++) {
        p->toTranslate.Append(p->paras.mono[i] ? Str() : p->paras.texts.At(i));
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
    ResetHover(nullptr);
    str::FreePtr(&gDocPath);
}

// shows the full translation of an ellipsis-cut paragraph under the mouse
bool BilingualViewOnSetCursor(MainWindow* win, Point pt) {
    if (!BilingualViewIsOn(win) || win != gHoverWin) {
        return false;
    }
    for (int i = 0; i < len(gHoverRects); i++) {
        Rect rc = gHoverRects[i];
        if (!rc.Contains(pt)) {
            continue;
        }
        win->ShowToolTip(gHoverTexts.At(i), rc, true);
        SetCursorCached(IDC_ARROW);
        return true;
    }
    return false;
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

// Copy of the left half (the document as rendered), source of the mirror pages
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

static COLORREF PixelColor(u32 v) {
    return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

static COLORREF SnapshotPixel(const LeftSnapshot& snap, int x, int y) {
    return PixelColor(snap.bits[y * snap.dx + x]);
}

// The whole page rendered off-screen, searched for blank space. Unlike the
// screen it covers the parts scrolled out of view, so a page is laid out once.
struct PageMap {
    Vec<u32> bits; // top-down 0xAARRGGBB rows of dx pixels
    int dx = 0;
    int dy = 0;
    COLORREF bg = 0;
};

static bool RenderPageMap(DisplayModel* dm, int pageNo, float zoom, PageMap* map) {
    RenderPageArgs args(pageNo, zoom, dm->GetRotation());
    Pixmap* px = PixmapToBgra(dm->GetEngine()->RenderPage(args));
    if (!px) {
        return false;
    }
    map->dx = px->width;
    map->dy = px->height;
    VecResize(map->bits, map->dx * map->dy);
    for (int y = 0; y < map->dy; y++) {
        memcpy(map->bits.els + y * map->dx, px->data + y * px->stride, map->dx * sizeof(u32));
    }
    FreePixmap(px);
    map->bg = map->dx > 0 && map->dy > 0 ? PixelColor(map->bits[0]) : RGB(0xff, 0xff, 0xff);
    return map->dx > 0 && map->dy > 0;
}

static bool IsNearColor(COLORREF a, COLORREF b) {
    int d = abs((int)GetRValue(a) - (int)GetRValue(b)) + abs((int)GetGValue(a) - (int)GetGValue(b)) +
            abs((int)GetBValue(a) - (int)GetBValue(b));
    return d <= kBlankTolerance;
}

static bool IsBlank(const PageMap& map, int x, int y) {
    return IsNearColor(PixelColor(map.bits[y * map.dx + x]), map.bg);
}

static bool RowBlank(const PageMap& map, int y, int x0, int x1) {
    for (int x = x0; x < x1; x++) {
        if (!IsBlank(map, x, y)) {
            return false;
        }
    }
    return true;
}

static bool ColBlank(const PageMap& map, int x, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        if (!IsBlank(map, x, y)) {
            return false;
        }
    }
    return true;
}

// Blank area right of and below a paragraph that its translation may use:
// grows while the page stays blank, so it stops at other text, table lines,
// figures and translations already placed (they are marked in the map).
// Never left or up, so translations keep the paragraph's left edge and top.
static Rect FreeRectAround(const PageMap& map, Rect rc, int fontPx) {
    rc = rc.Intersect(Rect(0, 0, map.dx, map.dy));
    if (rc.IsEmpty()) {
        return rc;
    }
    int maxRight = std::max(rc.dx / 2, fontPx * kMaxGrowEm);
    int maxDown = std::max(rc.dy * 2, fontPx * kMaxGrowEm);

    int x1 = rc.x + rc.dx;
    int xEnd = std::min(map.dx, x1 + maxRight);
    while (x1 < xEnd && ColBlank(map, x1, rc.y, rc.y + rc.dy)) {
        x1++;
    }
    if (x1 < xEnd) {
        x1 = std::max(x1 - kGapPx, rc.x + rc.dx);
    }

    int y1 = rc.y + rc.dy;
    int yEnd = std::min(map.dy, y1 + maxDown);
    while (y1 < yEnd && RowBlank(map, y1, rc.x, x1)) {
        y1++;
    }
    if (y1 < yEnd) {
        y1 = std::max(y1 - kGapPx, rc.y + rc.dy);
    }
    return Rect(rc.x, rc.y, x1 - rc.x, y1 - rc.y);
}

// claim an area of the page so later translations don't grow into it: paint
// it in a color far from the page background
static void MarkUsed(PageMap& map, Rect rc) {
    int luma = GetRValue(map.bg) + GetGValue(map.bg) + GetBValue(map.bg);
    u32 mark = luma > 3 * 128 ? 0x000000 : 0xffffff;
    rc = rc.Intersect(Rect(0, 0, map.dx, map.dy));
    for (int y = rc.y; y < rc.y + rc.dy; y++) {
        u32* row = map.bits.els + y * map.dx;
        for (int x = rc.x; x < rc.x + rc.dx; x++) {
            row[x] = mark;
        }
    }
}

// Box of a translation at the paragraph's left edge and top: the paragraph's
// own width if that fits within the blank space below, else widened right.
static bool PlaceText(HDC hdc, WStr ws, Rect rc, Rect free, int px, FontWeight weight, Rect* out) {
    int maxDy = free.y + free.dy - rc.y;
    int widths[] = {rc.dx, free.x + free.dx - rc.x};
    for (int dx : widths) {
        if (dx <= 0 || maxDy <= 0) {
            continue;
        }
        int textDy = TextHeight(hdc, ws, dx, px, weight);
        if (textDy <= maxDy) {
            *out = Rect(rc.x, rc.y, dx, textDy);
            return true;
        }
    }
    return false;
}

// translated paragraphs of a page, ready to lay out
struct LayoutInput {
    // screen pixels relative to the page's top-left
    Vec<Rect> boxes;
    // the same in page map pixels
    Vec<Rect> mapBoxes;
    // source font size in screen pixels
    Vec<float> srcPx;
    Vec<bool> bold;
    // Traditional Chinese, in the temp arena
    Vec<WStr> texts;
    // screen pixels per page map pixel
    float scale = 1;
};

enum class PassMode {
    Measure,
    Commit,
};

static Rect MapToScreen(Rect rc, float scale) {
    int x0 = (int)((float)rc.x * scale);
    int y0 = (int)((float)rc.y * scale);
    int x1 = (int)((float)(rc.x + rc.dx) * scale);
    int y1 = (int)((float)(rc.y + rc.dy) * scale);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

static Rect ScreenToMap(Rect rc, float scale) {
    int x0 = (int)floorf((float)rc.x / scale);
    int y0 = (int)floorf((float)rc.y / scale);
    int x1 = (int)ceilf((float)(rc.x + rc.dx) / scale);
    int y1 = (int)ceilf((float)(rc.y + rc.dy) / scale);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

// Places every paragraph with font sizes scaled by fontScale, in reading
// order, each taking the blank space the ones before it left. Returns how
// many fit without an ellipsis. Commit also cuts the rest and fills out.
static int LayoutPass(HDC hdc, const LayoutInput& in, const PageMap& srcMap, float fontScale, PassMode mode,
                      BvLayout* out) {
    PageMap map = srcMap;
    int nFit = 0;
    for (int i = 0; i < len(in.boxes); i++) {
        Rect rc = in.boxes[i];
        int px = limitValue((int)(in.srcPx[i] * fontScale + 0.5f), kMinFontPx, kMaxFontPx);
        FontWeight weight = in.bold[i] ? FontWeight::Bold : FontWeight::Normal;

        // blank space found in the map, in screen pixels; at least the paragraph
        Rect free = MapToScreen(FreeRectAround(map, in.mapBoxes[i], (int)((float)px / in.scale)), in.scale);
        free = Rect::FromXY(rc.x, rc.y, std::max(free.x + free.dx, rc.x + rc.dx),
                            std::max(free.y + free.dy, rc.y + rc.dy));

        WStr ws = in.texts[i];
        Rect place;
        bool fits = PlaceText(hdc, ws, rc, free, px, weight, &place);
        if (fits) {
            nFit++;
        } else {
            place = free;
        }
        Rect used = rc.Union(place);
        MarkUsed(map, ScreenToMap(used, in.scale));
        if (mode == PassMode::Measure) {
            continue;
        }

        TempWStr shown = fits ? ws : TruncateToFitTemp(hdc, ws, place.dx, place.dy, px, weight);
        VecAppend(out->covers, used);
        VecAppend(out->boxes, place);
        VecAppend(out->fontPx, px);
        VecAppend(out->bold, in.bold[i]);
        out->texts.Append(ToUtf8Temp(shown));
        out->fullTexts.Append(fits ? Str() : ToUtf8Temp(ws));
    }
    return nFit;
}

// font size of most of the text: the one with the most characters
static float BodyFontPx(const LayoutInput& in) {
    float best = 0;
    int bestChars = -1;
    for (int i = 0; i < len(in.srcPx); i++) {
        int chars = 0;
        for (int j = 0; j < len(in.srcPx); j++) {
            if (fabsf(in.srcPx[j] - in.srcPx[i]) < 0.5f) {
                chars += len(in.texts[j]);
            }
        }
        if (chars > bestChars) {
            bestChars = chars;
            best = in.srcPx[i];
        }
    }
    return best;
}

static void ResetLayout(BvLayout* l) {
    VecReset(l->covers);
    VecReset(l->boxes);
    VecReset(l->fontPx);
    VecReset(l->bold);
    l->texts.Reset();
    l->fullTexts.Reset();
    l->valid = false;
}

// Lays out a whole page at once: one font scale for all its translations,
// the largest at which kFitPercent of them fit (body text kMinBodyPx at least).
//
//   source sizes   heading 16  body 12  table 11   (screen px)
//   start          heading 17  body 13  table 12   (body at kStartBodyPx)
//   6 of 20 don't fit: shrink every size by kShrinkStep and try again
static void LayoutPage(HDC hdc, DisplayModel* dm, BvPage* p, PageInfo* pi) {
    BvLayout* l = &p->layout;
    ResetLayout(l);
    int pageNo = p->pageNo;
    l->zoom = dm->GetZoomReal(pageNo);
    l->rotation = dm->GetRotation();
    l->valid = true;

    if (l->zoom <= 0) {
        return;
    }
    float mapZoom = std::min(l->zoom, kMaxMapZoom);
    PageMap map;
    if (!RenderPageMap(dm, pageNo, mapZoom, &map)) {
        return;
    }

    LayoutInput in;
    in.scale = l->zoom / mapZoom;
    EngineBase* engine = dm->GetEngine();
    PointF mapOrigin = engine->Transform(dm->PageMediaBoxForLayout(pageNo), pageNo, mapZoom, l->rotation).TL();
    Point pageOrigin = pi->pageOnScreen.TL();
    const PageParagraphs& paras = p->paras;
    for (int i = 0; i < len(paras.boxes); i++) {
        TempStr text;
        // skipped (values, register names) and failed paragraphs keep the
        // original text
        if (TrServiceGet(gService, pageNo, i, &text) != TrState::Done) {
            continue;
        }
        Rect rc = dm->CvtToScreen(pageNo, paras.boxes[i]);
        rc.Offset(-pageOrigin.x, -pageOrigin.y);
        RectF mapRc = engine->Transform(paras.boxes[i], pageNo, mapZoom, l->rotation);
        mapRc.Offset(-mapOrigin.x, -mapOrigin.y);

        VecAppend(in.boxes, rc);
        VecAppend(in.mapBoxes, mapRc.Round());
        VecAppend(in.srcPx, paras.fontSizes[i] * l->zoom);
        VecAppend(in.bold, paras.bold[i]);
        VecAppend(in.texts, (WStr)ToTraditionalTemp(ToWStrTemp(text)));
    }
    int n = len(in.boxes);
    if (n == 0) {
        return;
    }

    float body = std::max(BodyFontPx(in), 1.0f);
    float scale = std::max(1.0f, (float)DpiScale(kStartBodyPx) / body);
    float minScale = std::min(scale, (float)DpiScale(kMinBodyPx) / body);
    while (scale > minScale) {
        int nFit = LayoutPass(hdc, in, map, scale, PassMode::Measure, nullptr);
        if (nFit * 100 >= n * kFitPercent) {
            break;
        }
        scale = std::max(minScale, scale * kShrinkStep);
    }
    LayoutPass(hdc, in, map, scale, PassMode::Commit, l);
}

static bool PageTranslated(BvPage* p) {
    for (int i = 0; i < len(p->paras.boxes); i++) {
        if (TrServiceGet(gService, p->pageNo, i, nullptr) == TrState::Pending) {
            return false;
        }
    }
    return true;
}

// The mirror page is the rendered page itself, so figures, tables and
// diagrams show as they are. Once all of a page is translated, its
// translated paragraphs are replaced, all at once.
static void PaintMirrorPage(HDC hdc, const LeftSnapshot& snap, DisplayModel* dm, PageInfo* pi, int pageNo,
                            COLORREF txtCol) {
    int dx = snap.dx;
    Rect visible = pi->pageOnScreen.Intersect(Rect(0, 0, snap.dx, snap.dy));
    if (visible.IsEmpty()) {
        return;
    }
    BitBlt(hdc, visible.x + dx, visible.y, visible.dx, visible.dy, snap.dc, visible.x, visible.y, SRCCOPY);
    COLORREF pageBg = SnapshotPixel(snap, visible.x + 1, visible.y + 1);

    BvPage* p = GetPage(dm, pageNo);
    if (!PageTranslated(p)) {
        return;
    }
    BvLayout* l = &p->layout;
    bool stale = !l->valid || l->zoom != dm->GetZoomReal(pageNo) || l->rotation != dm->GetRotation();
    if (stale) {
        LayoutPage(hdc, dm, p, pi);
    }

    // layout rectangles are relative to the page; the mirror is dx to the right
    Point org(pi->pageOnScreen.x + dx, pi->pageOnScreen.y);
    HBRUSH brush = CreateSolidBrush(pageBg);
    SetTextColor(hdc, txtCol);
    for (int i = 0; i < len(l->boxes); i++) {
        Rect cover = l->covers[i];
        cover.Offset(org.x, org.y);
        cover.Inflate(kCoverPadPx, kCoverPadPx);
        RECT coverR = ToRECT(cover);
        FillRect(hdc, &coverR, brush);

        Rect box = l->boxes[i];
        box.Offset(org.x, org.y);
        FontWeight weight = l->bold[i] ? FontWeight::Bold : FontWeight::Normal;
        HGDIOBJ prevFont = SelectObject(hdc, TranslationFont(l->fontPx[i], weight));
        TempWStr ws = ToWStrTemp(l->texts.At(i));
        RECT r = ToRECT(Rect(box.x, box.y, box.dx, box.dy + 1));
        DrawTextW(hdc, ws.s, len(ws), &r, kTextFlags);
        SelectObject(hdc, prevFont);

        if (len(l->fullTexts.At(i)) > 0) {
            VecAppend(gHoverRects, box);
            gHoverTexts.Append(l->fullTexts.At(i));
        }
    }
    DeleteObject(brush);
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
    Rect rightRc(dx, 0, win->canvasRc.dx - dx, dy);
    RECT right = ToRECT(rightRc);
    HBRUSH brush = CreateSolidBrush(bgCol);
    FillRect(hdc, &right, brush);
    DeleteObject(brush);

    // a page scrolled sideways must not spill into the left half
    int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, right.left, right.top, right.right, right.bottom);
    SetBkMode(hdc, TRANSPARENT);
    ResetHover(win);
    TrServiceNewGeneration(gService);
    int lastVisible = 0;
    for (int pageNo = 1; pageNo <= dm->PageCount(); pageNo++) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || pi->visibleRatio == 0.0f) {
            continue;
        }
        lastVisible = pageNo;
        TrServiceRequest(gService, pageNo, GetPage(dm, pageNo)->toTranslate, TrPriority::Visible);
        PaintMirrorPage(hdc, snap, dm, pi, pageNo, txtCol);
    }
    for (int pageNo = lastVisible + 1; pageNo <= lastVisible + kPrefetchPages && pageNo <= dm->PageCount(); pageNo++) {
        TrServiceRequest(gService, pageNo, GetPage(dm, pageNo)->toTranslate, TrPriority::Prefetch);
    }

    TempStr status = StatusTextTemp();
    if (len(status) > 0) {
        int margin = DpiScale(kStatusMargin);
        DrawStatus(hdc, status, Rect(dx + margin, margin, dx - 2 * margin, DpiScale(kStatusFontPx) * 3));
    }
    RestoreDC(hdc, savedDc);
    FreeSnapshot(&snap);
}
