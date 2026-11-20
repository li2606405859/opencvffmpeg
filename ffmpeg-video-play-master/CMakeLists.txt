cmake_minimum_required(VERSION 3.10)
project(ffmpeg-video-play)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)

# 根据目标平台设置架构
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH "x64")
else()
    set(ARCH "x86")
endif()

message(STATUS "Target architecture: ${ARCH}")

# 设置库目录
link_directories(
        ${CMAKE_SOURCE_DIR}/lib/SDL2/lib/${ARCH}
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}
        ${CMAKE_SOURCE_DIR}/lib/windows-kits/lib/${ARCH}
)

# 包含目录
include_directories(
        ${CMAKE_SOURCE_DIR}/lib/SDL2/include
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/include
)

# 源文件
set(SOURCES
        main.cpp
        VideoClock.cpp
        VideoRenderer.cpp
        VideoPlayer.cpp
)

# 添加可执行文件
add_executable(${PROJECT_NAME} ${SOURCES})

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/SDL2/lib/${ARCH}/SDL2.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/avcodec.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/avdevice.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/avfilter.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/avformat.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/avutil.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/swresample.lib
        ${CMAKE_SOURCE_DIR}/lib/ffmpeg-4.2.1/lib/${ARCH}/swscale.lib
        ${CMAKE_SOURCE_DIR}/lib/windows-kits/lib/${ARCH}/Ole32.Lib
)

# DLL 文件复制
if(WIN32)
    # 根据架构选择 DLL 文件目录
    set(DLL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/dll/${ARCH}")

    # 获取所有 DLL 文件
    file(GLOB DLL_FILES "${DLL_SOURCE_DIR}/*.dll")

    # 复制 DLL 文件到输出目录
    foreach(DLL_FILE ${DLL_FILES})
        get_filename_component(DLL_NAME ${DLL_FILE} NAME)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_FILE}"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DLL_NAME}"
                COMMENT "Copying ${DLL_NAME} to output directory"
        )
    endforeach()
endif()
