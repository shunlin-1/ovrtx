# Omniverse Web UI 整合架構決策

> 高雄燈塔等地端 desktop 部署的 Web UI 整合方案
> 起始日期:2026-05-07
> 重大架構決定:2026-05-08 — pivot 從 Kit microkernel 改為 **ovrtx library + 自寫 extension framework + USD as contract**
> 此 doc 保留決策演進歷史,當前生效架構在「架構決定(2026-05-08)」一節

---

## 架構決定(2026-05-08)

經過兩日 spike + Public SDK 探勘 + GTC 2026 product 比對後,確定以下架構:

### 三大支柱

| | 選擇 | 理由 |
|---|---|---|
| **Renderer** | `ovrtx` C library(直接 link) | NV 2026 GTC 公開,公開 Vulkan/CUDA interop,GPU 資源 access 完整,解決 Kit `carb::graphics` 黑盒問題 |
| **App framework** | 自寫 Y framework(`.app.toml` + extension loader) | 保留 Kit 「全部模組化」哲學,但 format 自己 control,不被 NV ABI 變動綁架 |
| **Inter-tool contract** | USD(NV/Pixar 標準格式) | 跨工具(Composer / Maya / 你 viewer)、跨年代、跨 vendor 可攜,客戶端 BIM 檔 5 年後 viewer 仍讀 |

### 新架構圖

```
your_company_viewer.exe(自己 build,Steam-style 雙擊啟動)
│
├─ Link 進來的 NV libraries(都是 ov* 系列獨立 C library)
│   ├─ ovrtx          ← RTX 渲染 + USD 場景載入(usdrt 加速)
│   ├─ ovphysx        ← 物理模擬(可選,Isaac Sim 核心能力)
│   └─ ovstorage      ← USD asset 管理(可選)
│
├─ Link 進來的第三方
│   ├─ CEF SDK        ← 嵌入 Chromium
│   ├─ Vulkan         ← 視窗 / shared texture
│   └─ pybind11       ← Python extension 支援(可選)
│
├─ 你寫的 main.cpp
│   ├─ Vulkan window setup
│   ├─ ovrtx context + USD scene load
│   ├─ CEF init(non-OSR → OSR → shared texture)
│   ├─ Composite layer(Vulkan shader 把 CEF texture overlay 在 ovrtx 渲染上)
│   └─ Main loop(physics step → render → composite → present)
│
└─ Y Framework extension host
    ├─ Reads viewer.app.toml(取代 .kit file)
    ├─ Resolve dependencies + topo sort
    ├─ Load extensions in order
    └─ Tick all extensions per frame
        ├─ your_company.commands       ← USD 業務邏輯,跨產品
        ├─ your_company.bridge         ← 命令 queue / handler registry
        ├─ your_company.ui.cef         ← CEF 整合(C++)
        └─ <product>.handlers          ← 產品專屬(BIM / Lighthouse / 中華電信)
```

### 為何這個 stack 對齊未來

| 戰略點 | 對齊 |
|---|---|
| NV 策略方向(2026 GTC) | ✅ Library-first 是 NV 自己在推的方向,我們搭順風車 |
| 換引擎 hedge(doc 原始目標) | ✅ ov* 是 C library,Wicked / Unreal / Rust app 都 FFI 得來,可攜性遠超 Kit |
| Isaac Sim 整合潛力 | ✅ Isaac Sim 核心能力是 ovphysx + ovrtx + sensor schemas,我們直接 link 同等能力 |
| 跨產品 reusability | ✅ Y framework + USD schema 兩層 reuse,跨客戶 deployment 共享 framework + 各自 schema |
| USD 格式長期 portability | ✅ Pixar 主版號保持向後相容承諾,5 年後客戶 USD 仍可讀 |

### 從探索期到決定期關鍵 finding

1. ❌ Kit C++ public IRenderer.h transitive include 缺檔(packaging bug)
2. ❌ Python `omni.kit.renderer.core` 沒 binding,`omni.gpu_foundation_factory` 把 Device/Texture 全砍成空 class
3. ❌ `omni.kit.renderer.imgui` 明標 `python_api_hidden = true` `cpp_api_hidden = true`
4. ✅ `ovrtx` 公開 C API + Vulkan interop 範例,正好對應我們 Stage 3 需求
5. ✅ ov* 系列(ovrtx / ovphysx / ovstorage)都是獨立 library 不是 Kit extension,直接 link 即可
6. ✅ Isaac Sim 核心能力可透過 ov* libraries 取得,不需要拖 Kit framework

### USD 為什麼是合理的 inter-tool contract

USD 在 NV 生態系是真正的 portable contract:
- **層內優化(SubLayers)**:base / overrides / annotations 分層,改一層另兩層不動
- **變體系統(Variants)**:`winter` / `summer` / `phase_1` 切換,適合燈塔場景配置
- **Reference 機制**:模組化建模,版本升級改 path 一行,所有引用自動跟
- **跨工具 / 跨年代相容**:Pixar 維持 forward+backward compat,5 年後新 reader 仍讀舊檔

下游 viewer 完全不依賴上游用什麼工具產 USD,只要產出 standard USD,你 viewer load 就吃。**update 工作流靠 USD layered composition 解決,viewer .exe 不用重 build**。

---

## 背景與目標

**主要需求**
- 地端部署,使用體驗類似 Steam 遊戲(雙擊 .exe 即可)
- UI 重(複雜 component、需要 overlay 蓋在 viewport 上)
- Omni UI 不夠用,需要現代 web UI(Vue / React / Svelte)
- 跨平台(Windows + Linux)
- 未來可能換引擎(Wicked Engine),架構要保留彈性

**為什麼 Omni UI 不夠用**

Omni UI 適合做 DCC 工具型 UI(USD Composer、Code、View 等級),但對企業客戶交付有明顯不足:

- 流暢動畫、複雜 layout 難實作
- 沒有 npm 生態,任何輪子都要自己做
- 設計師無法介入(Figma 設計無法直接套)
- 視覺上是「工程工具」而非「產品」
- 給政府 / 中華電信 / 燈塔展示等場景,觀感達不到「現代產品」標準

---

## 架構演進過程(2026-05-07 至 05-08 探索期歷史)

> 以下保留決策歷史,當前生效架構參見上方「架構決定(2026-05-08)」。
> 這段歷史揭露為什麼 Kit-based 路線最終棄掉,對未來重新評估有參考價值。

### 已排除的選項

**Kit App Streaming(server-side rendering)**
- 需要 server 端 GPU,不符合「地端 desktop app」需求
- 排除

**Native Window Embedding(HWND reparenting)**
- 零延遲、效能最好
- 但 native window 永遠在 OS 最上層,**無法做 overlay**
- UI 大量需要按鈕蓋在 viewport 上 → 排除

**Tauri + Headless Kit + Localhost WebRTC(純串流)**
- 16–33ms 延遲、H.264 壓縮失真
- 對 BIM 細節 / 文字標註 不夠好
- 仍保留作為 fallback / 遠端模式

**Unreal-style WebBrowser(CEF on Game Thread)**
- Unreal 把 CEF 塞進 Game Thread,UI 重時必卡
- Kit 主執行緒比較閒,可行但仍需設計避免類似陷阱

### 選定方案:CEF OSR + C++ Extension + Shared GPU Texture

**關鍵設計**

- C++ extension(避開 Python GIL)
- CEF OSR(off-screen rendering)模式
- `multi_threaded_message_loop = true`(CEF 完全自己 thread)
- **DXGI shared NT handle**:CEF GPU process(D3D11/ANGLE)→ Kit(D3D12 native)的 GPU texture 共享
- Hydra render task 做 overlay composite
- 命令協定 transport-agnostic(支援未來切換 WebRTC mode)

**Kit 的 graphics 真實狀態(2026-05 實測)**

掃 `_build/.../extscache/` 跟 `kit/dev/include/` 確認:

| 面向 | 實際 |
|---|---|
| Kit 是否內建 CEF / Chromium / V8 | ❌ 完全沒有,我們拉 CEF 不會跟 Kit 打架 |
| Kit Windows 用什麼 graphics API | **D3D12 native**(只有 `omni.gpu_foundation.shadercache.d3d12` 跟 `omni.hydra.rtx.shadercache.d3d12`,無 d3d11/vulkan) |
| Kit 是否暴露 `ID3D12Device*` | ⚠️ 沒直接暴露,經過 `carb::graphics::Device*` opaque 抽象層 |
| 有 interop interface 的對象 | CUDA(`carb::cudainterop`)、OpenGL(`carb::glinterop`),**沒有 D3D interop** |
| Texture 匯入 API | `IRenderer::createExternalTextureGpuData()`(line 213, 待研究) |

**對 Stage 3 的具體影響**

CEF GPU process 給的是 DXGI shared NT handle(D3D11 風格)。Kit 是 D3D12,但這個 handle **跨 D3D 版本通用**(都 build 在 DXGI 上)。問題不是 D3D11/D3D12 不通,是 **Kit 不讓我們拿 D3D12 device 去 OpenSharedHandle**。

**Public API 探勘狀態(2026-05-08 實測)**

掃 `kit/dev/include/` 跟 `extscache/*/omni/*.pyi`,**所有 public 路徑都被收窄**:

| 路徑 | 問題 | 證據 |
|---|---|---|
| C++ `IRenderer.h` 直接 include | Build 失敗,缺 `carb/memorytracking/IGpuMemoryTracker.h`(packaging bug 或刻意收窄) | 我們實測 build error |
| C++ `createExternalTextureGpuData()` API | 因 IRenderer.h 不能 include,連帶不能用 | 同上 |
| Python `omni.kit.renderer.core` bindings | **沒有 Python 模組**,純 C++ 純 DLL | `extscache/omni.kit.renderer.core-*/` 沒任何 .py / .pyi |
| Python `omni.gpu_foundation_factory` | `Device` / `Texture` / `RpResource` 都是空 class,只能拿 reference 不能操作。沒 texture import API | `_gpu_foundation_factory.pyi:29-50` `class Device(): pass` |
| Python `omni.kit.hydra_texture` | 方向反了,是「Hydra 渲染 → texture output」不是 input | `_hydra_texture.pyi` 整檔沒 import / external texture API |
| `omni.kit.renderer.imgui` | NV 明確標 `python_api_hidden = true` `cpp_api_hidden = true` | `extension.toml:187-188` |

**結論**:NV 故意把 GPU resource 的 **import / create API 從 public 全砍**(C++ 跟 Python 兩個都砍),Kit 公開 SDK 的目標是「給你資訊跟 lifecycle 控制,不給你建立資源」。對 third-party CEF 整合 **基本上沒乾淨路徑**。

**剩下的可行路徑**

| 路徑 | 機制 | 評估 |
|---|---|---|
| **C. USD prim + material** | 創 screen-space quad,把 texture 當 material 貼上去,讓 Hydra 自然渲染 | 走 USD 公開 API,純黑盒,可行但 perf 看 USD asset reload 是否每 frame |
| **E. NV enterprise SDK** | 跟 NV 簽 NDA / 企業合約拿 internal headers / advanced API | 必須做的事,但流程慢 |
| **F. Stub 強行 build** | 補 `IGpuMemoryTracker.h` 偽 header,強行使用 IRenderer | 可能技術上通,但 NV 任何升級都會破,production 不適合 |
| **G. 重新評估架構** | 改用 Tauri + WebRTC streaming(犧牲畫質換實作可行性) | 你 doc line 41 已列為 fallback,不要太快走但要記得它存在 |

**目前推薦**:E + C 並行(對 NV 提 ticket / 找企業窗口同時用 USD prim 路線做 Phase 1 demo)。Stage 3 從技術風險最高升級為「需要 NV 介入或繞道」,**這對專案 timeline 影響顯著,要早讓 stakeholders 知道**。

---

## 重大策略選項:`ovrtx`(2026 GTC 發布,公開 C/C++ 庫)

**2026-05-08 發現**:NV 在 2026 GTC 公開 modular library,把 Omniverse 三大核心拆成獨立 C API:

| Library | 內容 | GitHub |
|---|---|---|
| **ovrtx** | RTX 渲染 + USD 場景載入 + CUDA/Vulkan interop | github.com/NVIDIA-Omniverse/ovrtx |
| **ovphysx** | PhysX 模擬,DLPack zero-copy tensor exchange | github.com/NVIDIA-Omniverse/PhysX |
| **ovstorage** | USD storage layer | (待查) |

**對我們的關鍵意義**:`ovrtx` 公開 **Vulkan Interop 範例**(`examples/c/vulkan-interop`),示範「外部 Vulkan window + ovrtx 渲染 USD 到共享 texture」 — **這正是我們 Stage 3 要做的 GPU shared texture 機制,只是換 Kit → ovrtx**。

### 兩個架構選項對照

| | A. 維持 Kit + 繞道 | B. 改架構走 ovrtx |
|---|---|---|
| Renderer | Kit microkernel + omni.kit.renderer | ovrtx C library 直接 link |
| GPU 資源 access | ✗ 黑盒(我們驗證過) | ✅ 公開 C API,Vulkan/CUDA interop |
| CEF 整合難度 | 卡住,要 NV 商務介入 | 直接用 ovrtx Vulkan interop 範例改 |
| USD 場景 | Kit 完整支援 | ovrtx 支援(focus on 視覺,沒 authoring) |
| Extension ecosystem | ✅ Python extension / setup / messaging | ❌ 沒 microkernel,自己寫 app loop |
| API 穩定性 | Kit 110.x 已 production | early access(2026 年內 production stable) |
| Deployment | .kit file driven | 自己 build .exe |
| 跟 BIM viewer 需求對齊度 | 過量(Kit 給太多 authoring 工具) | 剛好(viewer-focused) |

### 推薦評估

**對 BIM viewer / 燈塔 / 中華電信 use case 來說,B(ovrtx)架構更合適**:
- 你需求是「載 USD + RTX 渲染 + web UI overlay」,**不是 USD authoring 工具**
- ovrtx 公開 API 解決我們 GPU 資源 access 卡點
- Vulkan interop 範例 ≈ 50% Stage 3 工作免費送
- early access 風險 → 2026 內會 stabilize,跟燈塔 timeline 估計符合

**轉換成本**:
- 之前在 kit-app-template 內做的 C++ extension 工作可重用 80%(都是 carb / pybind 知識)
- `your_company.commands` / `bridge` / `ui.cef` 三個 extension **概念照搬**,只是 host 從 Kit 變 ovrtx
- 失去 Kit 的 extension marketplace,但你本來就沒打算用 NV 第三方 extension

---

## 探索期最終架構草案(已被上方 ovrtx 架構取代)

> 此圖描述 Kit-based 假設下的 thread / process 規劃。
> 切到 ovrtx 後 thread 模型基本不變(因為 carb framework / Vulkan threading 概念類似),
> 只是 host 從 `kit.exe` 變成 `your_company_viewer.exe`,
> Hydra render task 變成 ovrtx + Vulkan composite shader。

```
Kit Process(單一 .exe) — 已被 ovrtx-based 取代
│
├─ Main Thread
│   ├─► USD authoring(只在使用者操作瞬間活躍)
│   └─► Hydra Sync(stage 變更同步)
│
├─ Render Thread
│   ├─► Hydra Execute、RTX rendering
│   └─► UI overlay composite(C++ Hydra task)
│
├─ CEF UI Thread
│   └─► message loop、event dispatch
│
├─ CEF Renderer Process(獨立 process)
│   └─► V8 + Vue + DOM
│
├─ CEF GPU Process(獨立 process)
│   └─► UI → shared GPU texture
│
└─ Worker Threads
    ├─► JS bridge handlers、業務邏輯計算
    └─► 命令推到 main thread queue
```

**ovrtx 版本的差異**:
- Main thread 改為「你 main.cpp 自己寫的 loop」,內含 ovphysx step + ovrtx render + composite + present
- 沒有 Kit microkernel + extension lifecycle,改用 Y framework 的 `host.tick()`
- 「Hydra render task」這層機制 ovrtx 是否開放給 user 自定 task 待 spike vulkan-interop 範例後確認;若不開,改用 Vulkan composite shader 在 main loop 後處理(更直接)
- 主執行緒對 CEF 的負擔仍接近 0,因為 CEF MTML 自己跑 thread,主執行緒只在 dequeue command + apply USD edit 時活躍

---

## 關鍵技術概念

### Threading 模型:Unreal vs Kit

| 面向 | Unreal | Kit |
|---|---|---|
| 主執行緒負擔 | AActor::Tick + Blueprint VM 每 frame 跑 | 只有 USD authoring + extension lifecycle |
| Gameplay 邏輯 | Game Thread 序列化(滿載) | 沒有 gameplay tick |
| 渲染 | Render Thread(1 frame behind) | Hydra/RTX 獨立 thread |
| 適合塞 CEF | ✗(主執行緒已滿) | ✓(主執行緒寬鬆) |

**重要 reframe**:不是「browser 重 → Kit 怕 browser」,而是「Unreal Game Thread 已經滿 → 多塞 CEF 必爆;Kit 主執行緒沒滿 → 塞 CEF 還有空間」。

**Unreal 不是只跑一個 core 的迷思**:Unreal 用很多 core,但 gameplay 邏輯集中在 Game Thread 一條 thread。Render Thread / RHI Thread / TaskGraph Workers / Audio Thread 都是獨立 thread。問題不是「只用一個 core」,是「Game Thread single-threaded」。

### Omniverse vs Unreal 架構差異

| 面向 | Omniverse | Unreal |
|---|---|---|
| 場景模型 | USD(layered, composable, declarative) | UObject / AActor 樹(instance-based) |
| 修改模型 | Authoring layers(non-destructive) | 直接改 instance state |
| 渲染 | Hydra(可插拔 render delegate) | 單體 Renderer(Lumen/Nanite 綁死) |
| 應用模型 | Microkernel + Extensions | Monolithic Engine |
| 主要語言 | Python + C++ (carb) | C++ + Blueprint |
| 編輯 / 執行 | 同一個 runtime | Editor / PIE 分離 |
| 多人協作 | 內建(Nucleus + Live Sync) | 要自己做 replication |

### 三條主執行緒暗線(即使 CEF 跑獨立 thread 仍要處理)

1. **Python GIL**:Python callback 必須在主執行緒 dispatch,即使重邏輯丟 worker thread,dispatch 本身仍占主執行緒
2. **USD Stage 是 single-writer**:任何 stage 編輯最後一哩都要回主執行緒序列化
3. **Hydra Sync Phase**:stage 變更跟 render index 同步點壓在主執行緒

**C++ extension 解掉 1**,但 2、3 是 USD 設計本身,任何方案都要面對。

**解法**:
- 重 callback 在 worker thread 計算結果
- 推命令到 main thread queue,不直接寫 stage
- 主 thread 每 frame 用 `SdfChangeBlock` 批次處理 queue
- Hydra task 完全跑在 render thread,不碰主 thread

```cpp
// 範例:命令推到主執行緒 queue
void onSelectObject(const std::string& primPath) {
    auto result = computeSelectionResult(primPath);  // worker thread
    g_mainThreadCommandQueue.push([primPath, result]() {
        UsdContext::getSelection().setSelectedPaths({primPath});
    });
}

void onUpdate(float dt) {  // 主 thread tick
    SdfChangeBlock block;  // 批次化,只觸發一次 stage notification
    while (auto cmd = g_mainThreadCommandQueue.tryPop()) {
        cmd();
    }
}
```

---

## 雙模式設計:Local CEF / Remote WebRTC(ovrtx + Y framework 版本)

**核心原則**:mode 由 `.app.toml` 決定,extension 互斥載入,Vue codebase 共用 99%。

### 專案結構

```
your_app/
├─ apps/
│  ├─ viewer.local.app.toml       ← 載 CEF extension(自己 build .exe 時讀)
│  ├─ viewer.streaming.app.toml   ← 載 WebRTC extension
│  └─ viewer.shared.app.toml      ← 共用 dependency(Y framework 支援 import)
│
├─ src/
│  ├─ main.cpp                     ← 你 build 的 .exe 主進入點
│  └─ framework/                   ← Y framework 實作
│
└─ exts/
   ├─ your_company.ui.cef/         ← 只在 local 載
   │  ├─ extension.toml            ← Y framework 自定 manifest format
   │  └─ ui_cef.cpp
   ├─ your_company.bridge/         ← 兩個都載(命令 queue / handler registry)
   ├─ your_company.commands/       ← 兩個都載(USD 業務邏輯)
   ├─ your_company.transport.ws/   ← 只在 streaming 載(WebSocket transport)
   └─ <product>.handlers/          ← 產品專屬(BIM / 訓練 / 醫療...)
```

### `.app.toml` 範例(Y framework 自定 format)

```toml
# viewer.shared.app.toml
[app]
name = "Your BIM Viewer"
version = "0.1.0"
window = { width = 1920, height = 1080, title = "BIM Viewer" }

[ovrtx]
usd_scene = "scenes/lighthouse.usdz"
default_camera = "/World/Cameras/Main"

[extensions]
"your_company.bridge" = {}
"your_company.commands" = {}
```

```toml
# viewer.local.app.toml(本地模式)
imports = ["viewer.shared.app.toml"]

[extensions]
"your_company.ui.cef" = {}
```

```toml
# viewer.streaming.app.toml(遠端模式)
imports = ["viewer.shared.app.toml"]

[extensions]
"your_company.transport.ws" = { listen = "0.0.0.0:8211" }
# 注意:ovrtx 渲染輸出走 ovrtx 自己的 streaming API,不再依賴 omni.kit.livestream
# 待 spike 確認 ovrtx 是否內建 streaming 或需要自寫 H.264 encode + WebRTC server
```

兩個 mode 都 build 出獨立 .exe(`viewer.local.exe` / `viewer.streaming.exe`),客戶安裝包根據場景選一個 ship。

### Vue 端 transport 抽象

```typescript
// kitBridge.ts
interface Transport {
  send(cmd: string, payload: any): void
  on(event: string, handler: Function): void
}

class CEFTransport implements Transport { /* window.cefQuery */ }
class WebSocketTransport implements Transport { /* localhost ws */ }

// 啟動時偵測環境
const transport = window.cefQuery 
  ? new CEFTransport()
  : new WebSocketTransport('ws://localhost:8211')
```

兩個模式 Vue 共用 99% codebase,只有最外層 layout 處理差異:
- **Local mode**:UI 是「鏤空的」,中間 transparent 讓 Kit viewport 透出來
- **Remote mode**:UI 中間是 `<video>` 顯示 stream

---

## Extension 分層與跨產品 Reusability

`your_company.ui.cef` 是「公司基礎建設」級的 extension — 之後做別的 Kit 產品要能整包 copy 過去直接用,因此設計上劃成兩層。

### 四層結構與依賴方向

```
─── 跨產品 core(可整包 copy 到別的 ovrtx-based 專案)─────────
your_company.ui.cef        ← CEF + GPU shared texture + 透明 JS bridge
your_company.bridge        ← 抽象命令 queue / handler registry(純 C++)
─── 產品專屬(每個 deployment 一份)──────────────────
<product>.commands         ← 業務命令(BIM / 訓練 / 醫療... 跨不了產品)
<product>.handlers         ← setup,串接 ui.cef ↔ bridge ↔ commands
─── App 層 ────────────────────────────────────────
<product>.app.toml         ← 宣告 dependency,選 mode(local / streaming)
```

### 鐵律:依賴方向只能單向

`product → bridge / ui.cef`,反向永遠不行。grep `ui.cef/` 整包資料夾不應出現任何產品名(`lighthouse` / `bim` 等),否則就是破戒。

```cpp
// ❌ 不可以出現在 ui.cef/ 內
#include "lighthouse/bim_commands.h"
const std::string kEntryUrl = "https://lighthouse-app/...";

// ✅ 配置從產品端 API call 傳進來
ui.createPanel({.entry_url = "...", .viewport = vp, .bounds = ...});
```

### Message bridge 透明 pass-through

`ui.cef` 看到的 message 永遠是 `std::string`,不 parse、不知道 schema、不關心命令名稱。每個產品自己定 JSON 格式、在自己的 commands extension 內處理。

犧牲 type safety 換 reuse —— 在產品端寫薄 wrapper(`LighthouseCommands::sendSelectFloor(int floor)`)補 type 安全,錯誤抓在產品端,不污染 `ui.cef`。

### 對 C++ 進 core 的影響

無。Reusability 是 **code organization rule**(誰 include 誰),不是 **runtime architecture rule**(threading / process / memory)。`ui.cef` 內部 threading model、`bridge` 是純 C++ in-process queue、Python 進場點(commands handler dispatch)都跟原本規劃一致。額外成本是每個 message 一次 `std::string` move,對比 GPU compositing 是噪音等級。

### 驗證方法

在 `examples/hello_cef/` 放一個 50 行 setup,只開一個 panel 載 `https://google.com`。如果這個 minimal product 跑得起來、且 `ui.cef` source folder 內 grep 不到任何 lighthouse 字眼,reusability 就守住了。

---

## Implementation Roadmap(2026-05-08 update,ovrtx-based)

### Phase 0:已完成(2026-05-07 ~ 05-08 探索期)

- [x] `kit-app-template` 起 base app,確認能跑
- [x] 最小 C++ extension(hello world),確認 build 環境(carb / pybind11)
- [x] Smoke test kit 跑通,C++ printf + Python pybind print 兩條 path 都 OK
- [x] Public API 探勘:確認 Kit C++ IRenderer 對 third-party 不可用,pivot 到 ovrtx

### Phase 1:ovrtx 環境驗證(下一步,1–2 週)

- [ ] 從 GitHub clone `NVIDIA-Omniverse/ovrtx`,build 最小範例
- [ ] 跑 `examples/c/minimal` 確認 ovrtx 能載 USD 並渲染到 file
- [ ] 跑 `examples/c/vulkan-interop` 確認 ovrtx 能輸出到外部 Vulkan window
- [ ] 估 RAM / 啟動時間 / install footprint(對比 Kit usd_viewer)
- [ ] 寫 stake-out doc:ovrtx 公開 API 涵蓋 / 不涵蓋哪些功能(輸入 / 輸出 / camera 控制 / picking)

### Phase 2:Y framework MVP(2 週)

- [ ] 設計 `.app.toml` schema(extensions / dependencies / settings 區塊)
- [ ] 寫 `extension_host.cpp`(~300 行):TOML parse、dependency resolve、DLL load、lifecycle 管理
- [ ] 寫 1 個 hello world extension `your_company.hello`,能被 host 載入並印 message
- [ ] 寫 1 個 dependency 範例:extension B 依賴 A,確認載入順序對
- [ ] 整合 ovrtx context:host 在 main loop 內 tick,extensions 能呼叫 ovrtx API

### Phase 3:CEF 整合(3–4 週)

- [ ] CEF non-OSR 模式先讓 browser 起得來(獨立 window 顯示 google.com)
- [ ] 切到 OSR + Vulkan shared NT handle(從 ovrtx vulkan-interop 範例改)
- [ ] 寫 composite shader(GLSL),把 CEF texture overlay 在 ovrtx 輸出 framebuffer
- [ ] **Reusability gate**:`grep -r "lighthouse\|bim" exts/your_company.ui.cef/` 無結果

### Phase 4:命令協定 + 雙模式(2–3 週)

- [ ] CEF MessageRouter 接到 `your_company.bridge` queue
- [ ] Worker thread + main thread command queue + `SdfChangeBlock` 批次
- [ ] 最小 demo:Vue 點按鈕 → 改 USD prim visibility / material
- [ ] WebSocket transport extension(streaming mode 替代品)
- [ ] Vue 端 `Transport` 抽象 layer(local CEF / WebSocket 切換)

### Phase 5:跨平台(2 週)

- [ ] Linux build(X11 / Wayland — Vulkan 自然支援)
- [ ] 對比 Windows / Linux ovrtx 行為差異
- [ ] 跨平台 launcher 腳本

### Phase 6:打包與部署(2 週)

- [ ] Installer 打包(NSIS / WiX / .deb / .AppImage)
- [ ] Splash screen + ovrtx 啟動 UX(預期比 Kit 快 5x:無 microkernel cold start)
- [ ] NVIDIA driver check
- [ ] Single instance lock
- [ ] Firewall rule pre-configure(streaming mode)
- [ ] USD asset 打包策略(內建 vs 客戶側)

### Phase 7:Isaac Sim 能力整合(可選,看客戶需求)

- [ ] Link `ovphysx`,跑物理模擬 spike(碰撞檢測 / 重力)
- [ ] 評估 ovrtx-sensors(lidar / depth camera 模擬,if available)
- [ ] USD 內 Isaac schemas(`ArticulationRootAPI` 等)能不能讀取
- [ ] BIM 客戶情境 demo:火災疏散 / 監視器視野模擬 / 結構分析

---

## 部署 UX 注意事項

**啟動時間**:ovrtx 預計 < 5 秒(無 microkernel cold start;Kit usd_viewer 是 10–30 秒)。Splash screen 顯示有意義進度文字(「初始化 RTX」、「載入 USD 場景」)

**安裝包大小**:ovrtx runtime + USD lib + RTX shaders + CEF 預估 800MB–1.5GB(比 Kit 1–2GB 略小);加 BIM assets 看場景複雜度。差分更新 / online installer 仍建議

**NVIDIA driver 依賴**:RTX GPU + 較新 driver,installer 加 check + 引導更新

**USD asset 策略**:預先打包 vs 客戶側 download。BIM 模型大(數百 MB ~ GB),建議 viewer .exe 跟 USD 場景分離,USD 走 update 機制(USD subLayer / reference 設計天然支援)

**防火牆 prompt**:streaming mode 起 localhost server,installer 階段預先加 firewall rule

**多開保護**:single instance lock,第二次雙擊就 focus 現有視窗,避免兩個 viewer subprocess 爆 GPU

---

## 待釐清議題

### 已解決(2026-05-08 update 後)

- [x] ~~CEF DLL 跟 Kit 內建 Chromium 版本相容性~~ — Kit 不內建 CEF/Chromium,且 pivot 到 ovrtx 後 host 是自寫,完全 control DLL load order
- [x] ~~`kit-app-template` streaming variant 的 .kit file 實際內容~~ — pivot 後改用 `.app.toml`,streaming 機制要重新設計

### 仍待解決

- [ ] **ovrtx vulkan-interop 範例實際 spike**:確認 50% Stage 3 工作真的免費送(待 Phase 1)
- [ ] **ovrtx 是否開放自訂 render task**:不開的話,改用 Vulkan composite shader 在 main loop 後處理(更直接但失去 Hydra task 抽象)
- [ ] **ovrtx streaming 能力**:有沒有內建 H.264 / WebRTC encode,或要自己接 NVENC SDK
- [ ] **Y framework 是否要支援 hot reload**:dev 期方便但 production 不需要,看 spike 後 dev workflow 估價值
- [ ] **Input routing**(滑鼠在 UI 區 vs viewport 區)hit-test 設計 — Vulkan window 自己拿 HWND 後,routing 比 Kit 路徑簡單
- [ ] **Linux Wayland-only 環境**:ovrtx + Vulkan 應該天然支援,但要實測
- [ ] **前端框架選型**(Vue 3 / Svelte 5 / React + shadcn)
- [ ] **Component library 選型**(影響「產品感」)
- [ ] **Isaac Sim 整合範圍**:燈塔 / BIM 案要不要 ovphysx?客戶需求是 viewer 還是 simulator?
- [ ] **內部 USD authoring 工具策略**:用 USD Composer(Kit-based)還是用 Maya/Blender USD plugin?(混合架構的另一半)

---

## 名詞表

| 術語 | 說明 |
|---|---|
| **Kit** | Omniverse 的 microkernel 應用框架(我們**不再使用**,pivot 到 ovrtx) |
| **Extension** | 程式碼模組單位(Kit 有自己的格式,我們的 Y framework 自定 `.app.toml`) |
| **App / .kit file** | Kit 啟動 config(我們改用 `.app.toml`) |
| **ovrtx** | NV 2026 GTC 公開的 RTX 渲染 + USD 載入 C library,獨立可 link,**我們的渲染主軸** |
| **ovphysx** | NV 公開的 PhysX 模擬 C library,Isaac Sim 核心能力之一 |
| **ovstorage** | NV 公開的 USD asset / storage layer C library |
| **ov\* libraries** | NV 2026 開始的 modular library 系列(library-first,非 framework) |
| **usdrt** | NV 維護的 USD runtime 加速層,Fabric Scene Delegate + GPU resident,ovrtx 內建 |
| **Y framework** | 我們自寫的 extension host 框架,概念類似 Kit microkernel 但 format / API 自己 control |
| **`.app.toml`** | Y framework 的 app manifest,取代 Kit 的 `.kit` file |
| **USD** | Universal Scene Description,Pixar 的場景描述格式,**inter-tool contract** |
| **USD Layer / SubLayer** | USD 的資料分層概念,differential / non-destructive update 的核心機制 |
| **USD Variant** | USD 的多版本切換機制(`winter` / `summer` / `phase_1` 等場景配置) |
| **USD Reference** | USD 的模組化引用機制,asset 重用 + 版本升級 |
| **USDZ** | USD 的 archive format(.zip 內裝 USD + textures),適合客戶交付 |
| **Hydra** | USD 的可插拔渲染框架,scene delegate + render delegate |
| **HdTask** | Hydra render task,渲染 pipeline 的擴充點(ovrtx 是否開放給 user 自定 task 待確認) |
| **carb** | Carbonite,NV 的 C++ 底層 framework(Kit + ovrtx 共用) |
| **OSR** | Off-Screen Rendering,CEF 把 UI 畫到 texture 而非視窗 |
| **MTML** | `multi_threaded_message_loop`,CEF 自己起 message loop thread |
| **SdfChangeBlock** | USD 批次編輯 API,合併多次 stage 變更為單次 notification |
| **CEF** | Chromium Embedded Framework,把 Chromium 嵌進 native app |
| **DXGI Shared NT Handle** | Windows GPU 跨 process / 跨 D3D 版本共享 texture 的 handle 機制 |

---

## 前端框架選型備忘(尚未決定)

排除原因:Qwik(resumability 對 desktop 無意義)、Astro(內容站靜態網站)、Next.js / Nuxt(SSR meta-framework)、Angular(企業大團隊強制結構)。

真正候選:

| 選項 | 優勢 | 劣勢 | 適用情境 |
|---|---|---|---|
| **Vue 3**(現用) | 生態熟、PrimeVue/Naive UI 完整 | 無明顯升級空間 | 90% 場景夠用 |
| **Svelte 5** | runes 跟 Vue Composition API 心智近、code 量少、編譯時無 runtime overhead | 生態較小 | 新專案、想升級 DX |
| **SolidJS** | 最快 runtime、fine-grained reactivity 對高頻 prop 更新有實際意義 | 生態小、企業採用度低 | Kit 高頻推送物件狀態的場景 |
| **React + shadcn/ui** | shadcn 視覺領先一個世代、AI 工具支援度最好、TanStack 生態 | switching cost 高、心智負擔重 | high-stakes demo、最高端客戶 |

**結論**:現有專案不換,新專案視客戶等級評估。比換框架更有效的事:升級 component library、設計 token 系統、嚴格 TypeScript。
