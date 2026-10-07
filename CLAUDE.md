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
- 不依賴 Win32 的模組（放在 `src/shared`、`src/base`）先在 Linux 編譯並跑 unit tests，再 push 給 CI：`bun cmd/ng-build.ts -linux -dbg test_util -run -- -for-ai`（需先 `apt-get install libx11-dev libcairo2-dev libpango1.0-dev libgdk-pixbuf-2.0-dev libglib2.0-dev libssl-dev`）。
- 新增 `src/shared` 模組時要登記：`premake5.files.lua`、`vs2022/SumatraPDF*.vcxproj(.filters)`、`cmd/helper/ng-shared.ts`、`cmd/helper/ng-targets.ts`，以及 `src/tests/Sumatra_ut.cpp`、`src/ng/tests/Sumatra_ut.cpp` 的 unit test 呼叫。vcxproj 由 premake 產生，但 premake 只有 Windows 版，所以照既有項目的格式與排序手動加入。

## 進行中的功能：PDF 中英對照（鏡像頁）

已定案的設計決策：

- 對照模式啟用時，畫布平分左右兩半：左邊原文，右邊「鏡像頁」。鏡像頁直接套用 `DisplayModel` 的頁面座標，每段譯文畫在原文段落的 bbox 位置；用原生 Direct2D/DirectWrite 繪製。
- 段落來源：在 `EngineBase` 新增取 block 的介面，由 `EngineMupdf` 用 `fz_stext_block` 實作。EPUB/FB2 預設也走 `EngineMupdf`，所以一併支援。block 內各行由 `src/shared/ParagraphText` 合成段落並判斷是否跳過。
- 翻譯 provider：先只做 Google 翻譯的非官方免費端點（`translate.googleapis.com/translate_a/single?client=gtx`，不需 API key），付費 API 之後再加；透過 provider 介面抽象。
- 候選 provider：Google AI Studio 免費 API key（Gemini API）。和 gtx 端點的差異：

  | | gtx 端點（目前採用） | Gemini API（免費 key） |
  |---|---|---|
  | 性質 | 非公開端點，無文件，可能隨時改格式或封鎖 | 官方 API，有文件 |
  | 註冊 | 不需要 | Google 帳號 + API key |
  | 額度 | 未公開；超過回 429 + 封鎖頁，雲端 IP 直接被擋 | 公開的每分鐘、每日上限，依模型而定 |
  | 品質 | 一般機器翻譯；實測暫存器名稱、型號、單位都保留 | LLM，prompt 可帶術語表與不翻譯清單 |
  | 速度 | 快 | 較慢 |
  | 資料 | 無條款可依循 | 免費方案的輸入可能被用來改進 Google 產品；有 NDA 的文件要先確認 |
  | 測試 | 只能在使用者的網路測 | CI 可用存在 GitHub Secrets 的 key 做整合測試 |

  改用 Gemini 時要做的事：`RateLimiter` 加每日 window；新增 Gemini provider（一次送多段、要求固定格式回傳）；gtx 降為不需 key 的備用 provider。
- 限速：`src/shared/RateLimiter`，每分鐘與每小時兩個 sliding window；服務回 429 或封鎖頁時冷卻 30s→60s→120s→240s，連續第 4 次改為暫停，等使用者手動恢復。
- 快取：`src/shared/TranslationCache`，以 `AppendStore` 存在 `GetAppDataDirTemp()` 下的 `translations\`；key 為 SHA1(provider + 目標語言 + 段落文字)，開啟時全部載入記憶體。
- 預設目標語言 `zh-TW`。
- gtx 端點實測（使用者的網路）：
  - 段落用空行（`\n\n`）分隔後一次 POST，回傳的段落數與分隔都保留，所以可以批次送。
  - POST 40000 字元仍回 200；但只驗證了 JSON 有效，沒驗證長文是否完整翻譯，所以每次請求上限先定 5000 字元（約一頁 datasheet）。
  - 每次延遲約 0.5～3 秒；約每 3 秒一次、連續 30 次沒被擋。預設限速每分鐘 20 次、每小時 300 次；每小時上限尚未實測。

P1 狀態：

- 已完成：`fork-build.yml`、spike 實測、`RateLimiter`、`TranslationCache`、`ParagraphText`。
- 待辦（依序）：Google provider 與回應解析（用使用者提供的實際回應當 fixture）→ `EngineBase` 取 block 介面 → `TranslationService`（佇列、批次、背景執行緒）→ 鏡像頁 UI 與「切換中英對照」命令。
