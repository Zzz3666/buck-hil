//=============================================================================
// MainWindow.h — 主窗口 (三线程模型)
//
// 线程 1 (GUI):  MainWindow + QCustomPlot + ParameterPanel (主事件循环)
// 线程 2 (Comm): CommWorker (QTcpSocket + FrameParser)
// 线程 3 (Proc): DataProcessor (RingBuffer 管理)
//
// 信号槽全部 QueuedConnection (跨线程安全)
//=============================================================================
#pragma once
#include <QMainWindow>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include "FrameParser.h"

// 前向声明
class QCustomPlot;
class CommWorker;
class DataProcessor;
class ParameterPanel;
class QThread;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    /// → CommWorker (跨线程)
    void sendFrame(Frame frame);

public slots:
    /// ← DataProcessor: 新数据到达 → 追加到绘图缓冲
    void updatePlot();

    /// ← DataProcessor: 触发完成
    void onTriggerReady(uint32_t seq);

    /// ← DataProcessor: 参数更新
    void onParamsUpdated();

    /// ← DataProcessor: 状态更新
    void onStatusUpdated(uint8_t state, uint8_t flags);

    /// ← CommWorker: 连接状态
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString &error);

    /// ← CommWorker: 错误事件
    void onDeviceError(uint8_t code, const QString &msg);

    /// ← CommWorker: 异步事件
    void onAsyncEvent(uint8_t eventType, uint8_t data);

private slots:
    // 用户操作
    void onParamChanged(uint16_t id, uint32_t value);
    void onReadAllParams();
    void onConnectClicked();
    void onDisconnectClicked();
    void onStartClicked();
    void onStopClicked();
    void onResetClicked();
    void onSelfTestClicked();

private:
    void setupUi();
    void setupThreads();
    void setupConnections();
    void closeEvent(QCloseEvent *event) override;

    // --- UI 组件 ---
    QCustomPlot   *m_plotVout;
    QCustomPlot   *m_plotIL;
    ParameterPanel *m_paramPanel;

    QLabel  *m_statusLabel;
    QLabel  *m_framesLabel;
    QLabel  *m_connLabel;

    QPushButton *m_btnConnect;
    QPushButton *m_btnStart;
    QPushButton *m_btnStop;
    QPushButton *m_btnReset;
    QPushButton *m_btnSelfTest;

    // --- 连接配置 ---
    QLineEdit *m_editHost;
    QLineEdit *m_editPort;

    // --- 线程对象 ---
    QThread       *m_commThread;
    QThread       *m_procThread;
    CommWorker    *m_commWorker;
    DataProcessor *m_dataProcessor;

    // --- 渲染定时器 ---
    QTimer *m_renderTimer;

    // --- 统计 ---
    uint32_t m_plotUpdates  = 0;
    uint32_t m_totalSamples = 0;
};
