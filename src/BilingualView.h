/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

bool BilingualViewIsOn(MainWindow* win);
void BilingualViewToggle(MainWindow* win);
void BilingualViewPaint(MainWindow* win, HDC hdc);
void BilingualViewShutdown();
bool BilingualViewOnSetCursor(MainWindow* win, Point pt);
