//=============================================================================
// DataProcessor.cpp — 数据处理线程实现
//=============================================================================
#include "DataProcessor.h"
#include <QtGlobal>
#include <cmath>

DataProcessor::DataProcessor(QObject *parent)
    : QObject(parent)
{
}

//=============================================================================
// 流数据解析 (CMD_STREAM_DATA)
//
// 连续流帧格式: 同 TRIG_DATA 无 SEQ/COUNT header，
// 直接存储原始采样点 (8B each: Vout(2B) IL(2B) Duty(2B) TS(2B))
//=============================================================================
void DataProcessor::onStreamData(QByteArray rawData)
{
    if (rawData.isEmpty()) return;

    // 解析采样数据
    std::vector<double> vout, il, duty;
    parseStreamBlock(rawData, vout, il, duty);

    // 追加到环形缓冲
    if (!vout.empty()) m_ringVout.append(vout);
    if (!il.empty())   m_ringIL.append(il);
    if (!duty.empty()) m_ringDuty.append(duty);

    emit plotDataReady();
}

//=============================================================================
// 触发数据解析 (CMD_TRIG_DATA)
//
// 帧格式: SEQ(4B) + COUNT(2B) + CH_MASK(1B) + SAMPLE[COUNT] × 8B
//=============================================================================
void DataProcessor::onTriggerData(uint32_t seq, QByteArray rawData)
{
    // 检查最小长度: 4B SEQ + 2B COUNT + 1B MASK = 7B header
    if (rawData.size() < 7) return;

    // 提取 COUNT
    uint16_t count = (static_cast<uint8_t>(rawData[4]) << 8)
                   | static_cast<uint8_t>(rawData[5]);

    // 提取 CH_MASK
    uint8_t chMask = static_cast<uint8_t>(rawData[6]);

    // 验证数据长度
    int expectedSize = 7 + count * 8;
    if (rawData.size() < expectedSize) return;

    // 解析采样数据
    std::vector<double> vout, il;
    parseTriggerBlock(rawData, vout, il);

    // 存储触发块 (供后续分析/导出)
    m_trigBlocks.push_back({seq, rawData});

    emit triggerReady(seq);
    emit plotDataReady();

    (void)chMask; // 未来按通道掩码过滤
}

//=============================================================================
// 参数应答 (CMD_PARAM_RESP: ID(2B) + VALUE(4B))
//=============================================================================
void DataProcessor::onParamResponse(uint16_t id, uint32_t value)
{
    switch (id) {
    case Protocol::PARAM_L:       m_params.l_nh        = value; break;
    case Protocol::PARAM_C:       m_params.c_pf        = value; break;
    case Protocol::PARAM_R_LOAD:  m_params.r_load_mohm = value; break;
    case Protocol::PARAM_VIN:     m_params.vin_mv      = value; break;
    case Protocol::PARAM_R_L:     m_params.r_l_mohm    = value; break;
    case Protocol::PARAM_VF:      m_params.vf_mv       = value; break;
    case Protocol::PARAM_F_SW:    m_params.f_sw_hz     = value; break;
    case Protocol::PARAM_IL_MAX:  m_params.il_max_ma   = value; break;
    default: break;
    }
    emit paramsUpdated();
}

//=============================================================================
// 状态应答 (CMD_STATUS_RESP)
//=============================================================================
void DataProcessor::onStatusResponse(uint8_t state, uint8_t flags,
                                     uint16_t /*pwmFreq*/,
                                     uint16_t /*voutMv*/,
                                     uint16_t /*ilMa*/,
                                     uint16_t /*duty*/)
{
    m_simRunning  = (state == 1);  // 1 = running
    m_statusFlags = flags;
    emit statusUpdated(state, flags);
}

//=============================================================================
// 原始数据 → 工程值
//
// 流数据中每个采样: VOUT(2B,BE) + IL(2B,BE) + DUTY(2B,BE) + TS(2B,BE)
// 量纲转换:
//   Vout: raw → V = raw / 65535 * 12.0  (0~12V 量程)
//   IL:   raw → A = raw / 65535 * 20.0  (0~20A 量程)
//   Duty: raw → % = raw / 65535 * 100.0
//=============================================================================
void DataProcessor::parseStreamBlock(const QByteArray &raw,
                                     std::vector<double> &vout,
                                     std::vector<double> &il,
                                     std::vector<double> &duty)
{
    const int sampleSize = 8;
    int numSamples = raw.size() / sampleSize;

    vout.reserve(static_cast<size_t>(numSamples));
    il.reserve(static_cast<size_t>(numSamples));
    duty.reserve(static_cast<size_t>(numSamples));

    const uint8_t *data = reinterpret_cast<const uint8_t *>(raw.constData());

    for (int i = 0; i < numSamples; i++) {
        const uint8_t *s = data + i * sampleSize;

        uint16_t voutRaw = (static_cast<uint16_t>(s[0]) << 8) | s[1];
        uint16_t ilRaw   = (static_cast<uint16_t>(s[2]) << 8) | s[3];
        uint16_t dutyRaw = (static_cast<uint16_t>(s[4]) << 8) | s[5];

        // 量纲转换: 这些比例因子最终应从 PL 获取 (VOUT_SCALE, IL_SCALE)
        double voutV = static_cast<double>(voutRaw) / 65535.0 * 12.0;
        double ilA   = static_cast<double>(ilRaw)   / 65535.0 * 20.0;
        double dutyPct = static_cast<double>(dutyRaw) / 65535.0 * 100.0;

        vout.push_back(voutV);
        il.push_back(ilA);
        duty.push_back(dutyPct);
    }
}

void DataProcessor::parseTriggerBlock(const QByteArray &raw,
                                      std::vector<double> &vout,
                                      std::vector<double> &il)
{
    // 跳过 7 字节 header (SEQ+COUNT+MASK)
    const int headerSize = 7;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(raw.constData()) + headerSize;

    uint16_t count = (static_cast<uint8_t>(raw[4]) << 8)
                   | static_cast<uint8_t>(raw[5]);

    vout.reserve(count);
    il.reserve(count);

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *s = data + i * 8;

        uint16_t voutRaw = (static_cast<uint16_t>(s[0]) << 8) | s[1];
        uint16_t ilRaw   = (static_cast<uint16_t>(s[2]) << 8) | s[3];

        vout.push_back(static_cast<double>(voutRaw) / 65535.0 * 12.0);
        il.push_back(static_cast<double>(ilRaw)   / 65535.0 * 20.0);
    }
}
