cmake_minimum_required(VERSION 3.22)

project(SMTCLyricsCpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

set(SMTCLYRICS_SOURCES
    src/app/Application.cpp
    src/cache/Cache.cpp
    src/config/Config.cpp
    src/http/HttpClient.cpp
    src/lyrics/LrcParser.cpp
    src/lyrics/LyricRepository.cpp
    src/lyrics/OnlineLyrics.cpp
    src/lyrics/QrcDecrypter.cpp
    src/smtc/SmtcProvider.cpp
    src/ui/ControlWindow.cpp
    src/ui/DesktopLyricsWindow.cpp
    src/util/Base64.cpp
    src/util/Encoding.cpp
    src/util/Inflate.cpp
    src/util/Path.cpp
)

add_library(smtclyrics_core STATIC ${SMTCLYRICS_SOURCES})
target_include_directories(smtclyrics_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/../e-packager-master/thirdparty
)
target_link_libraries(smtclyrics_core PUBLIC gdiplus winhttp crypt32 shlwapi runtimeobject windowsapp ole32 shell32 comdlg32)

add_executable(SMTCLyrics WIN32 src/main.cpp resource.rc)
target_link_libraries(SMTCLyrics PRIVATE smtclyrics_core)

add_executable(SMTCLyricsTests tests/core_tests.cpp)
target_link_libraries(SMTCLyricsTests PRIVATE smtclyrics_core)

add_custom_command(TARGET SMTCLyrics POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../e/config.ini" "$<TARGET_FILE_DIR:SMTCLyrics>/config.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../e/cache.json" "$<TARGET_FILE_DIR:SMTCLyrics>/cache.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../e/lyrics" "$<TARGET_FILE_DIR:SMTCLyrics>/lyrics"
)

add_custom_command(TARGET SMTCLyricsTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../e/config.ini" "$<TARGET_FILE_DIR:SMTCLyricsTests>/config.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../e/cache.json" "$<TARGET_FILE_DIR:SMTCLyricsTests>/cache.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../e/lyrics" "$<TARGET_FILE_DIR:SMTCLyricsTests>/lyrics"
)
