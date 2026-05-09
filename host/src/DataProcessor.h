//=============================================================================
// DataProcessor.h — 数据处理线程
//
// 职责:
//   - 管理 Vout/IL RingBuffer (各 10M 点)
//   - 解析流数据 → 电压/电流工程值
//   - 触发数据重组 (SEQ 排序)
//   - 发射 plotDataReady → MainWindow 33ms 定时刷新
//=============================================================================
#pragma once
#include <QObject>
#include <QFile>
#include <vector>
#include "RingBuffer.h"
#include "protocol/Protocol.h"

class DataProcessor : public QObject
{
    Q_OBJECT
public:
    explicit DataProcessor(QObject *parent = nullptr);

    // 环形缓冲 (MainWindow 只读访问)
    RingBuffer<double> &voutBuffer() { return m_ringVout; }
    RingBuffer<double> &ilBuffer()   { return m_ringIL; }
    RingBuffer<double> &dutyBuffer() { return m_ringDuty; }

    // 参数缓存
    struct CachedParams {
        uint32_t l_nh        = 100000;
        uint32_t c_pf        = 1000000;
        uint32_t r_load_mohm = 10000;
        uint32_t vin_mv      = 12000;
        uint32_t r_l_mohm    = 100;
        uint32_t vf_mv       = 700;
        uint32_t f_sw_hz     = 200000;
        uint32_t il_max_ma   = 10000;
    };
    const CachedParams &params() const { return m_params; }

    // 系统状态
    bool isRunning() const { return m_simRunning; }
    uint8_t statusFlags() const { return m_statusFlags; }

public slots:
    void onStreamData(QByteArray rawData);
    void onTriggerData(uint32_t seq, QByteArray rawData);
    void onParamResponse(uint16_t id, uint32_t value);
    void onStatusResponse(uint8_t state, uint8_t flags,
                          uint16_t pwmFreq, uint16_t voutMv,
                          uint16_t ilMa, uint16_t duty);

signals:
    void plotDataReady();               // → MainWindow::updatePlot()
    void triggerReady(uint32_t seq);     // → MainWindow::onTriggerReady()
    void paramsUpdated();               // → MainWindow 参数面板刷新
    void statusUpdated(uint8_t state, uint8_t flags);

private:
    RingBuffer<double> m_ringVout{5'000'000};
    RingBuffer<double> m_ringIL{5'000'000};
    RingBuffer<double> m_ringDuty{5'000'000};

    CachedParams m_params;
    bool  m_simRunning  = false;
    uint8_t m_statusFlags = 0;

    // 触发数据暂存
    struct TriggerBlock {
        uint32_t seq;
        QByteArray data;
    };
    std::vector<TriggerBlock> m_trigBlocks;

    // 解析数据块 → 工程值
    void parseStreamBlock(const QByteArray &raw,
                          std::vector<double> &vout,
                          std::vector<double> &il,
                          std::vector<double> &duty);
    void parseTriggerBlock(const QByteArray &raw,
                           std::vector<double> &vout,
                           std::vector<double> &il);
};
