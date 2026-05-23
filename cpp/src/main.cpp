#include "app/Application.h"

#include <windows.h>
#include <shellscalingapi.h>

#pragma comment(lib, "Shcore.lib")

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Enable DPI awareness for crisp rendering on high-DPI displays
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    smtc::app::Application app;
    return app.run();
}
