# SMTCLyrics

Windows 桌面歌词显示工具，通过 SMTC (System Media Transport Controls) 获取当前播放信息，自动从多个在线音乐平台获取歌词，并以卡拉OK风格实时同步显示在桌面上。

## 功能特性

- **SMTC 媒体感知** — 自动获取当前播放的歌曲信息（歌手、歌名、播放进度），支持两种轮询模式
- **多源歌词获取** — 支持 QQ音乐、酷狗、酷我、网易云音乐，按优先级自动切换
- **桌面歌词悬浮窗** — 透明置顶窗口，支持卡拉OK逐字高亮、渐变着色
- **丰富自定义** — 字体、颜色（支持2/3色渐变）、描边、显示模式（单行/双行）、全局/单曲偏移量
- **歌词缓存** — 记住每首歌的歌词来源和偏移设置，下次播放自动匹配
- **本地歌词** — 支持从 `lyrics/` 目录读取 `.lrc` 文件，模糊匹配歌名

## 截图

<!-- TODO: 添加截图 -->

## 构建

### 环境要求

- Windows 10/11
- CMake 3.22+
- MinGW (g++ with C++20) 或 MSVC

### 编译

```bash
cd cpp
cmake --preset mingw-debug      # 或 mingw-release
cmake --build build/mingw-debug
```

编译产物位于 `cpp/build/mingw-debug/`：

| 文件 | 说明 |
|------|------|
| `SMTCLyrics.exe` | 主程序 |
| `SMTCLyricsTests.exe` | 单元测试 |
| `config.ini` | 配置文件 |
| `cache.json` | 歌词缓存 |

### 运行

直接双击 `SMTCLyrics.exe`，播放任意音乐后程序会自动获取并显示歌词。

## 配置说明

程序目录下的 `config.ini` 控制各项设置：

```ini
[Font]
Name=微软雅黑          ; 字体名称
Size=36                ; 字体大小
Bold=1                 ; 粗体
Italic=0               ; 斜体

[Lyrics]
GlobalOffset=0         ; 全局偏移量 (毫秒)

[Sources]
Priority=qq,kugou,kuwo,netease   ; 歌词源优先级

[SMTC]
Mode=1                 ; SMTC 模式 (1=轮询, 2=事件驱动)
Interval=1000          ; 轮询间隔 (毫秒)

[Display]
Mode=1                 ; 显示模式 (0=单行, 1=当前+下一句, 2=上一句+当前)
```

颜色、窗口位置等更多设置可通过程序内的控制面板界面调整。

## 项目结构

```
SMTCLyrics/
├── cpp/                        # C++ 主程序
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── src/
│   │   ├── main.cpp
│   │   ├── app/                # 应用主逻辑
│   │   ├── ui/                 # 界面 (控制面板 + 桌面歌词窗)
│   │   ├── lyrics/             # 歌词解析与在线获取
│   │   ├── smtc/               # SMTC 媒体会话集成
│   │   ├── http/               # HTTP 客户端
│   │   ├── config/             # 配置读写
│   │   ├── cache/              # 缓存管理
│   │   └── util/               # 工具函数
│   └── tests/
├── e/                          # 易语言原版源码
└── e-packager-master/          # 易语言文件解包工具
```

## 技术栈

- **C++20** / Win32 API / WinRT
- **GDI+** — 文字渲染（渐变、描边、抗锯齿）
- **WinHTTP** — 在线歌词获取
- **nlohmann/json** — JSON 解析
- **CMake** — 构建系统

## 许可证

<!-- TODO: 选择许可证 -->
