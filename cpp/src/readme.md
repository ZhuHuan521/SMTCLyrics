# SMTCLyrics C++ 源码架构说明

这份 C++ 代码实现的是一个 Windows 桌面歌词工具：从系统 SMTC 媒体会话读取当前播放歌曲，按“本地歌词优先、在线歌词兜底”的策略加载歌词，解析成时间轴，再用透明置顶窗口绘制桌面歌词和高亮进度。

## 一、整体数据流

主流程可以按下面这条链路理解：

```text
WinMain
  -> app::Application
  -> smtc::SmtcProvider 读取当前歌曲、播放状态和进度
  -> lyrics::LyricRepository 查本地 lyrics 目录、查 cache.json、请求在线歌词
  -> lyrics::OnlineLyrics / http::HttpClient 抓取 QQ、酷狗、酷我、网易云歌词
  -> lyrics::LrcParser 解析 LRC/KRC/QRC/YRC
  -> ui::DesktopLyricsWindow 绘制透明桌面歌词
  -> ui::ControlWindow 提供配置和手动操作入口
```

`Application` 是调度中心。它不直接解析网络协议，也不直接画 GDI+ 细节，而是把 SMTC、歌词仓库、解析器和两个窗口串起来。

## 二、模块职责

`main.cpp` 是 Windows GUI 入口，只负责启用高 DPI 感知并启动 `Application::run()`。

`app/` 是应用编排层。`Application` 负责初始化配置和缓存、创建控制窗口与歌词窗口、启动两个定时器、读取 SMTC、判断换歌、异步加载歌词，以及把解析后的歌词帧送到窗口绘制。

`smtc/` 是媒体状态层。`SmtcProvider` 封装 Windows Runtime 的 `GlobalSystemMediaTransportControlsSessionManager`，读取当前播放应用的标题、歌手、播放状态和时间轴，并用本地 `steady_clock` 对播放位置做插值。

`lyrics/` 是歌词核心层。`LyricRepository` 决定从哪里加载歌词；`OnlineLyrics` 处理各平台搜索、下载、解密和格式转换；`LrcParser` 把普通 LRC、酷狗 KRC、QQ QRC、网易云 YRC 统一解析成 `LrcLine` 和 `LyricSegment`；`QrcDecrypter` 专门处理 QQ QRC 的 3DES 解密和 zlib 解压。

`ui/` 是 Win32 界面层。`ControlWindow` 是控制面板，负责配置输入、颜色选择、歌词源检测、换源、清缓存、本地歌词编辑等操作；`DesktopLyricsWindow` 是透明置顶歌词窗口，使用 GDI+ 绘制描边、渐变、阴影和百分比高亮。

`config/` 和 `cache/` 是持久化层。`ConfigStore` 读写 `config.ini`，并兼容旧版中文 section/key；`LyricCache` 读写 `cache.json`，保存每首歌上次成功的歌词源和单曲偏移。

`http/` 和 `util/` 是基础设施。`HttpClient` 是同步 WinHTTP 封装；`util` 提供编码转换、Base64、zlib/Deflate 解压、路径和文件工具。

## 三、启动与运行流程

1. `WinMain` 创建 `Application` 并进入 `run()`。
2. `Application::initialize()` 创建缓存文件、读取配置、创建桌面歌词窗口和控制窗口。
3. 程序启动两个定时器：SMTC 轮询定时器负责读取媒体状态，渲染定时器负责高频推进歌词高亮。
4. `smtcTick()` 读取当前歌曲。如果歌曲变化，调用 `loadLyricsForCurrentTrack()`。
5. 歌词加载在线程中执行，避免网络请求阻塞 UI。线程完成后用 `PostMessageW` 把 `AsyncLyricLoadResult` 交回主线程。
6. 主线程收到歌词后更新 `LrcParser`，之后每个 tick 通过 `parser_.frameAt()` 计算当前歌词帧。
7. `DesktopLyricsWindow::updateLyrics()` 接收文本和高亮百分比，并触发 GDI+ 重绘。

## 四、歌词加载策略

`LyricRepository::loadForKeyword()` 的顺序是：

1. 查本地 `lyrics/<标题 - 歌手>.lrc`。
2. 如果没有精确文件，再在 `lyrics` 目录里做宽松文件名匹配。
3. 如果没有本地歌词，并且没有要求忽略缓存，则读取 `cache.json` 中该歌曲上次成功的在线源。
4. 如果缓存源失败或没有缓存，就按 `AppConfig::sourcePriority` 依次尝试 QQ、酷狗、酷我、网易云。
5. 在线源成功后会写回缓存，下次同一首歌可以更快命中。

用户点击“换源”时，`Application::switchLyricsSource()` 不会永久改配置，而是临时把当前源放到队尾，再强制忽略缓存重新加载。

## 五、歌词解析模型

解析后的核心结构是：

```text
LrcLine
  timeMs       行开始时间
  durationMs   行持续时间
  text         最终显示文本
  segments     词级高亮片段
```

普通 LRC 通常只有行级时间，`segments` 为空，程序只能按整行持续时间估算高亮。KRC、QRC、YRC 带词级时间，解析器会生成 `LyricSegment`，从而支持更准确的逐字/逐词高亮。

`LrcParser::frameAt()` 根据当前播放位置和显示模式输出 `LyricFrame`。显示模式 1 是当前一句，模式 2 是当前句加下一句，模式 3 是上一句加当前句。

## 六、时间同步设计

SMTC 的时间轴更新频率和不同播放器行为并不完全一致，所以代码里有两层同步：

`SmtcProvider` 先把系统返回的原始时间轴整理成 `MediaState::positionMs`。模式 1 更依赖 SMTC 的 `LastUpdatedTime`，模式 2 更依赖本地 `steady_clock` 外推。

`Application` 再用 `lastSmtcPositionMs_`、`lastSmtcTimestampMs_` 和 `lastAcceptedPositionMs_` 做渲染层插值。这样 SMTC 可以低频轮询，但歌词高亮仍能用约 60fps 的渲染定时器平滑推进。

全局歌词偏移 `config_.lyricOffsetMs` 和单曲偏移 `currentSongOffsetMs_` 会叠加成 `totalOffsetMs()`，用于修正解析帧时的播放位置。

## 七、界面与绘制

控制窗口是传统 Win32 子控件布局，所有按钮和输入框通过 `ControlId` 枚举分组管理。它不直接操作歌词仓库或 SMTC，而是通过 `ControlWindowCallbacks` 把操作交给 `Application`。

桌面歌词窗口是 `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW` 的透明置顶窗口。绘制时先在 32 位 DIB 后备缓冲上用 GDI+ 画文字路径，再用 `UpdateLayeredWindow` 一次性提交到屏幕。高亮效果通过裁剪区域叠加高亮画刷实现。

## 八、建议阅读顺序

1. 先读 `main.cpp` 和 `app/Application.h`，建立程序入口和成员关系。
2. 再读 `Application::initialize()`、`smtcTick()`、`loadLyricsForCurrentTrack()`，理解主流程。
3. 读 `lyrics/LyricRepository.cpp`，理解歌词从本地、缓存、在线源之间如何切换。
4. 读 `lyrics/LrcParser.cpp`，重点看 `parseUtf8()` 和 `frameAt()`。
5. 最后读 `ui/DesktopLyricsWindow.cpp` 和 `ui/ControlWindow.cpp`，理解窗口消息、绘制和配置交互。

如果只想先跑通核心逻辑，优先关注 `Application + LyricRepository + LrcParser` 这三块；如果想学习 Windows 桌面绘制，再深入 `DesktopLyricsWindow`。
