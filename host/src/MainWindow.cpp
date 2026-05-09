//=============================================================================
// MainWindow.cpp — 主窗口实现
//
// 线程模型: 3 线程 + QueuedConnection 信号槽
// UI 线程零阻塞 —— 绝不做 socket I/O / 文件 I/O / sleep
//=============================================================================
#include "MainWindow.h"
#include "CommunicationWorker.h"
#include "DataProcessor.h"
#include "ParameterPanel.h"
#include "RingBuffer.h"
#include "protocol/Protocol.h"
#include "qcustomplot.h"

#include <QThread>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QCloseEvent>
#include <QMessageBox>

//=============================================================================
// 构造 / 析构
//=============================================================================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupThreads();
    setupConnections();
}

MainWindow::~MainWindow()
{
    // 停止定时器
    m_renderTimer->stop();

    // 安全退出线程
    m_commThread->quit();
    m_procThread->quit();
    m_commThread->wait(3000);
    m_procThread->wait(3000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_renderTimer->stop();
    event->accept();
}

//=============================================================================
// UI 构建
//=============================================================================
void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("Buck HIL — ZU3EG 仿真平台"));
    resize(1400, 900);

    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(6, 6, 6, 6);

    // === 工具栏: 连接 + 仿真控制 ===
    auto *toolbar = new QHBoxLayout;

    // 连接设置
    toolbar->addWidget(new QLabel(QStringLiteral("Host:"), this));
    m_editHost = new QLineEdit("192.168.1.100", this);
    m_editHost->setMaximumWidth(140);
    toolbar->addWidget(m_editHost);

    toolbar->addWidget(new QLabel(QStringLiteral("Port:"), this));
    m_editPort = new QLineEdit("5000", this);
    m_editPort->setMaximumWidth(60);
    toolbar->addWidget(m_editPort);

    m_btnConnect = new QPushButton(QStringLiteral("连接"), this);
    toolbar->addWidget(m_btnConnect);

    toolbar->addSpacing(20);

    // 仿真控制
    m_btnStart = new QPushButton(QStringLiteral("▶ 启动仿真"), this);
    m_btnStart->setEnabled(false);
    toolbar->addWidget(m_btnStart);

    m_btnStop = new QPushButton(QStringLiteral("■ 停止"), this);
    m_btnStop->setEnabled(false);
    toolbar->addWidget(m_btnStop);

    m_btnReset = new QPushButton(QStringLiteral("↺ 复位"), this);
    m_btnReset->setEnabled(false);
    toolbar->addWidget(m_btnReset);

    toolbar->addSpacing(20);

    m_btnSelfTest = new QPushButton(QStringLiteral("自检"), this);
    m_btnSelfTest->setEnabled(false);
    toolbar->addWidget(m_btnSelfTest);

    toolbar->addStretch();
    mainLayout->addLayout(toolbar);

    // === 主体: 波形 + 参数面板 ===
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // -- 左侧: 波形显示 --
    auto *plotContainer = new QWidget(this);
    auto *plotLayout = new QVBoxLayout(plotContainer);
    plotLayout->setContentsMargins(0, 0, 0, 0);

    // Vout 波形
    m_plotVout = new QCustomPlot(this);
    m_plotVout->addGraph();
    m_plotVout->graph(0)->setPen(QPen(QColor("#00BCD4"), 1.5));
    m_plotVout->graph(0)->setName(QStringLiteral("Vout"));
    m_plotVout->xAxis->setLabel(QStringLiteral("采样点"));
    m_plotVout->yAxis->setLabel(QStringLiteral("Vout (V)"));
    m_plotVout->yAxis->setRange(-0.5, 15.0);
    m_plotVout->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plotVout->axisRect()->setRangeZoom(Qt::Horizontal);
    m_plotVout->setNotAntialiasedElements(QCP::aeAll);
    m_plotVout->legend->setVisible(true);
    plotLayout->addWidget(m_plotVout, 3);

    // IL 波形
    m_plotIL = new QCustomPlot(this);
    m_plotIL->addGraph();
    m_plotIL->graph(0)->setPen(QPen(QColor("#FF5722"), 1.5));
    m_plotIL->graph(0)->setName(QStringLiteral("IL"));
    m_plotIL->xAxis->setLabel(QStringLiteral("采样点"));
    m_plotIL->yAxis->setLabel(QStringLiteral("IL (A)"));
    m_plotIL->yAxis->setRange(-0.5, 12.0);
    m_plotIL->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plotIL->axisRect()->setRangeZoom(Qt::Horizontal);
    m_plotIL->setNotAntialiasedElements(QCP::aeAll);
    m_plotIL->legend->setVisible(true);
    plotLayout->addWidget(m_plotIL, 2);

    splitter->addWidget(plotContainer);

    // -- 右侧: 参数面板 --
    m_paramPanel = new ParameterPanel(this);
    m_paramPanel->setMaximumWidth(280);
    splitter->addWidget(m_paramPanel);

    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter);

    setCentralWidget(centralWidget);

    // === 状态栏 ===
    m_connLabel   = new QLabel(QStringLiteral("未连接"), this);
    m_statusLabel = new QLabel(QStringLiteral("空闲"), this);
    m_framesLabel = new QLabel(QStringLiteral("帧: 0"), this);

    statusBar()->addWidget(m_connLabel, 1);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addWidget(m_framesLabel, 1);

    // === 信号连接 (UI → slots) ===
    connect(m_btnConnect, &QPushButton::clicked,
            this, &MainWindow::onConnectClicked);
    connect(m_btnStart,   &QPushButton::clicked,
            this, &MainWindow::onStartClicked);
    connect(m_btnStop,    &QPushButton::clicked,
            this, &MainWindow::onStopClicked);
    connect(m_btnReset,   &QPushButton::clicked,
            this, &MainWindow::onResetClicked);
    connect(m_btnSelfTest,&QPushButton::clicked,
            this, &MainWindow::onSelfTestClicked);
    connect(m_paramPanel, &ParameterPanel::paramChanged,
            this, &MainWindow::onParamChanged);
    connect(m_paramPanel, &ParameterPanel::readAllClicked,
            this, &MainWindow::onReadAllParams);
}

//=============================================================================
// 线程创建 (QThread::moveToThread 模式)
//=============================================================================
void MainWindow::setupThreads()
{
    // --- 通信线程 ---
    m_commThread = new QThread(this);
    m_commWorker = new CommWorker();  // 无 parent
    m_commWorker->moveToThread(m_commThread);

    connect(m_commThread, &QThread::finished,
            m_commWorker, &QObject::deleteLater);

    // --- 数据处理线程 ---
    m_procThread = new QThread(this);
    m_dataProcessor = new DataProcessor();  // 无 parent
    m_dataProcessor->moveToThread(m_procThread);

    connect(m_procThread, &QThread::finished,
            m_dataProcessor, &QObject::deleteLater);

    // 启动线程
    m_commThread->start();
    m_procThread->start();

    // --- 渲染定时器 (GUI 线程, 33ms ≈ 30 FPS) ---
    m_renderTimer = new QTimer(this);
    m_renderTimer->setInterval(33);
    connect(m_renderTimer, &QTimer::timeout,
            this, &MainWindow::updatePlot);
    m_renderTimer->start();
}

//=============================================================================
// 跨线程信号连接 (全部 QueuedConnection — 自动线程安全)
//=============================================================================
void MainWindow::setupConnections()
{
    // === CommWorker → DataProcessor (跨线程) ===
    connect(m_commWorker, &CommWorker::streamData,
            m_dataProcessor, &DataProcessor::onStreamData);
    connect(m_commWorker, &CommWorker::triggerData,
            m_dataProcessor, &DataProcessor::onTriggerData);
    connect(m_commWorker, &CommWorker::paramResponse,
            m_dataProcessor, &DataProcessor::onParamResponse);
    connect(m_commWorker, &CommWorker::statusResponse,
            m_dataProcessor, &DataProcessor::onStatusResponse);

    // === DataProcessor → MainWindow (跨线程) ===
    connect(m_dataProcessor, &DataProcessor::plotDataReady,
            this, &MainWindow::updatePlot);
    connect(m_dataProcessor, &DataProcessor::triggerReady,
            this, &MainWindow::onTriggerReady);
    connect(m_dataProcessor, &DataProcessor::paramsUpdated,
            this, &MainWindow::onParamsUpdated);
    connect(m_dataProcessor, &DataProcessor::statusUpdated,
            this, &MainWindow::onStatusUpdated);

    // === CommWorker → MainWindow (跨线程) ===
    connect(m_commWorker, &CommWorker::connected,
            this, &MainWindow::onConnected);
    connect(m_commWorker, &CommWorker::disconnected,
            this, &MainWindow::onDisconnected);
    connect(m_commWorker, &CommWorker::connectionError,
            this, &MainWindow::onConnectionError);
    connect(m_commWorker, &CommWorker::errorEvent,
            this, &MainWindow::onDeviceError);
    connect(m_commWorker, &CommWorker::asyncEvent,
            this, &MainWindow::onAsyncEvent);

    // === MainWindow → CommWorker: sendFrame (跨线程) ===
    // 使用 sendCommand 信号 → CommWorker::sendFrame slot
    connect(this, &MainWindow::sendFrame,
            m_commWorker, &CommWorker::sendFrame);
}

//=============================================================================
// 用户操作 → 协议命令
//=============================================================================
void MainWindow::onParamChanged(uint16_t id, uint32_t value)
{
    // 构建 CMD_WRITE_PARAM 帧
    QByteArray payload;
    payload.append(static_cast<char>(id >> 8));
    payload.append(static_cast<char>(id & 0xFF));
    payload.append(static_cast<char>(value >> 24));
    payload.append(static_cast<char>(value >> 16));
    payload.append(static_cast<char>(value >> 8));
    payload.append(static_cast<char>(value & 0xFF));

    Frame frame{Protocol::CMD_WRITE_PARAM, payload};
    emit sendFrame(frame);
}

void MainWindow::onReadAllParams()
{
    // 发送 CMD_READ_PARAM 读各参数 (非阻塞批量)
    const uint16_t ids[] = {
        Protocol::PARAM_L, Protocol::PARAM_C, Protocol::PARAM_R_LOAD,
        Protocol::PARAM_VIN, Protocol::PARAM_R_L, Protocol::PARAM_VF,
        Protocol::PARAM_F_SW, Protocol::PARAM_IL_MAX
    };

    for (uint16_t id : ids) {
        QByteArray payload;
        payload.append(static_cast<char>(id >> 8));
        payload.append(static_cast<char>(id & 0xFF));
        Frame frame{Protocol::CMD_READ_PARAM, payload};
        emit sendFrame(frame);
    }
}

void MainWindow::onConnectClicked()
{
    QString host = m_editHost->text();
    quint16 port = static_cast<quint16>(m_editPort->text().toUInt());

    // 通过信号槽方式调用 CommWorker::connectToHost
    // 由于 connectToHost 是 slot 且跨线程，必须用 QMetaObject::invokeMethod
    QMetaObject::invokeMethod(m_commWorker, "connectToHost",
                              Qt::QueuedConnection,
                              Q_ARG(QString, host),
                              Q_ARG(quint16, port));
}

void MainWindow::onDisconnectClicked()
{
    QMetaObject::invokeMethod(m_commWorker, "disconnectFromHost",
                              Qt::QueuedConnection);
}

void MainWindow::onStartClicked()
{
    QByteArray payload;
    payload.append(static_cast<char>(Protocol::SIM_START));
    Frame frame{Protocol::CMD_SIM_CTRL, payload};
    emit sendFrame(frame);
}

void MainWindow::onStopClicked()
{
    QByteArray payload;
    payload.append(static_cast<char>(Protocol::SIM_STOP));
    Frame frame{Protocol::CMD_SIM_CTRL, payload};
    emit sendFrame(frame);
}

void MainWindow::onResetClicked()
{
    QByteArray payload;
    payload.append(static_cast<char>(Protocol::SIM_RESET));
    Frame frame{Protocol::CMD_SIM_CTRL, payload};
    emit sendFrame(frame);
}

void MainWindow::onSelfTestClicked()
{
    Frame frame{Protocol::CMD_SELF_TEST, QByteArray()};
    emit sendFrame(frame);
}

//=============================================================================
// 波形更新 (GUI 线程, 33ms 周期)
//
// ▎铁律: 只读共享缓冲区, 不分配大内存, 不做 I/O
//=============================================================================
void MainWindow::updatePlot()
{
    // 从 DataProcessor 环形缓冲获取最近数据
    auto voutData = m_dataProcessor->voutBuffer().getRecent(10000);
    auto ilData   = m_dataProcessor->ilBuffer().getRecent(10000);

    m_plotUpdates++;
    if (!voutData.empty())
        m_totalSamples += voutData.size();

    // 更新 Vout 曲线
    if (!voutData.empty()) {
        // 使用预分配 addData 以降低开销
        QVector<double> x(voutData.size());
        for (size_t i = 0; i < voutData.size(); i++)
            x[i] = static_cast<double>(i);

        m_plotVout->graph(0)->setData(
            QVector<double>(x.begin(), x.end()),
            QVector<double>(voutData.begin(), voutData.end()), true);
        m_plotVout->replot(QCustomPlot::rpQueuedReplot);
    }

    // 更新 IL 曲线
    if (!ilData.empty()) {
        QVector<double> x(ilData.size());
        for (size_t i = 0; i < ilData.size(); i++)
            x[i] = static_cast<double>(i);

        m_plotIL->graph(0)->setData(
            QVector<double>(x.begin(), x.end()),
            QVector<double>(ilData.begin(), ilData.end()), true);
        m_plotIL->replot(QCustomPlot::rpQueuedReplot);
    }

    // 更新统计信息
    m_framesLabel->setText(
        QStringLiteral("刷新: %1 | 采样: %2")
            .arg(m_plotUpdates).arg(m_totalSamples));
}

//=============================================================================
// 状态回调
//=============================================================================
void MainWindow::onConnected()
{
    m_connLabel->setText(QStringLiteral("<font color='green'>● 已连接</font>"));
    m_btnConnect->setEnabled(false);
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(true);
    m_btnReset->setEnabled(true);
    m_btnSelfTest->setEnabled(true);

    // 连接后自动读取状态
    Frame frame{Protocol::CMD_GET_STATUS, QByteArray()};
    emit sendFrame(frame);
}

void MainWindow::onDisconnected()
{
    m_connLabel->setText(QStringLiteral("<font color='red'>● 已断开</font>"));
    m_btnConnect->setEnabled(true);
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(false);
    m_btnReset->setEnabled(false);
    m_btnSelfTest->setEnabled(false);
}

void MainWindow::onConnectionError(const QString &error)
{
    m_connLabel->setText(QStringLiteral("<font color='orange'>⚠ %1</font>").arg(error));
}

void MainWindow::onParamsUpdated()
{
    // 将 DataProcessor 缓存的参数同步到 ParameterPanel UI
    const auto &p = m_dataProcessor->params();
    m_paramPanel->updateFromDevice(Protocol::PARAM_L,       p.l_nh);
    m_paramPanel->updateFromDevice(Protocol::PARAM_C,       p.c_pf);
    m_paramPanel->updateFromDevice(Protocol::PARAM_R_LOAD,  p.r_load_mohm);
    m_paramPanel->updateFromDevice(Protocol::PARAM_VIN,     p.vin_mv);
    m_paramPanel->updateFromDevice(Protocol::PARAM_R_L,     p.r_l_mohm);
    m_paramPanel->updateFromDevice(Protocol::PARAM_VF,      p.vf_mv);
    m_paramPanel->updateFromDevice(Protocol::PARAM_F_SW,    p.f_sw_hz);
    m_paramPanel->updateFromDevice(Protocol::PARAM_IL_MAX,  p.il_max_ma);
}

void MainWindow::onStatusUpdated(uint8_t state, uint8_t flags)
{
    const char *stateText[] = {"空闲", "运行中", "错误", "触发中"};
    m_statusLabel->setText(
        QStringLiteral("%1 [flags: 0x%2]")
            .arg((state < 4) ? stateText[state] : "未知")
            .arg(flags, 2, 16, QChar('0')));
}

void MainWindow::onTriggerReady(uint32_t seq)
{
    statusBar()->showMessage(
        QStringLiteral("触发 #%1 完成").arg(seq), 5000);
}

void MainWindow::onDeviceError(uint8_t code, const QString &msg)
{
    statusBar()->showMessage(
        QStringLiteral("设备错误 0x%1: %2")
            .arg(code, 2, 16, QChar('0')).arg(msg), 8000);
}

void MainWindow::onAsyncEvent(uint8_t eventType, uint8_t data)
{
    switch (eventType) {
    case 0x0D:  // EVT_HEARTBEAT
        // 心跳静默处理 (不刷屏)
        break;
    case 0x08:  // EVT_PWM_LOST
        m_statusLabel->setText(QStringLiteral("<font color='red'>PWM 丢失!</font>"));
        break;
    case 0x09:  // EVT_PWM_RECOVERED
        m_statusLabel->setText(QStringLiteral("运行中"));
        break;
    default:
        statusBar()->showMessage(
            QStringLiteral("异步事件: 0x%1 data=0x%2")
                .arg(eventType, 2, 16).arg(data, 2, 16), 3000);
        break;
    }
}
