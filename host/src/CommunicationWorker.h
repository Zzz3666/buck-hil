//=============================================================================
// CommunicationWorker.h — 通信线程 (QThread::moveToThread 模式)
//
// 职责:
//   - QTcpSocket 异步连接/重连
//   - 发送帧 (由 MainWindow 跨线程信号驱动)
//   - 接收数据 → FrameParser → 按 CMD 分发信号
//
// 线程安全: 所有 socket 操作均在 CommWorker 所在线程执行
//=============================================================================
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
    ~CommWorker() override;

    bool isConnected() const { return m_socket->state() == QAbstractSocket::ConnectedState; }

public slots:
    /// 连接到 PS 端
    void connectToHost(const QString &host, quint16 port);
    void disconnectFromHost();

    /// 发送帧 (由 MainWindow 跨线程调用, QueuedConnection)
    void sendFrame(Frame frame);

signals:
    // --- 连接状态 (→ MainWindow) ---
    void connected();
    void disconnected();
    void connectionError(const QString &error);

    // --- 解析后的数据 (→ DataProcessor, QueuedConnection) ---
    void paramResponse(uint16_t id, uint32_t value);
    void statusResponse(uint8_t state, uint8_t flags,
                        uint16_t pwmFreq, uint16_t voutMv,
                        uint16_t ilMa, uint16_t duty);
    void triggerData(uint32_t seq, QByteArray rawData);
    void streamData(QByteArray rawData);
    void selfTestResponse(uint8_t flags);
    void errorEvent(uint8_t code, QString message);
    void asyncEvent(uint8_t eventType, uint8_t data);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError socketError);
    void onReconnectTimer();

private:
    void dispatchFrame(const Frame &frame);

    QTcpSocket *m_socket;
    FrameParser m_parser;
    QTimer     *m_reconnectTimer;

    QString  m_host;
    quint16  m_port;
    bool     m_intentionalDisconnect = false;

    // 重连参数
    static constexpr int RECONNECT_INTERVAL_MS = 2000;
    static constexpr int MAX_RECONNECT_ATTEMPTS = 0;  // 0 = 无限
    int m_reconnectAttempts = 0;
};
