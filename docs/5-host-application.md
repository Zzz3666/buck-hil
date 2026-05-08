# 5. 上位机软件设计 (Qt 6 / C++)

## 5.1 技术选型

| 组件 | 选择 | 理由 |
|------|------|------|
| UI 框架 | Qt 6.5+ (Widgets) | 跨平台成熟稳定，原生 C++ 无运行时依赖 |
| 图形库 | QCustomPlot 2.1+ | 轻量（两个文件），高速刷新，BSD 协议 |
| 网络 | QTcpSocket (异步) | Qt 原生异步 socket，天然配合事件循环 |
| 线程 | QThread + 信号槽 | QueuedConnection 自动跨线程安全，无需手写锁 |
| 数据存储 | QFile + QDataStream | 异步 I/O，二进制紧凑格式 |
| 日志 | qInstallMessageHandler | 自带，重定向到文件 |
| 配置 | QSettings (INI) | 跨平台，零依赖 |
| 构建 | CMake 3.20+ | 跨平台，Vivado/Vitis 生态也认 |

## 5.2 线程模型（铁律）

三个 QObject 各住一个线程，用信号槽 QueuedConnection 通信，**UI 线程零阻塞**。

```
┌──────────────────────────────────────────────────────────────────┐
│                  Main Thread (GUI Thread)                         │
│  QApplication::exec() 事件循环                                    │
│                                                                   │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌─────────────┐   │
│  │MainWindow │  │QCustomPlot│  │ParamPanel │  │StatusBar    │   │
│  │.ui        │  │波形控件    │  │QLineEdit  │  │QLabel       │   │
│  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘  └──────┬──────┘   │
│        │              │              │               │           │
│  ┌─────┴──────────────┴──────────────┴───────────────┴─────┐    │
│  │           QTimer (33ms) → updatePlot()                  │    │
│  │           从 DataProcessor 拉取最新数据到 QCustomPlot     │    │
│  │           绝不：socket->read()、文件 I/O、sleep()         │    │
│  └───────────────────────────┬─────────────────────────────┘    │
│                              │                                   │
│   emit sendCommand(cmd, payload)  ──(QueuedConnection)──►        │
│   emit startStream()               ──(QueuedConnection)──►        │
│                              │                                   │
└──────────────────────────────┼───────────────────────────────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
       QueuedConnection  QueuedConnection  QueuedConnection
              │                │                │
              ▼                ▼                ▼
┌──────────────────────┐ ┌──────────────────────────────────────┐
│  CommWorker          │ │  DataProcessor (QThread)              │
│  (QThread)           │ │                                       │
│                      │ │  • RingBuffer<double> 管理             │
│  ┌────────────────┐  │ │  • 触发数据重组 (SEQ排序)              │
│  │ QTcpSocket     │  │ │  • 文件存储 (QFile)                    │
│  │ 异步连接/重连   │  │ │  • 降采样 (长期显示)                  │
│  └───────┬────────┘  │ └───────────────┬──────────────────────┘ │
│          │           │                 │                         │
│  ┌───────┴────────┐  │    QueuedConnection                      │
│  │ FrameParser    │  │                 │                         │
│  │ 逐字节解析      │──┼──► newDataReady(Vout, IL)                │
│  │ 状态机         │  │                 │                         │
│  └───────┬────────┘  │                 ▼                         │
│          │           │         [RingBuffer<double>]              │
│    frameReady()      │         [RingBuffer<double>]              │
│    → 按CMD分发:      │         [FileStorage]                     │
│    • 0x10→emit      │                                           │
│      paramResp      │                                           │
│    • 0x11→emit      │                                           │
│      trigData       │                                           │
│    • 0x12→emit      │                                           │
│      streamData     │                                           │
│                      │                                           │
│  绝不: UI操作、      │                                           │
│  阻塞式 waitFor*()   │                                           │
└──────────────────────┘                                           │
```

### 信号连接拓扑

```
MainWindow                 CommWorker                  DataProcessor
    │                          │                            │
    │──sendCommand()──────────►│                            │
    │                          │──paramResp()─────────────►│
    │                          │                            │
    │◄──statusChanged()────────│                            │
    │◄──connectionError()──────│                            │
    │                          │                            │
    │                          │──trigData()───────────────►│
    │                          │──streamData()─────────────►│
    │                          │                            │
    │◄──plotDataReady()────────┼────────────────────────────│
    │◄──triggerReady()─────────┼────────────────────────────│
    │                          │                            │
    │──startStream()──────────►│                            │
    │──stopStream()───────────►│                            │
    │──armTrigger()───────────►│                            │
```

## 5.3 关键类设计

### 5.3.1 FrameParser — 帧解析状态机

```cpp
// FrameParser.h
#pragma once
#include <QObject>
#include <QByteArray>
#include <cstdint>
#include <functional>

struct Frame {
    uint8_t  cmd;
    QByteArray payload;

    bool isValid() const { return cmd != 0xFF; }
    QByteArray serialize() const;  // 计算 CRC16 并组装完整帧
};

class FrameParser : public QObject
{
    Q_OBJECT
public:
    explicit FrameParser(QObject *parent = nullptr);

    /// 逐字节喂入，内部维护状态机。
    /// 解析到完整帧后发射 frameReady 信号。
    void feedByte(uint8_t byte);
    void feedBytes(const QByteArray &data);

signals:
    void frameReady(Frame frame);

private:
    enum State { Sync1, Sync2, Cmd, LenH, LenL, Payload, CrcH, CrcL };
    State m_state = Sync1;

    uint8_t  m_cmd = 0;
    uint16_t m_len = 0;
    uint16_t m_payloadIdx = 0;
    uint8_t  m_payload[65536]{};
    uint16_t m_crcCalc = 0;
    uint16_t m_crcRecv = 0;

    void reset();
    static uint16_t crc16Init();
    static uint16_t crc16Update(uint16_t crc, uint8_t byte);

    static constexpr uint16_t CRC16_POLY = 0x1021;
};

// FrameParser.cpp (关键实现)
void FrameParser::feedByte(uint8_t byte)
{
    switch (m_state) {
    case Sync1:
        if (byte == 0xAA) m_state = Sync2;
        break;
    case Sync2:
        if (byte == 0x55) {
            m_state = Cmd;
            m_crcCalc = crc16Init();
            m_crcCalc = crc16Update(m_crcCalc, byte);
        } else {
            m_state = Sync1;
        }
        break;
    case Cmd:
        m_cmd = byte;
        m_crcCalc = crc16Update(m_crcCalc, byte);
        m_state = LenH;
        break;
    case LenH:
        m_len = static_cast<uint16_t>(byte) << 8;
        m_crcCalc = crc16Update(m_crcCalc, byte);
        m_state = LenL;
        break;
    case LenL:
        m_len |= byte;
        m_crcCalc = crc16Update(m_crcCalc, byte);
        m_payloadIdx = 0;
        m_state = (m_len == 0) ? CrcH : Payload;
        break;
    case Payload:
        m_payload[m_payloadIdx++] = byte;
        m_crcCalc = crc16Update(m_crcCalc, byte);
        if (m_payloadIdx >= m_len) m_state = CrcH;
        break;
    case CrcH:
        m_crcRecv = static_cast<uint16_t>(byte) << 8;
        m_state = CrcL;
        break;
    case CrcL: {
        m_crcRecv |= byte;
        // 帧尾 0x55 由调用者在下一字节检查后决定
        // 这里简化为: CRC 匹配即认为帧有效
        if (m_crcCalc == m_crcRecv) {
            QByteArray payload(reinterpret_cast<char*>(m_payload), m_len);
            emit frameReady(Frame{m_cmd, payload});
        }
        m_state = Sync1;  // 等待下一个帧尾或下一个帧头
        break;
    }
    }
}

QByteArray Frame::serialize() const
{
    QByteArray frame;
    frame.append(static_cast<char>(0xAA));
    frame.append(static_cast<char>(0x55));
    frame.append(static_cast<char>(cmd));

    uint16_t len = static_cast<uint16_t>(payload.size());
    frame.append(static_cast<char>(len >> 8));
    frame.append(static_cast<char>(len & 0xFF));

    // CRC 计算范围: HEAD2 ~ payload 末尾
    uint16_t crc = 0xFFFF;
    auto update = [&crc](uint8_t b) {
        crc ^= static_cast<uint16_t>(b) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    };

    update(0x55);
    update(cmd);
    update(len >> 8);
    update(len & 0xFF);
    for (auto b : payload) update(static_cast<uint8_t>(b));

    frame.append(static_cast<char>(crc >> 8));
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>(0x55));

    return frame;
}
```

### 5.3.2 CommWorker — 通信线程

```cpp
// CommunicationWorker.h
#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include "FrameParser.h"

class CommWorker : public QObject
{
    Q_OBJECT
public:
    explicit CommWorker(QObject *parent = nullptr);
    ~CommWorker();

public slots:
    /// 连接到目标 (由 MainWindow 通过 QueuedConnection 调用)
    void connectToHost(const QString &host, quint16 port);
    void disconnectFromHost();

    /// 发送命令帧
    void sendFrame(Frame frame);

    /// 控制数据流
    void startStream();
    void stopStream();

signals:
    void connected();
    void disconnected();
    void connectionError(const QString &error);

    // 解析后的帧 → DataProcessor (QueuedConnection)
    void paramResponse(uint16_t id, uint32_t value);
    void triggerData(quint32 seq, QByteArray rawData);
    void streamData(quint32 seq, QByteArray rawData);
    void errorEvent(uint8_t code, QString message);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError error);
    void onReconnectTimer();

private:
    QTcpSocket  *m_socket;
    FrameParser  m_parser;
    QTimer      *m_reconnectTimer;
    QString      m_host;
    quint16      m_port;
    bool         m_intentionalDisconnect = false;

    void dispatchFrame(const Frame &frame);
};

// CommunicationWorker.cpp (关键)
void CommWorker::onReadyRead()
{
    QByteArray data = m_socket->readAll();
    m_parser.feedBytes(data);
}

void CommWorker::dispatchFrame(const Frame &frame)
{
    switch (frame.cmd) {
    case 0x10: { // CMD_PARAM_RESP
        if (frame.payload.size() >= 6) {
            uint16_t id = (static_cast<uint8_t>(frame.payload[0]) << 8)
                        | static_cast<uint8_t>(frame.payload[1]);
            uint32_t val = (static_cast<uint8_t>(frame.payload[2]) << 24)
                         | (static_cast<uint8_t>(frame.payload[3]) << 16)
                         | (static_cast<uint8_t>(frame.payload[4]) << 8)
                         | static_cast<uint8_t>(frame.payload[5]);
            emit paramResponse(id, val);
        }
        break;
    }
    case 0x11: // CMD_TRIG_DATA
        emit triggerData(0 /* TODO: extract seq */, frame.payload);
        break;
    case 0x12: // CMD_STREAM_DATA
        emit streamData(0, frame.payload);
        break;
    case 0xFF: // CMD_ERROR
        if (frame.payload.size() >= 2) {
            emit errorEvent(frame.payload[0],
                QString::fromUtf8(frame.payload.mid(1)));
        }
        break;
    }
}

// 连接在 MainWindow 构造时建立
void MainWindow::setupConnections()
{
    // CommWorker → DataProcessor (跨线程，自动 QueuedConnection)
    connect(m_commWorker, &CommWorker::streamData,
            m_dataProcessor, &DataProcessor::onStreamData);
    connect(m_commWorker, &CommWorker::triggerData,
            m_dataProcessor, &DataProcessor::onTriggerData);
    connect(m_commWorker, &CommWorker::paramResponse,
            m_dataProcessor, &DataProcessor::onParamResponse);

    // DataProcessor → MainWindow (跨线程)
    connect(m_dataProcessor, &DataProcessor::plotDataReady,
            this, &MainWindow::updatePlot);
    connect(m_dataProcessor, &DataProcessor::triggerReady,
            this, &MainWindow::onTriggerReady);

    // MainWindow → CommWorker (跨线程，sendCommand 是 slot)
    connect(this, &MainWindow::sendCommand,
            m_commWorker, &CommWorker::sendFrame);
}
```

### 5.3.3 RingBuffer — 高速环形缓冲

```cpp
// RingBuffer.h
#pragma once
#include <vector>
#include <atomic>
#include <span>

/// 线程安全的预分配环形缓冲。
/// 追加不分配内存，供高速波形数据暂存。
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_buffer(capacity), m_capacity(capacity), m_writePos(0) {}

    /// 追加数据（线程安全）
    void append(std::span<const T> data) {
        size_t writeIdx = m_writePos.load(std::memory_order_relaxed) % m_capacity;
        size_t len = data.size();

        if (writeIdx + len <= m_capacity) {
            std::copy(data.begin(), data.end(), m_buffer.begin() + writeIdx);
        } else {
            size_t firstPart = m_capacity - writeIdx;
            std::copy(data.begin(), data.begin() + firstPart,
                      m_buffer.begin() + writeIdx);
            std::copy(data.begin() + firstPart, data.end(),
                      m_buffer.begin());
        }
        m_writePos.fetch_add(len, std::memory_order_release);
    }

    /// 获取最近 N 个数据点
    std::vector<T> getRecent(size_t count) const {
        size_t pos = m_writePos.load(std::memory_order_acquire);
        size_t start = (pos >= count) ? (pos - count) : 0;
        start %= m_capacity;

        std::vector<T> result(count);
        if (start + count <= m_capacity) {
            std::copy(m_buffer.begin() + start,
                      m_buffer.begin() + start + count, result.begin());
        } else {
            size_t firstPart = m_capacity - start;
            std::copy(m_buffer.begin() + start, m_buffer.end(), result.begin());
            std::copy(m_buffer.begin(), m_buffer.begin() + (count - firstPart),
                      result.begin() + firstPart);
        }
        return result;
    }

    size_t totalWritten() const {
        return m_writePos.load(std::memory_order_acquire);
    }

private:
    std::vector<T>       m_buffer;
    size_t               m_capacity;
    std::atomic<size_t>  m_writePos;
};
```

### 5.3.4 MainWindow — QCustomPlot 波形显示

```cpp
// MainWindow.h
#pragma once
#include <QMainWindow>
#include "qcustomplot.h"
#include "RingBuffer.h"

class CommWorker;
class DataProcessor;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

signals:
    void sendCommand(Frame frame);

public slots:
    void updatePlot();
    void onTriggerReady(quint32 seq);

private:
    void setupUi();
    void setupThreads();
    void setupConnections();

    // UI
    QCustomPlot *m_plotVout;
    QCustomPlot *m_plotIL;

    // 线程对象
    QThread      *m_commThread;
    QThread      *m_procThread;
    CommWorker   *m_commWorker;
    DataProcessor *m_dataProcessor;

    // 渲染定时器
    QTimer       *m_renderTimer;
};

// MainWindow.cpp (关键)
void MainWindow::setupThreads()
{
    // 通信线程
    m_commThread = new QThread(this);
    m_commWorker = new CommWorker();  // 无 parent，手动 moveToThread
    m_commWorker->moveToThread(m_commThread);
    m_commThread->start();

    // 数据处理线程
    m_procThread = new QThread(this);
    m_dataProcessor = new DataProcessor();
    m_dataProcessor->moveToThread(m_procThread);
    m_procThread->start();

    // 清理
    connect(m_commThread, &QThread::finished, m_commWorker, &QObject::deleteLater);
    connect(m_procThread, &QThread::finished, m_dataProcessor, &QObject::deleteLater);
}

void MainWindow::setupUi()
{
    // === QCustomPlot 配置 ===

    // Vout 通道
    m_plotVout = new QCustomPlot(this);
    m_plotVout->addGraph();
    m_plotVout->graph(0)->setPen(QPen(QColor("#00BCD4"), 1.5));
    m_plotVout->xAxis->setLabel("采样点");
    m_plotVout->yAxis->setLabel("Vout (V)");
    m_plotVout->yAxis->setRange(0, 15);
    m_plotVout->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plotVout->axisRect()->setRangeZoom(Qt::Horizontal);

    // 性能优化
    m_plotVout->setNotAntialiasedElements(QCP::aeAll);   // 关闭抗锯齿
    m_plotVout->setOpenGl(false);                         // 不用 OpenGL (兼容性)
    m_plotVout->plotLayout()->setAutoMargins(false);
    m_plotVout->plotLayout()->setMargins(QMargins(0,0,0,0));

    // I_L 通道
    m_plotIL = new QCustomPlot(this);
    // ... 同上配置

    // 布局: 上下两个 Plot
    auto *layout = new QVBoxLayout;
    layout->addWidget(m_plotVout, 3);
    layout->addWidget(m_plotIL, 2);

    auto *central = new QWidget(this);
    central->setLayout(layout);
    setCentralWidget(central);

    // 渲染定时器 33ms → ~30 FPS
    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, &MainWindow::updatePlot);
    m_renderTimer->start(33);
}

void MainWindow::updatePlot()
{
    // 在 UI 线程执行，只读缓冲 → 生成 QCPGraphData
    // 绝不做 socket I/O 或任何阻塞操作

    auto voutData = m_dataProcessor->voutBuffer().getRecent(10000);
    auto ilData   = m_dataProcessor->ilBuffer().getRecent(10000);

    // 双缓冲更新 (QCustomPlot 的 replot 内部双缓冲)
    QVector<QCPGraphData> voutPoints(voutData.size());
    for (size_t i = 0; i < voutData.size(); i++) {
        voutPoints[i].key = static_cast<double>(i);
        voutPoints[i].value = voutData[i];
    }
    m_plotVout->graph(0)->data()->set(voutPoints, true);
    m_plotVout->replot(QCustomPlot::rpQueuedReplot);

    QVector<QCPGraphData> ilPoints(ilData.size());
    for (size_t i = 0; i < ilData.size(); i++) {
        ilPoints[i].key = static_cast<double>(i);
        ilPoints[i].value = ilData[i];
    }
    m_plotIL->graph(0)->data()->set(ilPoints, true);
    m_plotIL->replot(QCustomPlot::rpQueuedReplot);
}
```

### 5.3.5 DataProcessor — 数据处理线程

```cpp
// DataProcessor.h
#pragma once
#include <QObject>
#include "RingBuffer.h"

class DataProcessor : public QObject
{
    Q_OBJECT
public:
    explicit DataProcessor(QObject *parent = nullptr);

    RingBuffer<double> &voutBuffer() { return m_ringVout; }
    RingBuffer<double> &ilBuffer()   { return m_ringIL; }

public slots:
    void onStreamData(quint32 seq, QByteArray rawData);
    void onTriggerData(quint32 seq, QByteArray rawData);
    void onParamResponse(uint16_t id, uint32_t value);

signals:
    void plotDataReady();          // → MainWindow::updatePlot()
    void triggerReady(quint32 seq);

private:
    RingBuffer<double> m_ringVout{10'000'000};  // 10M points
    RingBuffer<double> m_ringIL{10'000'000};

    void parseDataBlock(const QByteArray &raw, std::vector<double> &vout,
                        std::vector<double> &il, std::vector<double> &duty);
};

void DataProcessor::onStreamData(quint32 /*seq*/, QByteArray rawData)
{
    std::vector<double> vout, il, duty;
    parseDataBlock(rawData, vout, il, duty);

    m_ringVout.append(vout);
    m_ringIL.append(il);

    emit plotDataReady();
}
```

## 5.4 参数配置面板

```cpp
// ParameterPanel.h
#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>

class ParameterPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ParameterPanel(QWidget *parent = nullptr);

signals:
    /// 参数变化 → MainWindow 转发到 CommWorker
    void paramChanged(uint16_t id, uint32_t value);

private slots:
    void onInductanceChanged();
    void onCapacitanceChanged();
    void onLoadResistanceChanged();
    void onVinChanged();

private:
    QLineEdit *m_editL;       // 电感 (μH)
    QLineEdit *m_editC;       // 电容 (μF)
    QLineEdit *m_editRLoad;   // 负载 (Ω)
    QLineEdit *m_editVin;     // 输入电压 (V)
    QLineEdit *m_editFsw;     // 开关频率 (kHz)

    void setupUi();
    uint32_t toRawValue(const QString &text, double scale);
};

void ParameterPanel::onInductanceChanged()
{
    bool ok;
    double val = m_editL->text().toDouble(&ok);
    if (ok && val > 0) {
        // μH → nH
        uint32_t raw = static_cast<uint32_t>(val * 1000);
        emit paramChanged(0x0001, raw);
    }
}
```

## 5.5 CMake 构建系统

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(BuckHil VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# === Qt 依赖 ===
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
# 如需跨平台串口备选:
# find_package(Qt6 COMPONENTS SerialPort)

# === QCustomPlot (单文件集成) ===
add_library(qcustomplot STATIC
    3rdparty/qcustomplot/qcustomplot.cpp
    3rdparty/qcustomplot/qcustomplot.h
)
target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)
target_include_directories(qcustomplot PUBLIC 3rdparty/qcustomplot)

# === 主程序 ===
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

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Network
    qcustomplot
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/protocol
)

# === 安装 ===
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
```

## 5.6 Protocol.h — 协议常量

```cpp
// Protocol.h
#pragma once
#include <cstdint>

namespace Protocol {

// 命令码
constexpr uint8_t CMD_WRITE_PARAM      = 0x01;
constexpr uint8_t CMD_READ_PARAM       = 0x02;
constexpr uint8_t CMD_WRITE_PARAM_BATCH = 0x03;
constexpr uint8_t CMD_SET_TRIGGER      = 0x04;
constexpr uint8_t CMD_SIM_CTRL         = 0x05;
constexpr uint8_t CMD_GET_STATUS       = 0x06;
constexpr uint8_t CMD_SELF_TEST        = 0x07;

constexpr uint8_t CMD_PARAM_RESP       = 0x10;
constexpr uint8_t CMD_TRIG_DATA        = 0x11;
constexpr uint8_t CMD_STREAM_DATA      = 0x12;
constexpr uint8_t CMD_STATUS_RESP      = 0x13;
constexpr uint8_t CMD_SELF_TEST_RESP   = 0x14;
constexpr uint8_t CMD_ASYNC_EVENT      = 0xFE;
constexpr uint8_t CMD_ERROR            = 0xFF;

// 仿真控制
constexpr uint8_t SIM_STOP   = 0x00;
constexpr uint8_t SIM_START  = 0x01;
constexpr uint8_t SIM_RESET  = 0x02;
constexpr uint8_t SIM_TRIG   = 0x03;

// 参数 ID
constexpr uint16_t PARAM_L            = 0x0001;
constexpr uint16_t PARAM_C            = 0x0002;
constexpr uint16_t PARAM_R_LOAD       = 0x0003;
constexpr uint16_t PARAM_VIN          = 0x0004;
constexpr uint16_t PARAM_R_L          = 0x0005;
constexpr uint16_t PARAM_VF           = 0x0006;
constexpr uint16_t PARAM_F_SW         = 0x0007;
constexpr uint16_t PARAM_IL_MAX       = 0x0008;
constexpr uint16_t PARAM_FW_VERSION   = 0x0100;
constexpr uint16_t PARAM_DEVICE_ID    = 0x0101;

} // namespace Protocol
```

## 5.7 项目结构

```
host/
├── CMakeLists.txt
├── 3rdparty/
│   └── qcustomplot/
│       ├── qcustomplot.h
│       └── qcustomplot.cpp
├── src/
│   ├── main.cpp                    # 入口: QApplication + MainWindow
│   ├── MainWindow.h / .cpp         # 主窗口, 线程创建, 信号连接
│   ├── ParameterPanel.h / .cpp     # 参数配置面板
│   ├── CommunicationWorker.h / .cpp # 通信线程 (QTcpSocket + FrameParser)
│   ├── FrameParser.h / .cpp        # 帧解析状态机
│   ├── DataProcessor.h / .cpp      # 数据处理线程 (RingBuffer 管理)
│   └── RingBuffer.h                # 环形缓冲模板
└── protocol/
    └── Protocol.h                  # 命令常量定义
```

## 5.8 main.cpp

```cpp
#include <QApplication>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("BuckHIL");
    app.setApplicationVersion("1.0.0");

    MainWindow window;
    window.setWindowTitle("Buck HIL — ZU3EG");
    window.resize(1280, 800);
    window.show();

    return app.exec();
}
```

## 5.9 跨平台说明

| 平台 | 编译器 | Qt 获取 | 备注 |
|------|--------|---------|------|
| Windows 10/11 | MSVC 2022 / MinGW 11+ | Qt Online Installer / vcpkg | 推荐 MSVC，调试体验好 |
| Ubuntu 22.04+ | GCC 11+ | `apt install qt6-base-dev` | 包管理器直接装 |
| macOS 13+ | Clang 14+ | Homebrew `qt@6` | — |

**构建命令统一**：
```bash
cd host
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

## 5.10 与 C#/WPF 方案的关键差异

| 方面 | C#/WPF (旧方案) | Qt/C++ (新方案) |
|------|-----------------|-----------------|
| 跨平台 | 仅 Windows (.NET 8 可跨但 WPF 不可) | Windows/Linux/macOS 全支持 |
| 线程模型 | ConcurrentQueue + BlockingCollection | 信号槽 QueuedConnection，自动线程安全 |
| 图形 | OxyPlot (托管) | QCustomPlot (原生，更高刷新率) |
| 部署 | 需 .NET Runtime | 静态链接或自带 Qt dll (~15MB) |
| 内存控制 | GC 控制困难，大缓冲易触发 Gen2 | 完全手动，零意外 GC |
| 性能 | JIT 预热后与 C++ 差距小 | 原生编译，冷启动即最优 |
| 依赖 | .NET 8 SDK + NuGet | CMake + Qt 6 SDK |
