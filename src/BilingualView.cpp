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
// smallest font tried before the text is clipped
constexpr int kMinFontPx = 8;
// a translation's first font size: the source line height times this
constexpr float kFontToLineHeight = 0.8f;
constexpr int kStatusMargin = 6;

constexpr COLORREF kPendingColor = RGB(0x99, 0x99, 0x99);
constexpr COLORREF kStatusColor = RGB(0x80, 0x80, 0x80);

// text blocks of one page, extracted once
struct BvPage {
    int pageNo = 0;
    Vec<RectF> boxes;
    Vec<int> nLines;
    StrVec paragraphs;
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
    PageTextBlocks blocks;
    if (dm->GetEngine()->ExtractTextBlocks(pageNo, &blocks)) {
        int n = len(blocks.boxes);
        for (int i = 0; i < n; i++) {
            int start = blocks.firstLine[i];
            int end = i + 1 < n ? blocks.firstLine[i + 1] : len(blocks.lines);
            StrVec lines;
            for (int l = start; l < end; l++) {
                lines.Append(blocks.lines.At(l));
            }
            VecAppend(p->boxes, blocks.boxes[i]);
            VecAppend(p->nLines, end - start);
            p->paragraphs.Append(JoinParagraphLinesTemp(lines));
        }
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
// fonts by pixel size, created on first use, kept for the process lifetime
static HFONT gFonts[kMaxFontPx + 1];

static HFONT TranslationFont(int px) {
    if (!gFonts[px]) {
        gFonts[px] = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFontName);
    }
    return gFonts[px];
}

// Draws text word-wrapped inside rc with the largest font size that fits,
// starting at startPx and going down to kMinFontPx; below that it's clipped.
static void DrawFittedText(HDC hdc, Str text, Rect rc, int startPx, COLORREF col) {
    TempWStr ws = ToWStrTemp(text);
    RECT r = ToRECT(rc);
    UINT flags = DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL;

    int px = limitValue(startPx, kMinFontPx, kMaxFontPx);
    HGDIOBJ prev = SelectObject(hdc, TranslationFont(px));
    for (; px > kMinFontPx; px--) {
        SelectObject(hdc, TranslationFont(px));
        RECT measure = r;
        DrawTextW(hdc, ws.s, len(ws), &measure, flags | DT_CALCRECT);
        if (measure.bottom - measure.top <= rc.dy) {
            break;
        }
    }

    SelectObject(hdc, TranslationFont(px));
    SetTextColor(hdc, col);
    DrawTextW(hdc, ws.s, len(ws), &r, flags);
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

static void PaintMirrorPage(HDC hdc, DisplayModel* dm, PageInfo* pi, int pageNo, int dx, Color bgCol, Color txtCol) {
    Rect pageRc = pi->pageOnScreen;
    pageRc.x += dx;
    HBRUSH brush = CreateSolidBrush(bgCol);
    RECT pageR = ToRECT(pageRc);
    FillRect(hdc, &pageR, brush);
    DeleteObject(brush);

    BvPage* p = GetPage(dm, pageNo);
    for (int i = 0; i < len(p->boxes); i++) {
        Rect rc = dm->CvtToScreen(pageNo, p->boxes[i]);
        rc.x += dx;
        int lineDy = rc.dy / (p->nLines[i] > 0 ? p->nLines[i] : 1);
        int startPx = (int)((float)lineDy * kFontToLineHeight);

        TempStr text;
        TrState state = TrServiceGet(gService, pageNo, i, &text);
        switch (state) {
            case TrState::Done:
                DrawFittedText(hdc, text, rc, startPx, txtCol);
                break;
            case TrState::Skipped:
                DrawFittedText(hdc, text, rc, startPx, txtCol);
                break;
            case TrState::Failed:
                DrawFittedText(hdc, p->paragraphs.At(i), rc, startPx, kPendingColor);
                break;
            case TrState::Pending:
                DrawFittedText(hdc, StrL("..."), rc, startPx, kPendingColor);
                break;
        }
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
    int oldBkMode = SetBkMode(hdc, TRANSPARENT);

    TrServiceNewGeneration(gService);
    int lastVisible = 0;
    for (int pageNo = 1; pageNo <= dm->PageCount(); pageNo++) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || pi->visibleRatio == 0.0f) {
            continue;
        }
        lastVisible = pageNo;
        TrServiceRequest(gService, pageNo, GetPage(dm, pageNo)->paragraphs, TrPriority::Visible);
        PaintMirrorPage(hdc, dm, pi, pageNo, dx, bgCol, txtCol);
    }
    for (int pageNo = lastVisible + 1; pageNo <= lastVisible + kPrefetchPages && pageNo <= dm->PageCount(); pageNo++) {
        TrServiceRequest(gService, pageNo, GetPage(dm, pageNo)->paragraphs, TrPriority::Prefetch);
    }

    TempStr status = StatusTextTemp();
    if (len(status) > 0) {
        Rect rc(dx + kStatusMargin, kStatusMargin, dx - 2 * kStatusMargin, 40);
        DrawFittedText(hdc, status, rc, 14, kStatusColor);
    }
    SetBkMode(hdc, oldBkMode);
}
