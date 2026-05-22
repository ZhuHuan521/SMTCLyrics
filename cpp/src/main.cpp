#include "app/Application.h"

#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    smtc::app::Application app;
    return app.run();
}
