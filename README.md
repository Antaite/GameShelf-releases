# 遊戲書架（GameShelf）

![icon](switch/icon.jpg)

在 Atmosphère 上透過區網，從自己的電腦或 NAS 安裝**自己 dump 的遊戲備份**的自製軟體。支援 **NSP、NSZ、XCI、XCZ** 四種格式。功能類似 Tinfoil 的「區網安裝」，但只連你自己的伺服器。

```
┌──────────────┐                       ┌──────────────────┐
│ Switch       │ ── HTTP / HTTPS ───▶ │ 電腦 / NAS        │
│ LanInstaller │ ◀─── HTTP + Range ── │ server/server.py │
│   .nro       │  邊下載邊解壓邊寫入    │ (你的備份資料夾)   │
└──────────────┘                       └──────────────────┘
```

## 功能

- 全繁體中文介面，直接使用 Switch 內建的系統字型（不需另外放字型檔），中文檔名也能正常顯示
- 支援 `.nsp` `.nsz` `.xci` `.xcz`，可混在同一個資料夾（含子資料夾）
- NSZ / XCZ 在 Switch 上即時解壓縮（zstd 串流模式與區塊模式都支援），不需要先在電腦轉檔
- XCI / XCZ：自動讀取卡帶映像檔的 secure 分割區並照原樣安裝
- 清單分頁顯示（每頁 14 個），L/R 翻頁；按住上下可連續快速移動
- 啟動時讀取主機上已安裝的遊戲、更新、DLC（SD 卡與主機記憶體），和伺服器檔案比對：標示「已安裝」「可更新」「可裝 DLC」，可用分頁切換只看可更新、可裝 DLC 或已安裝的檔案
- 搜尋檔名：內建**注音、倉頡、英數**鍵盤（候選字只列出檔名裡有的字），也可以用主機的系統鍵盤；**繁體、簡體互通**（打「寶可夢」也找得到「宝可梦」）
- **多台伺服器**：自己新增、命名，隨時切換（清單標題點一下伺服器名稱）
- **線上更新**：開啟時檢查新版，先詢問再更新，並驗證作者的數位簽章
- 清理 Atmosphère 的當機報告
- **三種檢視方式**（Y →「檢視方式」）：清單（含封面，一頁 14 款）、**封面牆**（一頁 32 款封面，右側顯示游標所在遊戲的資訊）、**精簡清單**（一頁 14 款，表格顯示檔名、發售日、版本、狀態、大小）
- **HOME 選單捷徑**：第一次啟動時自動在 HOME 選單建立「遊戲書架」捷徑，點它就以完整記憶體模式開啟（不管當初是從相簿還是遊戲進來）；Y →「HOME 捷徑」可以重新建立或移除
- 所有畫面直接顯示 NAS 上的檔名（不帶資料夾路徑）
- 檔名太長時，游標所在那一行和右側標題會以跑馬燈捲動顯示
- 游標所在的遊戲會在右側顯示封面（SD 卡快取 → 主機已安裝的遊戲圖示 → 網路封面網站）
- 伺服器清單存在 `sdmc:/config/lan-installer/servers.ini`（名稱、位址、帳號、密碼）。從舊版升級時，原本的位址會自動變成「伺服器 1」
- 可單選、多選、全選，批次安裝
- 串流安裝：不需先複製到 SD 卡
- 一律安裝到 SD 卡
- 支援本體、更新檔、DLC，一個 XCI 裡同時有多個內容也能一次裝完；自動匯入 ticket / cert
- 每個 NCA 都做 SHA-256 驗證（NSZ 會在解壓後驗證），壞檔一定會被擋下
- 進度條、速度、剩餘時間；按住 B 取消；安裝中暫時停用休眠和 HOME 鍵

## 專案結構

| 路徑 | 說明 |
|---|---|
| `switch/source/main.cpp` | 主畫面、按鍵操作、安裝佇列 |
| `switch/source/screens.cpp` | 各畫面的繪製（清單、進度、結果、訊息） |
| `switch/source/gfx.cpp` | 軟體繪圖與文字排版（stb_truetype，讀取系統字型） |
| `switch/source/install.cpp` | 安裝核心：ncm 佔位寫入、es 匯入 ticket、ns 註冊應用程式 |
| `switch/source/container.cpp` | 開啟 NSP/NSZ（PFS0）與 XCI/XCZ（HFS0 secure 分割區） |
| `switch/source/nca_stream.cpp` | NCZ 解壓 + AES-CTR 重新加密、SHA-256、卡帶→eShop 標頭轉換 |
| `switch/source/crypto.cpp` | 加密包裝：Switch 用 libnx 硬體加速，電腦測試用 OpenSSL |
| `switch/source/http.cpp` | 輕量 HTTP/1.1 客戶端（支援 Range），不需 curl |
| `switch/source/formats.cpp` | PFS0 / HFS0 / CNMT 解析 |
| `switch/source/remote.cpp` | 檔案列表、設定檔 |
| `server/server.py` | 電腦/NAS 端伺服器，只用 Python 標準函式庫 |
| `tests/` | 在電腦上測試所有格式處理邏輯的程式 |

## 1. 編譯 .nro

**方法 A：GitHub Actions（最簡單）**
把整個資料夾推到你自己的 GitHub repo，Actions 會自動編譯。到 **Actions → 最新一次 Build → Artifacts** 下載 `LanInstaller.nro`。

**方法 B：本機編譯**
1. 安裝 [devkitPro](https://devkitpro.org/wiki/Getting_Started)
2. 安裝需要的套件：`dkp-pacman -S switch-dev`（zstd 解壓程式已內建在專案裡）
3. 執行：
   ```
   cd switch
   make
   ```

**方法 C：Docker**
```
docker run --rm -v "$PWD":/src -w /src/switch devkitpro/devkita64 make
```

## 2. 啟動伺服器（電腦或 NAS）

需要 Python 3.8 以上：
```
python3 server/server.py "D:\Switch\Games"
```
畫面會顯示 Switch 要連的位址，例如 `http://192.168.1.20:8000/`。

- 預設 HTTP 埠 `8000`，可用 `--port` 修改
- Windows 第一次執行時，請在防火牆允許 Python 使用「私人網路」
- NAS：用 SSH 或排程執行同一行指令即可
- 伺服器只會分享你指定的資料夾，無法讀取資料夾外的檔案

## 2b. 不跑 server.py，直接連 NAS

也可以直接連 NAS 內建的檔案服務，程式會自動判斷伺服器類型（依序嘗試 server.py → JSON 檔案清單 → WebDAV → 網頁目錄）：

| 伺服器類型 | 位址範例 |
|---|---|
| server.py（電腦） | `http://192.168.1.20:8000/` |
| WebDAV（Synology / QNAP / ASUSTOR…） | `http://帳號:密碼@192.168.1.20:5005/games/` 或 `https://帳號:密碼@192.168.1.20:5006/games/` |
| 網頁目錄列表（nginx、Apache、`python -m http.server`） | `http://192.168.1.20:8080/games/` |
| Tinfoil 格式 JSON 檔案清單（`{"files":[{"url":…,"size":…}]}`） | `https://帳號:密碼@nas.example.com:8001/` |

- 帳號密碼直接寫在網址裡：`http://帳號:密碼@位址:埠/資料夾/`。密碼裡如果有 `@`、`:`、`/`，請改寫成 `%40`、`%3A`、`%2F`
- 畫面上會把密碼顯示成 `****`，但設定檔 `config.ini` 裡是明碼保存
- **HTTP 和 HTTPS 都支援**。Synology WebDAV Server 預設：HTTP 用 5005、HTTPS 用 5006
- HTTPS 不驗證憑證（NAS 幾乎都是自簽憑證），請只在自己的區網使用
- 如果出現「這個埠是 HTTPS」，把網址開頭的 `http://` 改成 `https://` 即可
- NAS 管理頁面（例如 DSM 的 5000/5001 埠）不是檔案服務，不能直接連

## 3. 在 Switch 上使用

1. 把 `LanInstaller.nro` 放到 SD 卡 `/switch/LanInstaller/LanInstaller.nro`（hbmenu 上顯示「遊戲書架」；檔名維持不變，萬能前端不用重做）
2. 建議按住 **R 啟動任一遊戲**或用萬能前端開啟（完整記憶體模式）
3. 第一次啟動：按 **Y** →「伺服器設定」→「＋ 新增伺服器」，依序輸入名稱、位址、帳號、密碼

| 按鍵 | 功能 |
|---|---|
| 上下 | 移動游標（按住會連續移動，按久會加速） |
| L / R、左右 | 上一頁 / 下一頁 |
| A | 選取 / 取消選取 |
| X | 把選取的遊戲加入安裝佇列（沒選就是游標所在的遊戲）。安裝在背景進行，可以繼續選其他遊戲 |
| − | 安裝進行中：查看安裝進度（也可以點畫面上方的藍色標籤）；進度畫面按 B 回到清單、按住 Y 全部取消 |
| Y | 開啟選單 |
| ZL / ZR | 切換分頁：全部 / 可更新 / 可裝 DLC / 已安裝 |
| B | 有搜尋 / 分頁時：清除搜尋並回到第一個分頁（只是返回，不會重新讀取伺服器；要重新讀取請按 Y → 重新整理） |
| + | 離開（從 HOME 選單捷徑或按住 R 開遊戲進來時，會直接回到 HOME 選單） |

安裝中斷（取消、離開軟體、斷線）時會保留進度，排隊中還沒開始的檔案也會記住（config/lan-installer/queue.txt）：下次開啟軟體會詢問要不要從中斷處繼續，或在清單中再按一次安裝也會接著下載。NSP / XCI 從中斷的位置接續；NSZ / XCZ 只重下正在處理的那個內容檔。連線問題會自動等 5、15、30、30 秒重試。

SD 卡上的資料不會越用越大：封面快取超過 50 MB 時，自動刪除最久沒看過的封面到 40 MB；版本紀錄只保留主機上現有遊戲；install.log 超過 512 KB 會重來。未完成的安裝放置超過 7 天，開啟時會用黃色提醒佔用的空間。

HTTPS 使用內建的 mbedTLS（比系統加密服務快），連不上時自動改用系統服務。install.log 會記錄每個大型內容檔的速度，以及時間花在等網路、寫 SD 卡或解壓縮的比例。連線時優先使用 ChaCha20-Poly1305 加密（Switch 的 CPU 算起來比 AES-GCM 快）；伺服器不支援時自動改用 AES-GCM。install.log 會記錄實際用到的加密方式（`TLS cipher:`）。

清單一款遊戲一列（顯示檔名有 [BASE] 的本體檔，不分大小寫）。選取後按 X：主機上還沒有這款遊戲時，本體＋最新更新＋所有 DLC 一起安裝；已經安裝時，只裝缺少的更新和 DLC。沒有本體檔的更新／DLC 只在主機已裝本體時才會出現。

觸控：分頁、排序標籤、搜尋標籤（點一下清除搜尋）、遊戲列（點一下選取）、選單項目都可以直接點。

選單（Y）：

| 項目 | 說明 |
|---|---|
| 排序方式 | 名稱、發售日（新→舊 / 舊→新）、檔案大小（大→小 / 小→大）。同一款遊戲的本體、更新、DLC 永遠排在一起 |
| 搜尋 | **注音**、**倉頡**、**英數**（內建鍵盤），或**系統鍵盤**；遊戲名稱、檔名、遊戲 ID 都可以。輸入時即時顯示符合幾款 |
| 一鍵選取所有更新與 DLC | 勾選所有「可更新」「可裝 DLC」的檔案，再按 X 一次裝完 |
| 全選／取消全選 | 目前分頁的所有檔案 |
| 重新整理 | 重新讀取伺服器與主機上的內容 |
| 同時安裝數量 | **自動（預設）**、1、2、3 個。自動：先裝 1 個，試著多開 1 個，總速度真的變快（多 15% 以上）才保留，沒變快就收回，5 分鐘後再試；網路是瓶頸時，一次裝滿速的 1 款會比 3 款分著慢慢裝更早能玩。優先同時裝不同遊戲；只剩同一款遊戲的檔案時，它們也會同時安裝。記憶體不足的檔案會在最後單獨重試 |
| 安裝時 CPU 加速 | 預設開啟。安裝期間把 CPU 拉到 1785 MHz（記憶體 1600 MHz、GPU 降到最低，畫面由 CPU 繪製不受影響），HTTPS 解密與解壓縮更快；較耗電、機身較熱，裝完、取消或離開時自動恢復原本頻率 |
| 伺服器設定 | 伺服器清單：新增、切換、修改名稱／位址／帳號／密碼、測試連線、刪除。安裝中也可以切換，排隊中的檔案繼續從原本那台下載 |
| 清理當機報告 | 顯示並清空 Atmosphère 的 crash_reports、erpt_reports、fatal_reports、fatal_errors（資料夾保留，其他不動） |
| 檢查更新 | 手動檢查新版（開啟時也會在背景檢查） |

### 安裝進度畫面

- 「排隊中」以遊戲為單位：↑↓ 選擇，**ZL 上移、ZR 下移、X 移除**；也可以點遊戲後按畫面上的「置頂／上移／下移／移除」。本體、更新、DLC 仍會照順序安裝。調整後的順序會存到 SD 卡，下次開啟一樣
- 顯示的速度是約 8 秒的平均，不會一直跳動
- 同一個檔案連續兩次算出一模一樣的錯誤雜湊值時（檔案本身和描述檔不一致，常見於重新打包過的檔案），不再重試，直接說明原因
- **R 或點「關閉螢幕」**：安裝中關掉掌機螢幕背光省電，安裝繼續；任意按鍵或觸碰螢幕叫醒（叫醒那一下不會觸發其他功能），全部裝完也會自動亮起。接電視時不影響電視畫面
- 連線卡住（30 秒內幾乎沒收到資料）會自動斷開重連；重試用完仍失敗的檔案，會在這批安裝結束後詢問要不要再試一次（從中斷處接續）
- 用按住 R 開遊戲的方式啟動時，每個安裝可以先下載 64 MB 暫存在記憶體，寫 SD 卡偶爾變慢時下載不用停（相簿模式維持 8 MB）
- 網路中斷時會暫停等待網路恢復，不消耗重試次數，恢復後從中斷處接著下載

選單裡任何設定（排序、同時安裝數量、CPU 加速、伺服器）一改完就立刻存檔、立刻生效。

### 搜尋鍵盤（注音／倉頡／英數）

- 方向鍵選鍵、**A** 輸入（或直接點），**B** 刪除，**L / R** 選候選字、**Y** 用這個字（「空白」也會用第一個），**X** 切換注音／倉頡／英數，**+** 完成，**−** 取消
- 注音：可只打聲母（例如「ㄇ」）或不打聲調；打了聲調只列那個聲調的字；「一聲」鍵代表第一聲
- 倉頡：可只打前幾碼
- **候選字只列出伺服器檔名裡真的有的字**，最常出現的排前面
- 字典資料：Unicode 漢字資料庫（Unihan），由 `tools/make_ime_table.py` 產生 `switch/source/ime_data.inc`

### 線上更新

- 開啟時在背景檢查 `github.com/Antaite/GameShelf-releases` 的最新版本；有新版先詢問（立即更新／稍後再說／略過這個版本）
- 下載後用內建的公鑰驗證數位簽章（ECDSA P-256），**不是作者簽的一律拒絕**；驗證通過才替換 .nro，失敗時舊版完全不變
- 自行編譯、沒有簽章金鑰的版本不會自動更新
- 發佈方式見下面「發佈新版（GitHub）」

### 畫面右上角

每個畫面都會顯示主機時間與網路狀況（Wi-Fi 訊號格數、有線，或紅色「網路中斷」）。

### 安裝速度

安裝與下載分散在 Switch 開放給軟體的多顆 CPU 核心上執行（舊版全部擠在同一顆核心）。

## 已安裝內容比對

- 遊戲 ID 與版本從檔名讀取，例如 `[0100A1301EECC800][v65536]`：ID 結尾 `000` 是本體、`800` 是更新、其他是 DLC
- 更新：本體已安裝，且檔名的版本比主機上的新（或主機上還沒有更新）→「可更新」
- DLC：本體已安裝，但主機上沒有這個 DLC →「可裝 DLC」
- 本體沒安裝的更新 / DLC 不會列在「可更新」「可裝 DLC」裡，但在「全部」裡仍可安裝
- 檔名沒有版本號時，只要主機上已有同一個更新就視為已安裝
- 安裝完成後會自動重新比對

## 遊戲名稱與發售日（titledb）

- 發售日與封面網址來自公開的 [blawar/titledb](https://github.com/blawar/titledb)。原始資料四區加起來約 280 MB，所以改由 GitHub 每週自動整理（`.github/workflows/titledb.yml` 執行 `tools/make_titledb_notes.py`）：
  - 依序合併 **香港 → 韓國 → 日本 → 美國**，香港查不到的遊戲由後面的地區補上
  - 同一款遊戲有好幾筆（普通版、Switch 2 版、同捆版、重新上架…）時，取**最早的發售日**
  - 只留「遊戲 ID、發售日、封面網址」，約 3 MB，放在 GameShelf-releases 的 `titledb` 發佈（標成預先發佈，不會被當成程式更新）
- 主機下載這份資料存成 `sdmc:/config/lan-installer/titledb.tsv`；抓不到時改下載香港原始檔（約 50 MB）
- 清單顯示 NAS 上的檔名，右側顯示發售日
- 每 **7 天**自動在背景重新下載；也可以按 Y →「**更新遊戲資料庫**」手動更新。下載時清單照常可以用；想換來源：在 `config.ini` 加 `titledb=網址1,網址2`
- 下載失敗時只是不顯示中文名稱與發售日，其他功能不受影響
- 只會讀取 NAS 上的檔案，不會修改、上傳或刪除 NAS 上的任何東西

## 遊戲紀錄自動修復

- 每次啟動會檢查主機上每款遊戲的 HOME 選單紀錄：如果本體、更新或 DLC 已經安裝但沒有登記（例如 v1.7 以前安裝更新時造成的「無法啟動軟體」），會自動補回並顯示「已自動修復 N 款遊戲的紀錄」
- 安裝更新 / DLC 時，紀錄一律依主機實際安裝的內容重建，且絕不會寫入沒有本體的紀錄
- 安裝與修復的過程會記錄在 `sdmc:/config/lan-installer/install.log`，遇到問題時可以提供這個檔案

## HOME 選單捷徑

- 捷徑是一個約 300 KB 的小型安裝檔，在主機上當場產生並安裝（程式 ID `0100475348460000`），內容是 `switch/forwarder` 裡的載入程式（nx-hbloader 修改版，固定開啟 `sdmc:/switch/LanInstaller/LanInstaller.nro`）＋名稱與圖示
- 從 HOME 選單開啟就是「應用程式」，所以一定是完整記憶體模式；線上更新替換 .nro 後捷徑照樣可用
- 需要：SD 卡上的 prod.keys（`header_key`、`key_area_key_application_00`，加密捷徑用）以及簽章補丁（sigpatches）
- 第一次啟動自動建立一次（設定 `shortcut_auto=1`），之後只在 Y →「HOME 捷徑」重新建立或移除；移除只拿掉 HOME 上的圖示，不影響遊戲書架本身
- GitHub Actions 會先編譯 `switch/forwarder`，把 `hbl.nso`、`hbl.npdm` 和圖示放進 `switch/data/` 一起編進遊戲書架；自行編譯沒有這一步時，捷徑功能會顯示無法建立

## 遊戲封面

右側封面依序從這些地方取得：

1. SD 卡快取 `sdmc:/config/lan-installer/covers/<遊戲ID>.jpg`（也可以自己放圖片進去，JPG 或 PNG 都可以）
2. 主機上已安裝的同一款遊戲（更新檔、DLC 會顯示本體的圖示）
3. 遊戲資料庫裡的 eShop 官方圖示：Atmosphère 的 hosts 設定會擋掉所有任天堂網址（保護主機不連任天堂），所以改透過公開圖片代理 `wsrv.nl` 代抓並縮成 256×256；可以在 `config.ini` 加 `icon_proxy=網址` 改掉（`{url}` 會換成圖示網址），寫 `icon_proxy=` 留空則直接連
4. 網路封面網站：預設 `https://tinfoil.media/ti/{id}/256/256/`，可以在 `config.ini` 加一行 `cover=網址` 改掉（`{id}` 會換成 16 碼遊戲 ID），寫 `cover=` 留空則關閉網路下載

遊戲 ID 從檔名裡的 `[0100XXXXXXXXXXXX]` 取得，檔名沒有 ID 就不會顯示封面。安裝進行中會暫停下載封面。

- 網站明確回答「沒有這張圖」才會顯示「找不到封面」；網路暫時不穩時，30 秒後再選到那款遊戲會自動重試
- 按 Y →「**重新抓取封面**」可以立刻重試所有沒有封面的遊戲
- 封面牆一次顯示 32 款：已存過的馬上顯示，其他在背景一張一張下載（游標所在的遊戲優先），記憶體中最多保留約 100 張
- 封面下載失敗的原因（網站、錯誤代碼）會寫進 `install.log`，每次開啟最多記錄 40 筆

## 注意事項與限制

- **NSZ／XCZ 的記憶體**：很多 NSZ 壓縮時用了很大的壓縮視窗（可到 1 GB），而且沒記錄解壓後大小。本程式會改用「解壓後的實際大小」配置記憶體，小的 DLC／更新只需要很少記憶體。真正大的檔案仍需要大量記憶體：記憶體不足時會等其他檔案裝完再單獨重試，仍不夠時會提示改用按住 R 開遊戲的方式啟動。
- **從相簿開啟**時右上角會顯示黃色「相簿模式：記憶體較少」，點一下可以看說明。

- 更新檔和本體的安裝順序不拘，都會正確合併到同一個遊戲圖示。
- 若出現 `Open meta NCA` 或 `ImportTicket` 錯誤，通常是 Atmosphère 版本太舊、ticket 不完整，或缺少 sigpatches。
- 安裝失敗時會自動刪除寫到一半的資料，不會留下垃圾檔。
- 測試狀況：電腦端伺服器、網路傳輸、四種格式的解析、NCZ 解壓與重新加密、卡帶標頭轉換、壞檔與截斷檔偵測，都有自動測試（測試資料依真實格式產生：XTS 加密標頭、AES-CTR 區段、zstd 串流與區塊模式）。Switch 端程式碼已對照 libnx 標頭做過完整型別檢查，**但尚未在實機上測試**。第一次使用建議先拿一個小的 DLC 或更新檔試裝。

## 開發測試

需要 g++、Python 3 + `cryptography`、zstd 指令、OpenSSL：
```
bash tests/run_host_tests.sh
```

預覽 Switch 畫面（用電腦上的中文字型繪製，輸出 PPM 圖檔）：
```
g++ -std=gnu++20 -O2 -o render tests/render_screens.cpp switch/source/screens.cpp switch/source/gfx.cpp switch/source/http.cpp
./render /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc 3 .
```

## 發佈新版（GitHub）

原始碼放在私人 repo，編譯好的 .nro 自動發佈到公開的 `Antaite/GameShelf-releases`，主機從那裡更新。

**第一次設定（只做一次）：**
1. 在 GitHub 建立空的**公開** repo：`GameShelf-releases`
2. 建立權杖：GitHub 右上頭像 → Settings → Developer settings → Personal access tokens → **Fine-grained tokens** → Generate new token
   - Repository access：Only select repositories → 選 `GameShelf-releases`
   - Permissions → Repository permissions → **Contents：Read and write**
   - 產生後複製權杖
3. 私人 repo → Settings → Secrets and variables → Actions → New repository secret：名稱 `RELEASES_TOKEN`，內容貼上權杖
4. 私人 repo → Actions → 「Make update signing key (run once)」→ Run workflow。跑完後下載 `signing-key`，打開 `private-key.pem`，把**整個內容**新增成 secret：名稱 `UPDATE_SIGNING_KEY`。完成後刪掉那次執行紀錄（右上 ⋯ → Delete workflow run）
5. 私人 repo 的 Actions 要允許執行（Settings → Actions → General → Allow all actions）

**每次發佈：** 私人 repo → Releases → Draft a new release → 新增標籤（例如 `v3.0.1`，要比目前版本大，`switch/Makefile` 和 `main.cpp` 的版本號也一起改）→ 說明欄寫更新內容 → Publish。Actions 會自動編譯、簽名，並發佈到公開 repo；主機下次開啟就會詢問更新。

## 第三方程式碼

- `switch/source/zstd/`：Zstandard 解壓縮器（BSD 授權，見該資料夾 LICENSE）
- `switch/source/stb_truetype.h`：stb_truetype（Public Domain / MIT）
- `switch/source/stb_image.h`、`switch/source/stb_image_write.h`：stb_image／stb_image_write（Public Domain / MIT）
- `switch/forwarder/`：nx-hbloader（ISC 授權，https://github.com/switchbrew/nx-hbloader）
- `switch/source/ime_data.inc`、`switch/source/tsconv_data.inc`：由 Unicode 漢字資料庫（Unihan：kMandarin、kTGHZ2013、kXHC1983、kCangjie、kSimplifiedVariant）產生，Unicode License V3（https://www.unicode.org/license.txt）

## 授權與用途

僅供安裝你自己合法購買並自行 dump 的遊戲備份使用。
