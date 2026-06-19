cmake_minimum_required(VERSION 3.22)

project(SMTCLyricsCpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SMTCLYRICS_ENABLE_LTO "Enable link-time optimization for non-debug builds" ON)
option(SMTCLYRICS_STRIP_RELEASE "Strip release executables after linking" ON)

if(SMTCLYRICS_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SMTCLYRICS_IPO_SUPPORTED OUTPUT SMTCLYRICS_IPO_ERROR LANGUAGES CXX)
    if(SMTCLYRICS_IPO_SUPPORTED AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(CMAKE_CXX_COMPILER_AR)
            set(CMAKE_AR "${CMAKE_CXX_COMPILER_AR}" CACHE FILEPATH "Archiver" FORCE)
        endif()
        if(CMAKE_CXX_COMPILER_RANLIB)
            set(CMAKE_RANLIB "${CMAKE_CXX_COMPILER_RANLIB}" CACHE FILEPATH "Ranlib" FORCE)
        endif()
    endif()
else()
    set(SMTCLYRICS_IPO_SUPPORTED OFF)
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

set(SMTCLYRICS_SOURCES
    src/applemusic/AppleMusicInternalProvider.cpp
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
target_link_libraries(smtclyrics_core PUBLIC gdiplus winhttp bcrypt crypt32 shlwapi runtimeobject windowsapp ole32 shell32 comdlg32 advapi32)

add_library(SMTCLyricsAppleMusicBridge SHARED src/applemusic/AppleMusicBridgeProbe.cpp)
target_include_directories(SMTCLyricsAppleMusicBridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(SMTCLyricsAppleMusicBridge PRIVATE runtimeobject ole32 advapi32)
set_target_properties(SMTCLyricsAppleMusicBridge PROPERTIES PREFIX "" OUTPUT_NAME "SMTCLyricsAppleMusicBridge")

set(SMTCLYRICS_EMBEDDED_BRIDGE_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAppleMusicBridge.rc")
add_custom_command(
    OUTPUT "${SMTCLYRICS_EMBEDDED_BRIDGE_RESOURCE}"
    COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT=${SMTCLYRICS_EMBEDDED_BRIDGE_RESOURCE}"
            -DRESOURCE_ID=102
            "-DBRIDGE_DLL=$<TARGET_FILE:SMTCLyricsAppleMusicBridge>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteEmbeddedBridgeResource.cmake"
    DEPENDS SMTCLyricsAppleMusicBridge
    VERBATIM
)

add_executable(SMTCLyrics WIN32 src/main.cpp resource.rc "${SMTCLYRICS_EMBEDDED_BRIDGE_RESOURCE}")
target_link_libraries(SMTCLyrics PRIVATE smtclyrics_core)
add_dependencies(SMTCLyrics SMTCLyricsAppleMusicBridge)

add_executable(SMTCLyricsTests tests/core_tests.cpp)
target_link_libraries(SMTCLyricsTests PRIVATE smtclyrics_core)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(target SMTCLyrics SMTCLyricsTests SMTCLyricsAppleMusicBridge)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
endif()

foreach(target smtclyrics_core SMTCLyrics SMTCLyricsTests SMTCLyricsAppleMusicBridge)
    target_compile_options(${target} PRIVATE
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang>>:-ffunction-sections>
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang>>:-fdata-sections>
    )
    if(SMTCLYRICS_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
endforeach()

foreach(target SMTCLyrics SMTCLyricsTests SMTCLyricsAppleMusicBridge)
    target_link_options(${target} PRIVATE
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang>>:-Wl,--gc-sections>
    )
endforeach()

if(SMTCLYRICS_STRIP_RELEASE AND CMAKE_STRIP AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    foreach(target SMTCLyrics SMTCLyricsTests SMTCLyricsAppleMusicBridge)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_STRIP}" --strip-all "$<TARGET_FILE:${target}>"
        )
    endforeach()
endif()

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
