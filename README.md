# SMTCLyrics

SMTCLyrics 是一个 Windows 桌面歌词显示工具。它通过 SMTC（System Media Transport Controls）读取当前媒体会话的歌曲名、歌手、播放状态和进度，优先加载本地 `.lrc` 歌词，找不到时再从在线歌词源获取同步歌词，并用透明置顶窗口显示桌面歌词。

本仓库当前以 `cpp/` 下的 C++20 工程为主；`e/` 中保留了早期易语言工程与运行资源，部分配置、缓存和本地歌词会在 CMake 构建后复制到可执行文件目录。

## 功能特性

- 自动读取支持 SMTC 的播放器信息，包括标题、歌手、播放/暂停状态和播放进度。
- 支持本地歌词优先，默认读取运行目录或当前目录下的 `lyrics/`。
- 支持 QQ 音乐、酷狗音乐、酷我音乐、网易云音乐等在线歌词源。
- 支持普通 LRC，以及 QRC、KRC、YRC 等带逐字时间轴的歌词格式。
- 支持卡拉 OK 式逐字高亮，渲染定时器约 60 FPS，用于平滑高亮动画。
- 支持单行、当前行加下一行、上一行加当前行等显示模式。
- 控制窗口可调整字体、颜色、渐变、显示模式、歌词源优先级、SMTC 模式和轮询间隔。
- 支持桌面歌词窗口置顶显示、拖动定位、锁定位置和窗口位置记忆。
- 支持全局歌词偏移和单曲偏移记忆。
- 支持歌词源缓存：同一首歌再次播放时会优先尝试上次成功的在线来源。
- 歌词加载在后台线程执行，避免在线请求或 QRC/KRC 解析阻塞界面。

## 项目结构

```text
SMTCLyrics/
|-- cpp/                  # 当前 C++ 主工程
|   |-- src/
|   |   |-- app/          # 应用入口、定时器、歌词加载调度
|   |   |-- cache/        # 歌词源和单曲偏移缓存
|   |   |-- config/       # INI 配置读写与旧配置迁移
|   |   |-- http/         # WinHTTP 封装
|   |   |-- lyrics/       # 歌词获取、解密和解析
|   |   |-- smtc/         # SMTC 媒体会话读取
|   |   |-- ui/           # 控制窗口和桌面歌词窗口
|   |   `-- util/         # 编码、路径、Base64、Inflate 等工具
|   |-- tests/            # 核心逻辑测试
|   |-- generated/        # C++/WinRT 生成头文件
|   |-- CMakeLists.txt
|   `-- CMakePresets.json
|-- e/                    # 早期易语言工程、配置、缓存、本地歌词资源
|-- e-packager-master/    # 易语言工程解包工具及 nlohmann/json 头文件
|-- qrckit-master/        # QQ 音乐 QRC 解密解析参考实现
|-- LDDC-main/            # 歌词处理相关参考项目
|-- 1.jpg
|-- 2.jpg
`-- README.md
```

## 构建要求

- Windows 10 或 Windows 11
- CMake 3.22+
- 支持 C++20 的编译器
- MinGW（仓库已提供 `mingw-debug` 和 `mingw-release` 预设）

工程使用的主要系统库包括 `gdiplus`、`winhttp`、`bcrypt`、`crypt32`、`shlwapi`、`runtimeobject`、`windowsapp`、`ole32`、`shell32`、`comdlg32` 等。第三方 JSON 依赖来自 `e-packager-master/thirdparty/json.hpp`，WinRT 投影头文件已放在 `cpp/generated/` 中。

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

常见产物位置：

```text
cpp/build/mingw-debug/SMTCLyrics.exe
cpp/build/mingw-debug/SMTCLyricsTests.exe
cpp/build/mingw-release/SMTCLyrics.exe
cpp/build/mingw-release/SMTCLyricsTests.exe
```

构建 `SMTCLyrics` 和 `SMTCLyricsTests` 后，CMake 会把 `e/config.ini`、`e/cache.json` 和 `e/lyrics/` 复制到对应可执行文件目录，方便直接运行。

## 运行

运行构建目录下的 `SMTCLyrics.exe`。启动后保持一个支持 SMTC 的音乐播放器正在播放，程序会自动识别当前曲目并加载歌词。

运行目录中常见文件：

```text
SMTCLyrics.exe       # 主程序
SMTCLyricsTests.exe  # 核心测试程序
config.ini           # 字体、颜色、窗口、歌词源和 SMTC 配置
cache.json           # 歌词源缓存和单曲偏移缓存
lyrics/              # 本地歌词目录
```

如果没有检测到 SMTC 媒体会话，桌面歌词窗口会显示等待提示。在线歌词源依赖第三方服务接口和网络状态，接口失效时建议优先使用本地歌词。

## 本地歌词

程序优先查找 `lyrics/` 目录中的本地 `.lrc` 文件。精确文件名建议使用程序生成的关键字格式：

```text
歌曲标题 - 歌手.lrc
```

例如：

```text
夜空中最亮的星 - 逃跑计划.lrc
```

如果没有精确匹配，程序会忽略空格、短横线、下划线和点号做一次简单模糊匹配。控制窗口中的“打开本地歌词”会按当前曲目在 `lyrics/` 中创建或打开对应 `.lrc` 文件。

## 在线歌词源

歌词源编号与默认优先级如下：

1. QQ 音乐
2. 酷狗音乐
3. 酷我音乐
4. 网易云音乐

QQ 音乐会优先尝试 QRC 逐字歌词；酷狗音乐会优先尝试 KRC；网易云音乐会优先尝试 YRC，失败后回退到普通 LRC。成功的在线来源会写入 `cache.json`，下次播放同一首歌时优先使用该来源。

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
```

关键配置：

- `Lyrics.offsetMs`：全局歌词偏移，单位毫秒。
- `Sources.priority1` 到 `priority4`：在线歌词源优先级，`1=QQ`，`2=酷狗`，`3=酷我`，`4=网易云`。
- `SMTC.mode`：SMTC 进度同步模式，取值 `1` 或 `2`。
- `SMTC.pollIntervalMs`：SMTC 轮询间隔，程序会限制在 `500` 到 `2000` 毫秒。
- `Display.mode`：显示模式，`1=单行`，`2=当前行+下一行`，`3=上一行+当前行`。
- `Window`：歌词窗口位置和尺寸，通常通过控制窗口拖动保存。

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

测试覆盖 LRC/KRC/QRC/YRC 解析、QRC 解密、zlib inflate、配置迁移与缓存容错等核心逻辑。

## 开发备注

- 主入口是 `cpp/src/main.cpp`，应用主循环在 `cpp/src/app/Application.cpp`。
- 歌词加载由后台线程完成，并通过 `WM_APP` 消息回到主线程更新状态。
- `LyricRepository` 的加载顺序是：本地歌词 -> 缓存的在线来源 -> 当前配置的在线来源优先级。
- 桌面歌词窗口使用 GDI+ 绘制透明置顶窗口，并根据逐字时间轴计算高亮百分比。
- 当前仓库保留多个参考目录，开发主线请优先关注 `cpp/`。
