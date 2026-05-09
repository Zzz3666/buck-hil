#=============================================================================
# CMakeLists.txt — Buck HIL 上位机 (Qt 6 / C++17)
#
# 构建:
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   cmake --build . --parallel $(nproc)
#
# 依赖:
#   Qt 6.5+ (Widgets + Network)
#   QCustomPlot (bundled in 3rdparty/)
#=============================================================================
cmake_minimum_required(VERSION 3.20)
project(BuckHilHost VERSION 1.0.0 LANGUAGES CXX)

# ---- C++ 标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Qt 自动处理 ----
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---- Qt 依赖 ----
find_package(Qt6 REQUIRED COMPONENTS Widgets Network PrintSupport)

# ---- QCustomPlot (单文件静态库) ----
# 获取方式:
#   https://www.qcustomplot.com/release/2.1.1fixed/QCustomPlot-source.tar.gz
# 解压后将 qcustomplot.h 和 qcustomplot.cpp 放入 3rdparty/qcustomplot/
if(EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/qcustomplot/qcustomplot.cpp")
    add_library(qcustomplot STATIC
        3rdparty/qcustomplot/qcustomplot.cpp
    )
    target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)
    target_include_directories(qcustomplot PUBLIC 3rdparty/qcustomplot)
    # MinGW: single-file QCustomPlot has 30K+ sections → need big-obj
    if(MINGW)
        target_compile_options(qcustomplot PRIVATE -Wa,-mbig-obj)
    endif()
else()
    message(WARNING "QCustomPlot not found in 3rdparty/qcustomplot/")
    message(STATUS "  Download: https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz")
    message(STATUS "  Extract to: host/3rdparty/qcustomplot/")
endif()

# ---- 源文件 ----
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ParameterPanel.cpp
    src/CommunicationWorker.cpp
    src/FrameParser.cpp
    src/DataProcessor.cpp
)

set(HEADERS
    src/MainWindow.h
    src/ParameterPanel.h
    src/CommunicationWorker.h
    src/FrameParser.h
    src/DataProcessor.h
    src/RingBuffer.h
    protocol/Protocol.h
)

# ---- 可执行文件 ----
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Network
)

if(TARGET qcustomplot)
    target_link_libraries(${PROJECT_NAME} PRIVATE qcustomplot)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

# ---- 编译选项 ----
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- 安装 ----
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# ---- 平台特定 ----
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

# ---- 打印配置摘要 ----
message(STATUS "============================================")
message(STATUS "  Buck HIL Host Application")
message(STATUS "  Version:   ${PROJECT_VERSION}")
message(STATUS "  Qt:        ${Qt6_DIR}")
message(STATUS "  Standard:  C++${CMAKE_CXX_STANDARD}")
message(STATUS "  QCustomPlot: $<IF:$<TARGET_EXISTS:qcustomplot>,FOUND,NOT FOUND>")
message(STATUS "============================================")
