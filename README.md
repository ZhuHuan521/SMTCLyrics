# SMTCLyrics

SMTCLyrics 是一个 Windows 桌面歌词显示工具。它通过 SMTC（System Media Transport Controls）读取当前播放的歌曲信息和进度，自动从多个在线音乐源获取歌词，并以桌面悬浮窗的方式实时同步显示。

## 功能特性

- 自动读取当前播放器的歌曲名、歌手、播放状态和播放进度。
- 支持本地歌词和在线歌词源：QQ 音乐、酷狗、酷我、网易云音乐。
- 支持普通 LRC 歌词和逐字歌词。
- QQ 音乐源支持 QRC 精确逐字歌词解析。
- 酷狗源支持 KRC 逐字歌词解析。
- 歌词解析在后台线程执行，解析 QRC/KRC 时不会卡住控制窗口。
- 解析期间歌词窗口会显示“正在解析歌词...”。
- 支持桌面歌词悬浮窗、置顶显示、拖动定位、字体与颜色配置。
- 支持单行、当前行加下一行、上一行加当前行等显示模式。
- 支持全局歌词偏移和单曲偏移记忆。
- 支持歌词源缓存，下次播放同一首歌时优先使用上次成功的来源。

## 项目结构

```text
SMTCLyrics/
├─ cpp/                  # C++ 主程序
│  ├─ src/
│  │  ├─ app/            # 应用主逻辑
│  │  ├─ cache/          # 歌词源和偏移缓存
│  │  ├─ config/         # 配置读写
│  │  ├─ http/           # WinHTTP 客户端
│  │  ├─ lyrics/         # 歌词获取、解密、解析
│  │  ├─ smtc/           # SMTC 媒体会话读取
│  │  ├─ ui/             # 控制窗口和桌面歌词窗口
│  │  └─ util/           # 编码、路径、压缩等工具
│  └─ tests/             # 核心测试
├─ e/                    # 易语言原工程相关内容
├─ e-packager-master/    # 易语言工程解包工具和第三方 json.hpp
└─ qrckit-master/        # QQ 音乐 QRC 解密解析参考实现
```

## 构建要求

- Windows 10 或 Windows 11
- CMake 3.22+
- MinGW 或 MSVC
- 支持 C++20 的编译器

## 构建方法

进入 `cpp` 目录后构建：

```powershell
cd cpp
cmake --preset mingw-debug
cmake --build build/mingw-debug
```

Release 构建：

```powershell
cd cpp
cmake --preset mingw-release
cmake --build build/mingw-release
```

构建产物位于：

```text
cpp/build/mingw-debug/SMTCLyrics.exe
cpp/build/mingw-release/SMTCLyrics.exe
```

## 运行

运行对应构建目录下的 `SMTCLyrics.exe`。启动后播放支持 SMTC 的音乐播放器，程序会自动识别当前歌曲并加载歌词。

常见运行文件：

```text
SMTCLyrics.exe       # 主程序
SMTCLyricsTests.exe  # 核心测试
config.ini          # 配置文件
cache.json          # 歌词源和单曲偏移缓存
lyrics/             # 本地歌词目录
```

## 本地歌词

程序会优先读取运行目录下的 `lyrics/` 文件夹。歌词文件建议命名为：

```text
歌手 歌名.lrc
```

如果没有精确匹配，程序会尝试做简单的模糊匹配。

## 在线歌词源

默认支持以下来源：

1. QQ 音乐
2. 酷狗音乐
3. 酷我音乐
4. 网易云音乐

其中 QQ 音乐会优先尝试获取 QRC 精确逐字歌词；酷狗会优先尝试 KRC 逐字歌词。如果逐字歌词不可用，则回退到普通 LRC。

## 配置说明

配置文件为运行目录下的 `config.ini`。常见配置项包括：

```ini
[Font]
name=Microsoft YaHei
size=36
bold=1
italic=0
underline=0

[Lyrics]
globalOffsetMs=0

[Sources]
priority1=1
priority2=2
priority3=3
priority4=4

[SMTC]
mode=1
pollIntervalMs=1000

[Display]
mode=2
```

说明：

- `globalOffsetMs`：全局歌词偏移，单位毫秒。
- `priority1` 到 `priority4`：歌词源优先级，`1=QQ`，`2=酷狗`，`3=酷我`，`4=网易云`。
- `mode`：SMTC 读取模式。
- `Display.mode`：歌词显示模式。

更多字体、颜色、窗口位置等设置可以通过控制窗口调整。

## 测试

```powershell
cd cpp
.\build\mingw-debug\SMTCLyricsTests.exe
```

或：

```powershell
cd cpp
.\build\mingw-release\SMTCLyricsTests.exe
```

## 说明

QQ 音乐 QRC 解密和解析逻辑参考了目录下的 `qrckit-master`。当前 C++ 主程序已经将其关键流程移植到 `cpp/src/lyrics/` 中，用于获取和显示 QQ 音乐源的精确逐字歌词。
