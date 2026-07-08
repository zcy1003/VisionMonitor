cmake_minimum_required(VERSION 3.19)
project(VisionMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network SerialPort Sql)

# OpenCV 的安装路径在 Windows/Linux 上差异较大，因此只把本机
# D:/OpenCV 作为 Windows 兜底值；Linux 或其他机器应通过
# -DOpenCV_DIR=... 或 -DVISIONMONITOR_OPENCV_DIR=... 显式传入。
set(VISIONMONITOR_OPENCV_DIR "" CACHE PATH "Optional OpenCV package directory")
if (VISIONMONITOR_OPENCV_DIR)
    set(OpenCV_DIR "${VISIONMONITOR_OPENCV_DIR}" CACHE PATH "OpenCV package directory" FORCE)
elseif (WIN32 AND NOT OpenCV_DIR AND EXISTS "D:/OpenCV/opencv/build/OpenCVConfig.cmake")
    set(OpenCV_DIR "D:/OpenCV/opencv/build" CACHE PATH "OpenCV package directory" FORCE)
endif()
find_package(OpenCV REQUIRED)

qt_standard_project_setup()

qt_add_executable(VisionMonitor
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    camerathread.cpp
    camerathread.h
    serialthread.cpp
    serialthread.h
    databasemanager.cpp
    databasemanager.h
    visiondetector.cpp
    visiondetector.h
)

# CameraProbe 用于独立验证当前平台的 OpenCV 摄像头后端是否能返回
# 非空帧，避免主程序只凭 isOpened() 误判摄像头可用。
add_executable(CameraProbe
    tools/camera_probe.cpp
)

target_link_libraries(VisionMonitor
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::SerialPort
        Qt::Sql
        ${OpenCV_LIBS}
)

target_link_libraries(CameraProbe
    PRIVATE
        ${OpenCV_LIBS}
)

if (WIN32)
    # Windows 下复制 Qt 运行时，方便 Qt Creator 或资源管理器直接启动。
    # Linux 通常由系统包、rpath 或 AppImage/linuxdeployqt 处理运行时部署。
    add_custom_command(TARGET VisionMonitor POST_BUILD
        COMMAND Qt6::windeployqt
                --no-translations
                --dir "$<TARGET_FILE_DIR:VisionMonitor>"
                "$<TARGET_FILE:VisionMonitor>"
        COMMENT "Deploying Qt runtime files for VisionMonitor"
    )

    if (OpenCV_DIR MATCHES "[/\\\\]opencv[/\\\\]build$")
        set(OpenCV_RUNTIME_DIR "${OpenCV_DIR}/x64/vc16/bin")
        set(OpenCV_WORLD_DLL
            "${OpenCV_RUNTIME_DIR}/opencv_world${OpenCV_VERSION_MAJOR}${OpenCV_VERSION_MINOR}${OpenCV_VERSION_PATCH}$<$<CONFIG:Debug>:d>.dll"
        )
        # 只对官方 Windows OpenCV 预编译包执行 DLL 复制；其他安装方式
        # 目录结构可能不同，强行复制会破坏跨机器配置。
        add_custom_command(TARGET VisionMonitor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${OpenCV_WORLD_DLL}"
                    "$<TARGET_FILE_DIR:VisionMonitor>"
            COMMENT "Copying OpenCV runtime DLL for VisionMonitor"
        )
    endif()
endif()

if (MSVC)
    # MSVC 默认源码编码受系统区域影响，显式使用 UTF-8 保证中文界面文本
    # 和注释在 Windows 上编译、调试时不乱码。
    target_compile_options(VisionMonitor PRIVATE /utf-8)
    target_compile_options(CameraProbe PRIVATE /utf-8)
endif()

install(TARGETS VisionMonitor
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET VisionMonitor
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
