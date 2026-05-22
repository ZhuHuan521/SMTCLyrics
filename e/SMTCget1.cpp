#include <iostream>
#include <fstream>
#include <Windows.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.h>
#include <string>
#include <chrono>
#include <windows.h>

using namespace std;
using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Foundation;
using namespace std::chrono;
#pragma comment( linker, "/subsystem:\"windows\" /entry:\"mainCRTStartup\"" )
int main() {
    init_apartment();
    while (true) {
        std::ofstream file("C:\\1.txt", std::ios::out | std::ios::trunc); // 每次循环时都打开文件并清空内容
        if (!file.is_open()) {
            cerr << "Unable to open file." << endl;
            return -1;
        }

        auto smtcsm = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto currentSession = smtcsm.GetCurrentSession();

        if (currentSession) {
            auto mediaProperties = currentSession.TryGetMediaPropertiesAsync().get();
            auto mediaPlaybackInfo = currentSession.GetTimelineProperties();
            if (mediaProperties && mediaPlaybackInfo) {
                std::wstring albumArtistWStr = mediaProperties.AlbumArtist().c_str();
                auto position = mediaPlaybackInfo.Position();
                auto duration = mediaPlaybackInfo.MaxSeekTime();
                auto positionSeconds = std::chrono::duration_cast<std::chrono::seconds>(position).count();
                auto durationSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                std::wstring titleWStr = mediaProperties.Title().c_str();

                int albumArtistUtf8Length = WideCharToMultiByte(CP_UTF8, 0, albumArtistWStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
                int titleUtf8Length = WideCharToMultiByte(CP_UTF8, 0, titleWStr.c_str(), -1, nullptr, 0, nullptr, nullptr);

                std::string albumArtistUtf8(albumArtistUtf8Length, '\0');
                std::string titleUtf8(titleUtf8Length, '\0');

                WideCharToMultiByte(CP_UTF8, 0, albumArtistWStr.c_str(), -1, &albumArtistUtf8[0], albumArtistUtf8Length, nullptr, nullptr);
                WideCharToMultiByte(CP_UTF8, 0, titleWStr.c_str(), -1, &titleUtf8[0], titleUtf8Length, nullptr, nullptr);

                // Remove the extra null character
                albumArtistUtf8.pop_back();
                titleUtf8.pop_back();

                file << "Artist: " << albumArtistUtf8 << ", Title: " << titleUtf8 << ", Time: " << positionSeconds << "/" << durationSeconds << " seconds" << std::endl;
                file.close(); // 关闭文件
            }
        }
        else {
            cerr << "No active media session found." << endl;
            file.close();
            return -1;
        }

        Sleep(700);
    }

    return 0;
}
