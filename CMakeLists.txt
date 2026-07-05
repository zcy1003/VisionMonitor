cmake_minimum_required(VERSION 3.19)
project(VisionMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Charts SerialPort)

set(OpenCV_DIR "D:/OpenCV/opencv/build")
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
    visiondetector.cpp
    visiondetector.h
)

target_link_libraries(VisionMonitor
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Charts
        Qt::SerialPort
        ${OpenCV_LIBS}
)

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
