//=============================================================================
// FrameParser.h — 逐字节帧解析状态机
//
// 与 PS 端 protocol.c 的 frame_parse_byte 逻辑完全一致。
// 用 feedByte 逐字节喂入，完整帧通过 frameReady 信号发射。
//=============================================================================
#pragma once
#include <QObject>
#include <QByteArray>
#include <cstdint>

struct Frame {
    uint8_t    cmd = 0xFF;
    QByteArray payload;

    QByteArray serialize() const;  // 计算 CRC 并组装完整帧
};

class FrameParser : public QObject
{
    Q_OBJECT
public:
    explicit FrameParser(QObject *parent = nullptr);

    /// 逐字节喂入
    void feedByte(uint8_t byte);

    /// 批量喂入 (内部逐字节调用)
    void feedBytes(const QByteArray &data);

    /// 重置状态机
    void reset();

    /// 帧统计
    uint32_t framesOk()  const { return m_framesOk; }
    uint32_t framesErr() const { return m_framesErr; }

signals:
    void frameReady(Frame frame);

private:
    enum State {
        Sync1, Sync2, Cmd, LenH, LenL, Payload, CrcH, CrcL, Tail
    };
    State m_state = Sync1;

    uint8_t  m_cmd = 0;
    uint16_t m_len = 0;
    uint16_t m_payloadIdx = 0;
    uint8_t  m_payload[Protocol::MAX_PAYLOAD];
    uint16_t m_crcCalc = 0;
    uint16_t m_crcRecv = 0;

    uint32_t m_framesOk  = 0;
    uint32_t m_framesErr = 0;

    QByteArray m_overflow;  // 半包暂存 (跨 socket read 保留状态)
};
