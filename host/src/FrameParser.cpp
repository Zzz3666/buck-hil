//=============================================================================
// FrameParser.cpp — 逐字节帧解析状态机实现
//
// 协议帧: 0xAA | 0x55 | CMD(1B) | LEN(2B,BE) | PAYLOAD | CRC16(2B,BE) | 0x55
// CRC 覆盖: HEAD2 到 PAYLOAD 末尾
//=============================================================================
#include "FrameParser.h"
#include "protocol/Protocol.h"
#include <QtGlobal>

FrameParser::FrameParser(QObject *parent)
    : QObject(parent)
{
    reset();
}

void FrameParser::reset()
{
    m_state      = Sync1;
    m_cmd        = 0;
    m_len        = 0;
    m_payloadIdx = 0;
    m_crcCalc    = 0;
    m_crcRecv    = 0;
}

void FrameParser::feedBytes(const QByteArray &data)
{
    for (int i = 0; i < data.size(); i++)
        feedByte(static_cast<uint8_t>(data[i]));
}

void FrameParser::feedByte(uint8_t byte)
{
    switch (m_state) {

    case Sync1:
        if (byte == Protocol::HEAD1)
            m_state = Sync2;
        // else: stay in Sync1, silently skip
        break;

    case Sync2:
        if (byte == Protocol::HEAD2) {
            m_state   = Cmd;
            m_crcCalc = Protocol::crc16Init();
            m_crcCalc = Protocol::crc16Update(m_crcCalc, byte);
        } else {
            m_state = Sync1;  // bad head2 → resync
        }
        break;

    case Cmd:
        m_cmd      = byte;
        m_crcCalc  = Protocol::crc16Update(m_crcCalc, byte);
        m_state    = LenH;
        break;

    case LenH:
        m_len      = static_cast<uint16_t>(byte) << 8;
        m_crcCalc  = Protocol::crc16Update(m_crcCalc, byte);
        m_state    = LenL;
        break;

    case LenL:
        m_len     |= byte;
        m_crcCalc  = Protocol::crc16Update(m_crcCalc, byte);
        if (m_len > Protocol::MAX_PAYLOAD) {
            m_framesErr++;
            reset();
            break;
        }
        m_payloadIdx = 0;
        m_state      = (m_len == 0) ? CrcH : Payload;
        break;

    case Payload:
        m_payload[m_payloadIdx++] = byte;
        m_crcCalc = Protocol::crc16Update(m_crcCalc, byte);
        if (m_payloadIdx >= m_len)
            m_state = CrcH;
        break;

    case CrcH:
        m_crcRecv = static_cast<uint16_t>(byte) << 8;
        m_state   = CrcL;
        break;

    case CrcL:
        m_crcRecv |= byte;
        m_state   = Tail;
        break;

    case Tail:
        if (byte == Protocol::TAIL && m_crcCalc == m_crcRecv) {
            // 完整帧，CRC 正确
            QByteArray payload(reinterpret_cast<const char *>(m_payload),
                               static_cast<int>(m_len));
            emit frameReady(Frame{m_cmd, payload});
            m_framesOk++;
        } else {
            m_framesErr++;
        }
        reset();
        break;
    }
}

//=============================================================================
// Frame::serialize — 构建完整帧 (发送用)
//=============================================================================
QByteArray Frame::serialize() const
{
    QByteArray out;
    out.reserve(8 + payload.size());

    // 帧头
    out.append(static_cast<char>(Protocol::HEAD1));
    out.append(static_cast<char>(Protocol::HEAD2));

    // CMD
    out.append(static_cast<char>(cmd));

    // LEN (大端)
    uint16_t len = static_cast<uint16_t>(payload.size());
    out.append(static_cast<char>(len >> 8));
    out.append(static_cast<char>(len & 0xFF));

    // PAYLOAD (CRC 从 HEAD2 到 payload 末尾)
    uint16_t crc = Protocol::crc16Init();
    crc = Protocol::crc16Update(crc, Protocol::HEAD2);
    crc = Protocol::crc16Update(crc, cmd);
    crc = Protocol::crc16Update(crc, static_cast<uint8_t>(len >> 8));
    crc = Protocol::crc16Update(crc, static_cast<uint8_t>(len & 0xFF));
    for (int i = 0; i < payload.size(); i++) {
        uint8_t b = static_cast<uint8_t>(payload[i]);
        out.append(payload[i]);
        crc = Protocol::crc16Update(crc, b);
    }

    // CRC (大端)
    out.append(static_cast<char>(crc >> 8));
    out.append(static_cast<char>(crc & 0xFF));

    // TAIL
    out.append(static_cast<char>(Protocol::TAIL));

    return out;
}
