@agents.md

# Fork 專屬說明（benfzc/sumatrapdf）

上面引入的 `agents.md` 是上游專案的開發規則，照樣遵守。本檔補充這個 fork 的規則；兩者衝突時以本檔為準。

## 溝通

- 使用台灣繁體中文回覆；專業術語用英文。
- 程式碼註解一律用英文。
- 設計軟體時先討論架構，使用者明確說「提供程式」才開始寫程式。
- 問指令或語法時直接給答案；解釋新概念時附實際範例。

## 寫文件的原則

適用於所有文件（HTML、Markdown、設計文件、commit 以外的說明文字）：

1. 由淺入深：先講是什麼、能做什麼，再講原理與細節。
2. 重點在前：開頭先給結論或摘要，細節放後面。
3. 專業術語用英文，例如 Workflow、Runner、bbox、rate limit。
4. 只寫目前的正確內容，不寫更正或修改的歷史紀錄。
5. 每次修改後重新 review 全文，確認沒有前後矛盾或重複的段落。

文件放在 `docs/`，檔名以 `-zh.html` 結尾。

## 建置

- 開發者電腦沒有 Visual Studio。Windows 版的編譯與 unit tests 交給 GitHub Actions（`windows-2025-vs2026` runner）執行。
- 上游的 `.github/workflows/build.yml`（`-ci` 模式）在本 fork 的 push 上會建置 Win32 版並跑 debug unit tests，不上傳任何東西，約 9 分鐘。
- 本 fork 的 `.github/workflows/fork-build.yml` 補上 x64：debug unit tests、release 建置，並把 `SumatraPDF.exe` 上傳成 artifact。
- Google 免費翻譯端點會封鎖雲端 IP（回 429 加 HTML "Sorry" 頁），所以 CI 和雲端容器裡的測試不可連線，改用存下來的回應當 fixture。
- 不依賴 Win32 的模組（放在 `src/shared`、`src/base`）先在 Linux 用 g++ 編譯並跑 unit tests，再 push 給 CI。

## 進行中的功能：PDF 中英對照（鏡像頁）

已定案的設計決策：

- 對照模式啟用時，畫布平分左右兩半：左邊原文，右邊「鏡像頁」。鏡像頁直接套用 `DisplayModel` 的頁面座標，每段譯文畫在原文段落的 bbox 位置；用原生 Direct2D/DirectWrite 繪製。
- 段落來源：在 `EngineBase` 新增取 block 的介面，由 `EngineMupdf` 用 `fz_stext_block` 實作。EPUB/FB2 預設也走 `EngineMupdf`，所以一併支援。
- 翻譯 provider：先只做 Google 免費端點，付費 API 之後再加；透過 provider 介面抽象。
- 限速：每個 provider 一個 token bucket；遇到 429 做指數退避冷卻。
- 快取：全域、只追加的檔案，放在 `GetAppDataDirTemp()` 下的 `translations\`；key 為 hash(正規化段落文字 + 目標語言 + provider)。
- 預設目標語言 `zh-TW`。
- 實作順序：步驟 0 新增 fork 專用的 CI workflow 並驗證建置；接著用 bash + curl 做 Google 端點的 spike；最後寫 C++（P1）。
