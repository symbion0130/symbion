cmake_minimum_required(VERSION 3.24)

project(SymbionNativeShell VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Symbion native shell currently targets Windows/WebView2 only.")
endif()

option(SYMBION_NATIVE_ENABLE_WEBVIEW2 "Enable WebView2 host code when the SDK headers are available" ON)
set(SYMBION_NATIVE_DEFAULT_URL "http://127.0.0.1:8000/" CACHE STRING "Default URL loaded by the thin native shell")
set(SYMBION_WEBVIEW2_SDK_ROOT "" CACHE PATH "Optional Microsoft.Web.WebView2 NuGet package root")

add_executable(symbion_native WIN32
    src/main.cpp
    src/symbion_shell.cpp
    src/symbion_shell.h
)

target_compile_features(symbion_native PRIVATE cxx_std_20)
target_compile_definitions(symbion_native PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    SYMBION_NATIVE_DEFAULT_URL=L\"${SYMBION_NATIVE_DEFAULT_URL}\"
)

if(MSVC)
    target_compile_options(symbion_native PRIVATE /W4 /permissive-)
else()
    target_compile_options(symbion_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(symbion_native PRIVATE
    ole32
    shell32
    user32
    winhttp
)

if(SYMBION_NATIVE_ENABLE_WEBVIEW2)
    set(_webview2_roots)
    if(SYMBION_WEBVIEW2_SDK_ROOT)
        list(APPEND _webview2_roots "${SYMBION_WEBVIEW2_SDK_ROOT}")
    endif()
    if(DEFINED ENV{WEBVIEW2_SDK_ROOT})
        list(APPEND _webview2_roots "$ENV{WEBVIEW2_SDK_ROOT}")
    endif()

    foreach(_root IN LISTS _webview2_roots)
        if(EXISTS "${_root}/build/native/include/WebView2.h")
            target_include_directories(symbion_native PRIVATE "${_root}/build/native/include")
            set(_webview2_loader_dir "${_root}/build/native/${CMAKE_VS_PLATFORM_NAME}")
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_webview2_loader_dir "${_root}/build/native/x64")
            elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
                set(_webview2_loader_dir "${_root}/build/native/x86")
            endif()
            if(EXISTS "${_webview2_loader_dir}/WebView2Loader.dll")
                add_custom_command(TARGET symbion_native POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_webview2_loader_dir}/WebView2Loader.dll"
                        "$<TARGET_FILE_DIR:symbion_native>/WebView2Loader.dll"
                )
            endif()
            break()
        elseif(EXISTS "${_root}/include/WebView2.h")
            target_include_directories(symbion_native PRIVATE "${_root}/include")
            break()
        endif()
    endforeach()
endif()
