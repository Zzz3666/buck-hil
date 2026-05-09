//=============================================================================
// CommunicationWorker.cpp — 通信线程实现
//=============================================================================
#include "CommunicationWorker.h"
#include "protocol/Protocol.h"
#include <QHostAddress>

CommWorker::CommWorker(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_parser(this)
    , m_reconnectTimer(new QTimer(this))
    , m_port(0)
{
    // socket → local slots (same thread, DirectConnection)
    connect(m_socket, &QTcpSocket::connected,
            this, &CommWorker::onConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &CommWorker::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &CommWorker::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, &CommWorker::onError);

    // parser → this → dispatch → emit
    connect(&m_parser, &FrameParser::frameReady,
            this, &CommWorker::dispatchFrame);

    // reconnect timer
    m_reconnectTimer->setInterval(RECONNECT_INTERVAL_MS);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &CommWorker::onReconnectTimer);
}

CommWorker::~CommWorker()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->disconnectFromHost();
}

//=============================================================================
// Slots
//=============================================================================
void CommWorker::connectToHost(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
    m_intentionalDisconnect = false;
    m_reconnectAttempts = 0;

    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    m_parser.reset();
    m_socket->connectToHost(QHostAddress(host), port);
}

void CommWorker::disconnectFromHost()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->disconnectFromHost();
}

void CommWorker::sendFrame(Frame frame)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray data = frame.serialize();
    m_socket->write(data);
}

//=============================================================================
// Private slots
//=============================================================================
void CommWorker::onConnected()
{
    m_reconnectAttempts = 0;
    emit connected();
}

void CommWorker::onDisconnected()
{
    m_parser.reset();
    emit disconnected();

    // 自动重连 (非主动断开 && 有目标地址)
    if (!m_intentionalDisconnect && !m_host.isEmpty() && m_port > 0) {
        m_reconnectTimer->start();
    }
}

void CommWorker::onReadyRead()
{
    // ▎铁律: socket read 在通信线程中，绝不阻塞 UI
    QByteArray data = m_socket->readAll();
    m_parser.feedBytes(data);
}

void CommWorker::onError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    if (!m_intentionalDisconnect) {
        emit connectionError(m_socket->errorString());
    }
}

void CommWorker::onReconnectTimer()
{
    if (m_intentionalDisconnect)
        return;

#if MAX_RECONNECT_ATTEMPTS > 0
    if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        emit connectionError(QStringLiteral("Max reconnect attempts reached"));
        return;
    }
#endif
    m_reconnectAttempts++;
    m_parser.reset();

    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    m_socket->connectToHost(QHostAddress(m_host), m_port);
}

//=============================================================================
// 帧分发
//=============================================================================
void CommWorker::dispatchFrame(const Frame &frame)
{
    const QByteArray &p = frame.payload;

    switch (frame.cmd) {

    // --- CMD_PARAM_RESP (0x10): ID(2B) + VALUE(4B) ---
    case Protocol::CMD_PARAM_RESP:
        if (p.size() >= 6) {
            uint16_t id = (static_cast<uint8_t>(p[0]) << 8)
                        | static_cast<uint8_t>(p[1]);
            uint32_t val = (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 24)
                         | (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 16)
                         | (static_cast<uint32_t>(static_cast<uint8_t>(p[4])) << 8)
                         | static_cast<uint8_t>(p[5]);
            emit paramResponse(id, val);
        }
        break;

    // --- CMD_TRIG_DATA (0x11): SEQ(4B) + COUNT(2B) + CH_MASK(1B) + samples ---
    case Protocol::CMD_TRIG_DATA:
        {
            uint32_t seq = 0;
            if (p.size() >= 4) {
                seq = (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
                    | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
                    | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8)
                    | static_cast<uint8_t>(p[3]);
            }
            emit triggerData(seq, p);
        }
        break;

    // --- CMD_STREAM_DATA (0x12): 连续流数据 ---
    case Protocol::CMD_STREAM_DATA:
        emit streamData(p);
        break;

    // --- CMD_STATUS_RESP (0x13): STATE(1B) + FLAGS(1B) + PWM_FREQ(2B) + ... ---
    case Protocol::CMD_STATUS_RESP:
        if (p.size() >= 10) {
            uint8_t  state    = static_cast<uint8_t>(p[0]);
            uint8_t  flags    = static_cast<uint8_t>(p[1]);
            uint16_t pwmFreq  = (static_cast<uint8_t>(p[2]) << 8)
                              | static_cast<uint8_t>(p[3]);
            uint16_t voutMv   = (static_cast<uint8_t>(p[4]) << 8)
                              | static_cast<uint8_t>(p[5]);
            uint16_t ilMa     = (static_cast<uint8_t>(p[6]) << 8)
                              | static_cast<uint8_t>(p[7]);
            uint16_t duty     = (static_cast<uint8_t>(p[8]) << 8)
                              | static_cast<uint8_t>(p[9]);
            emit statusResponse(state, flags, pwmFreq, voutMv, ilMa, duty);
        }
        break;

    // --- CMD_SELF_TEST_RESP (0x14): FLAGS(1B) + reserved(3B) ---
    case Protocol::CMD_SELF_TEST_RESP:
        if (p.size() >= 1) {
            emit selfTestResponse(static_cast<uint8_t>(p[0]));
        }
        break;

    // --- CMD_ASYNC_EVENT (0xFE): EVT_TYPE(1B) + DATA(1B) ---
    case Protocol::CMD_ASYNC_EVENT:
        if (p.size() >= 2) {
            emit asyncEvent(static_cast<uint8_t>(p[0]),
                            static_cast<uint8_t>(p[1]));
        }
        break;

    // --- CMD_ERROR (0xFF): CODE(1B) + MESSAGE(N bytes) ---
    case Protocol::CMD_ERROR:
        if (p.size() >= 2) {
            uint8_t code = static_cast<uint8_t>(p[0]);
            QString msg = QString::fromUtf8(p.mid(1));
            emit errorEvent(code, msg);
        }
        break;
    }
}
