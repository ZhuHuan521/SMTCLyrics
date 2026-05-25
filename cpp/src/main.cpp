#include "app/Application.h"

#include <windows.h>
#include <shellscalingapi.h>

#pragma comment(lib, "Shcore.lib")

// Windows GUI 程序入口：先启用高 DPI 感知，再把生命周期交给应用层。
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 开启逐显示器 DPI 感知，避免高分屏上歌词窗口发虚。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    smtc::app::Application app;
    return app.run();
}
