# SMTCLyrics

SMTCLyrics 是一个 Windows 桌面歌词显示工具。它通过 SMTC（System Media Transport Controls）或 Apple Music 内部接口读取当前媒体的歌曲名、歌手、播放状态和进度，优先加载本地 `.lrc` 歌词，找不到时再从在线歌词源获取同步歌词，并用透明置顶窗口显示桌面歌词。

本仓库包含早期易语言工程 `e/` 与当前的 C++20 工程 `cpp/`；部分配置、缓存和本地歌词会在 CMake 构建后复制到可执行文件目录。

## 功能特性

- 自动读取支持 SMTC 的播放器信息，包括标题、歌手、播放/暂停状态和播放进度。
- 可选 Apple Music 内部模式，通过注入 bridge DLL 直接读取 Windows 版 Apple Music 的内部播放状态，无需额外注册 Windows 身份包。
- bridge DLL 在构建时嵌入到 `SMTCLyrics.exe` 资源中，运行时自动释放到临时目录并注入 Apple Music 进程，退出时自动卸载。
- 支持本地歌词优先，默认读取运行目录下的 `lyrics/`。
- 支持 QQ 音乐、酷狗音乐、酷我音乐、网易云音乐，以及需要本机凭证的 Apple Music 官方逐字歌词源。
- Apple Music 专用模式优先使用 bridge 读取的 catalog ID 精确取词，普通 SMTC 模式经歌名、艺人、时长和版本校验；失败后继续下一歌词源。
- “操作与检测”中的“显示/隐藏翻译”可在当前音源的原文和译文之间切换；没有独立译文时状态栏提示“音源无翻译”。
- 支持普通 LRC，以及 QRC、KRC、YRC、Apple TTML 等带逐字时间轴的歌词格式。
- 支持卡拉 OK 式逐字高亮，渲染定时器约 60 FPS，用于平滑高亮动画。
- 支持单行、当前行加下一行、上一行加当前行等显示模式，双行模式下第二行使用独立的渐变颜色。
- 控制窗口可调整字体、颜色、渐变、显示模式、歌词源优先级、SMTC 模式和轮询间隔。
- 支持手动检测、加载或卸载 Apple Music bridge。
- 支持桌面歌词窗口置顶显示、拖动定位、锁定位置和窗口位置记忆。
- 支持全局歌词偏移和单曲偏移记忆。
- 在线源按配置的优先级依次尝试；逐行歌词可回退，存在逐字歌词时优先显示逐字歌词。
- 歌词加载在后台线程执行，避免在线请求或 QRC/KRC 解析阻塞界面。
- 控制窗口提供歌词源可用性检测，一键探测五个在线源是否仍可返回可解析歌词。
- Apple Music Token 由用户在控制窗口或本地 `config.ini` 的 `[AppleMusic]` 段填写，不会自动抓取或写入 Git。

## 项目结构

```text
SMTCLyrics/
|-- cpp/                  # 当前 C++ 主工程
|   |-- src/
|   |   |-- app/          # 应用入口、播放轮询、渲染定时、歌词加载调度
|   |   |-- applemusic/   # Apple Music bridge DLL 注入与内部播放状态读取
|   |   |-- cache/        # 歌词源和单曲偏移缓存
|   |   |-- config/       # INI 配置读写与旧配置迁移
|   |   |-- http/         # WinHTTP 封装
|   |   |-- lyrics/       # 歌词获取、解密和解析
|   |   |-- playback/     # 播放状态结构体 MediaState
|   |   |-- smtc/         # SMTC 媒体会话读取
|   |   |-- ui/           # 控制窗口和桌面歌词窗口
|   |   `-- util/         # 编码、路径、Base64、Inflate 等工具
|   |-- tests/            # 核心逻辑测试
|   |-- generated/        # C++/WinRT 生成头文件
|   |-- cmake/            # CMake 辅助脚本
|   |-- CMakeLists.txt
|   `-- CMakePresets.json
|-- e/                    # 早期易语言工程、配置、缓存、本地歌词资源
|-- e-packager-master/    # 易语言解包工具及 nlohmann/json 头文件
`-- README.md
```

## 构建要求

- Windows 10 或 Windows 11
- CMake 3.22+
- 支持 C++20 的编译器
- MinGW（仓库已提供 `mingw-debug`、`mingw-release` 和 `mingw-size` 预设）

工程使用的主要系统库包括 `gdiplus`、`winhttp`、`bcrypt`、`crypt32`、`shlwapi`、`runtimeobject`、`windowsapp`、`ole32`、`shell32`、`comdlg32`、`advapi32`、`Shcore` 等。第三方 JSON 依赖来自 `e-packager-master/thirdparty/json.hpp`，WinRT 投影头文件已放在 `cpp/generated/` 中。

## 构建方法

从仓库根目录进入 `cpp/` 后构建：

```powershell
cd cpp
cmake --preset mingw-debug
cmake --build --preset mingw-debug
```

Release 构建：

```powershell
cd cpp
cmake --preset mingw-release
cmake --build --preset mingw-release
```

体积优化构建（开启 LTO、strip 符号）：

```powershell
cd cpp
cmake --preset mingw-size
cmake --build --preset mingw-size
```

常见产物位置：

```text
cpp/build/mingw-debug/SMTCLyrics.exe
cpp/build/mingw-debug/SMTCLyricsAppleMusicBridge.dll
cpp/build/mingw-debug/SMTCLyricsTests.exe
cpp/build/mingw-release/SMTCLyrics.exe
cpp/build/mingw-release/SMTCLyricsAppleMusicBridge.dll
cpp/build/mingw-release/SMTCLyricsTests.exe
```

构建 `SMTCLyrics` 和 `SMTCLyricsTests` 后，CMake 会把 `e/config.ini`、`e/cache.json` 和 `e/lyrics/` 复制到对应可执行文件目录，方便直接运行。

## 运行

运行构建目录下的 `SMTCLyrics.exe`。启动后保持一个支持 SMTC 的音乐播放器正在播放，程序会自动识别当前曲目并加载歌词。

运行目录中常见文件：

```text
SMTCLyrics.exe       # 主程序（内置 Apple Music bridge 资源）
SMTCLyricsTests.exe  # 核心测试程序
config.ini           # 字体、颜色、窗口、歌词源和 SMTC 配置
cache.json           # 歌词源、单曲偏移和翻译显示偏好缓存
lyrics/              # 本地歌词目录
```

Release 版 `SMTCLyrics.exe` 已将 Apple Music bridge DLL 作为资源嵌入。运行时程序会把 bridge 释放到当前用户临时目录后加载到 Apple Music 进程中；同目录 `SMTCLyricsAppleMusicBridge.dll` 仅作为开发构建产物和资源缺失时的回退，不是发布时必需文件。正常退出 SMTCLyrics 时会通知 bridge 从 Apple Music 进程卸载。MinGW 的 C++/GCC/Winpthread 运行库已静态链接，不需要额外携带 `libstdc++-6.dll`、`libgcc_s_seh-1.dll` 或 `libwinpthread-1.dll`。

发布版程序位于 [cpp/release/AppleMusicLyrics/SMTCLyrics.exe](cpp/release/AppleMusicLyrics/SMTCLyrics.exe)。Apple Music 内部模式即使在程序启动时尚未建立 bridge 管道，也会在首次读取播放状态时自动加载 bridge；已有 bridge 模块但暂不响应时不会反复卸载和注入。

当前发布版为 [GitHub Release v1.4.1](https://github.com/ZhuHuan521/SMTCLyrics/releases/tag/v1.4.1)。下载的 `SMTCLyrics.exe` 大小为 674,318 字节，SHA256 为 `5812AFD9027D40F59ACB7A26E670B90BC11AC8EDF22C551A6D7B12C74BFC4EF2`。该附件按你提供的 `cpp1/SMTCLyrics.exe` 发布，使用封装启动器，内部携带未加壳程序。

如果没有检测到 SMTC 媒体会话或 Apple Music 内部播放信息，桌面歌词窗口会显示等待提示。在线歌词源依赖第三方服务接口和网络状态，接口失效时建议优先使用本地歌词。

## 本地歌词

程序优先查找 `lyrics/` 目录中的本地 `.lrc` 文件。精确文件名建议使用程序生成的关键字格式：

```text
歌曲标题 - 歌手.lrc
```

例如：

```text
夜空中最亮的星 - 逃跑计划.lrc
```

如果没有精确匹配，程序会忽略空格、短横线、下划线和点号做一次简单模糊匹配。控制窗口中的"打开本地歌词"会按当前曲目在 `lyrics/` 中创建或打开对应 `.lrc` 文件。

## 在线歌词源

歌词源编号与默认优先级如下：

1. QQ 音乐
2. 酷狗音乐
3. 酷我音乐
4. 网易云音乐
5. Apple Music 官方（需在控制窗口填写两项凭证）

QQ 音乐会优先尝试 QRC 逐字歌词；酷狗音乐会优先尝试 KRC；网易云音乐会优先尝试 YRC，失败后回退到普通 LRC。自动加载和自动播放换歌不会写来源缓存；只有手动“换源”选择了来源、对当前歌曲保存了微调偏移，或手动切换翻译显示时，才会写入 `cache.json`。

先检查原文歌词中的中日韩字符比例。外文歌直接显示原文；原文至少 75% 为中日韩文字时，依次请求 `zh-Hans-CN`、`zh-Hant-TW`、`zh-Hant-HK`，没有中文歌词则使用原文。音源返回的原文和译文分别解析为两套 `LrcLine`，按照各自的时间轴切换显示。切换翻译时只替换当前显示行，不改变 `v1.4` 的当前行查找和逐字高亮逻辑。点击“显示/隐藏翻译”会切换当前显示并使用缓存中的手动偏好；切换到没有独立译文的来源时，状态栏提示“音源无翻译”。

仅返回逐行 TTML 时继续寻找逐字歌词；如果所有源都只有逐行歌词，则回到配置中优先级最高的可用来源。点击“换源”会强制切换至下一个来源，不会因逐行/逐字优先级而跳过目标来源。

## 配置说明

配置文件位于运行目录下的 `config.ini`。程序会读取旧易语言版本的中文配置段，并在保存时迁移为当前英文配置键。

示例：

```ini
[Font]
name=Microsoft YaHei
size=35
bold=true
italic=false
underline=false

[Lyrics]
offsetMs=0
normalColor1=#FF0000
normalColor2=#FFFF00
normalBorderColor=#000000
normalGradientMode=1
highlightColor1=#0080FF
highlightColor2=#00FFFF
highlightBorderColor=#000000
highlightGradientMode=1
highlight2Color1=#808080
highlight2Color2=#C0C0C0
highlight2BorderColor=#000000
highlight2GradientMode=1

[Sources]
priority1=1
priority2=2
priority3=3
priority4=4
priority5=5

[SMTC]
mode=1
pollIntervalMs=1000

[Display]
mode=1

[Window]
left=0
top=0
width=1200
height=150

[AppleMusic]
Authorization=
Media-User-Token=
地区=
```

关键配置：

- `Lyrics.offsetMs`：全局歌词偏移，单位毫秒。
- `Lyrics.normal*`：未高亮歌词的渐变颜色和描边。
- `Lyrics.highlight*`：当前高亮歌词的渐变颜色和描边。
- `Lyrics.highlight2*`：双行显示模式下第二行的渐变颜色和描边。
- `Sources.priority1` 到 `priority5`：在线歌词源优先级，`1=QQ`，`2=酷狗`，`3=酷我`，`4=网易云`，`5=Apple Music 官方`。
- `SMTC.mode`：播放信息来源/同步模式，`1=SMTC1`，`2=SMTC2`，`3=Apple Music 内部`。
- `SMTC.pollIntervalMs`：SMTC 轮询间隔，程序会限制在 `500` 到 `2000` 毫秒。
- `Display.mode`：显示模式，`1=单行`，`2=当前行+下一行`，`3=上一行+当前行`。
- `Window`：歌词窗口位置和尺寸，通常通过控制窗口拖动保存。
- `AppleMusic.Authorization`：控制窗口「Authorization」输入框对应的凭证值。
- `AppleMusic.Media-User-Token`：控制窗口「Media-User-Token」输入框对应的凭证值，通常以 `0.Av` 开头。
- `AppleMusic.地区`：可选的 Apple Music 地区代码（例如 `cn`、`tr`）。留空时请求会依据凭证读取账号地区，但自动获取的地区不写入 `config.ini`。手动填写后按填写值请求。此段由控制窗口保存，请勿把真实凭证提交到 Git。

## 测试

先完成 Debug 或 Release 构建，再运行对应测试程序：

```powershell
cd cpp
.\build\mingw-debug\SMTCLyricsTests.exe
```

或：

```powershell
cd cpp
.\build\mingw-release\SMTCLyricsTests.exe
```

测试覆盖 LRC/KRC/QRC/YRC/TTML 解析、QRC 解密、zlib inflate、配置迁移、缓存容错、自动加载不写来源等核心逻辑。

## 开发备注

- 主入口是 `cpp/src/main.cpp`，应用主循环在 `cpp/src/app/Application.cpp`。
- `main.cpp` 同时包含 Apple Music bridge 探测的子命令入口，通过命令行开关区分。
- 歌词加载由后台线程完成，并通过 `WM_APP` 消息回到主线程更新状态。
- `LyricRepository` 的加载顺序是：本地歌词 -> 缓存的在线来源 -> 当前配置的在线来源优先级。
- 桌面歌词窗口使用 GDI+ 绘制透明置顶窗口，并根据逐字时间轴计算高亮百分比。
- 当前开发主线在 `cpp/`；早期易语言软件位于 `e/`，第三方 JSON 头文件位于 `e-packager-master/thirdparty/`。
