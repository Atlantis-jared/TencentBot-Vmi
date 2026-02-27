cmake_minimum_required(VERSION 3.20)
project(untitled)
enable_language(ASM_MASM)

# 修正 MASM 编码警告：仅对 CXX 应用 /utf-8
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置 Windows SDK 版本（确保支持 Graphics Capture API）
set(CMAKE_SYSTEM_VERSION 10.0.19041.0)
# 自动查找依赖
find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)

add_executable(untitled
        main.cpp
        dumper.cpp
        hv.asm
        src/TencentBot.cpp
        src/CheckpointStore.cpp
        src/DxgiWindowCapture.cpp
        src/CaptchaEngine.cpp
        src/GameMemory.cpp
        src/VisionEngine.cpp
)

add_subdirectory(./IbInputSimulator/Simulator)


target_link_libraries(untitled PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
        CURL::libcurl
        IbInputSimulator
        Microsoft::CppWinRT
        # Graphics Capture API 需要的额外库
        d3d11.lib
        dxgi.lib
        dwmapi.lib
        windowsapp.lib
)

# 设置编译选项
target_compile_options(untitled PRIVATE
        /await:strict  # 启用协程支持 (C++/WinRT 需要)
        /EHsc          # 启用 C++ 异常处理
)

# 设置编译定义
target_compile_definitions(untitled PRIVATE
        WINRT_LEAN_AND_MEAN
        NOMINMAX
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
)

# 自动拷贝 IbInputSimulator 的 DLL
add_custom_command(TARGET untitled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:IbInputSimulator>
        $<TARGET_FILE_DIR:untitled>
)
# 编译完成后拷贝资源目录到输出目录（源目录;目标子目录名）
set(ASSET_SRCS assets/maps assets/npc assets/mapsui)
set(ASSET_DSTS maps npcs mapsui)
list(LENGTH ASSET_SRCS N_ASSETS)
math(EXPR N_ASSETS_LAST "${N_ASSETS} - 1")
foreach(i RANGE ${N_ASSETS_LAST})
    list(GET ASSET_SRCS ${i} SRC)
    list(GET ASSET_DSTS ${i} DST)
    add_custom_command(TARGET untitled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
        "$<TARGET_FILE_DIR:untitled>/${DST}"
    )
endforeach()