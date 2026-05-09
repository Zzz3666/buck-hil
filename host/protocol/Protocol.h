//=============================================================================
// Protocol.h — Buck HIL 通信协议常量 (上位机端)
//
// 必须与 ps/inc/protocol.h 和 ps/src/protocol.c 保持同步
//=============================================================================
#pragma once
#include <cstdint>

namespace Protocol {

//=============================================================================
// 帧格式常量
//=============================================================================
constexpr uint8_t  HEAD1 = 0xAA;
constexpr uint8_t  HEAD2 = 0x55;
constexpr uint8_t  TAIL  = 0x55;
constexpr uint16_t MAX_PAYLOAD = 2048;

//=============================================================================
// 命令码
//=============================================================================
constexpr uint8_t CMD_WRITE_PARAM       = 0x01;
constexpr uint8_t CMD_READ_PARAM        = 0x02;
constexpr uint8_t CMD_WRITE_PARAM_BATCH = 0x03;
constexpr uint8_t CMD_SET_TRIGGER       = 0x04;
constexpr uint8_t CMD_SIM_CTRL          = 0x05;
constexpr uint8_t CMD_GET_STATUS        = 0x06;
constexpr uint8_t CMD_SELF_TEST         = 0x07;

constexpr uint8_t CMD_PARAM_RESP        = 0x10;
constexpr uint8_t CMD_TRIG_DATA         = 0x11;
constexpr uint8_t CMD_STREAM_DATA       = 0x12;
constexpr uint8_t CMD_STATUS_RESP       = 0x13;
constexpr uint8_t CMD_SELF_TEST_RESP    = 0x14;
constexpr uint8_t CMD_ASYNC_EVENT       = 0xFE;
constexpr uint8_t CMD_ERROR             = 0xFF;

//=============================================================================
// 仿真控制子命令
//=============================================================================
constexpr uint8_t SIM_STOP   = 0x00;
constexpr uint8_t SIM_START  = 0x01;
constexpr uint8_t SIM_RESET  = 0x02;
constexpr uint8_t SIM_TRIG   = 0x03;

//=============================================================================
// 错误码
//=============================================================================
constexpr uint8_t ERR_INVALID_CMD      = 0x01;
constexpr uint8_t ERR_INVALID_PARAM_ID = 0x02;
constexpr uint8_t ERR_PARAM_RANGE      = 0x03;
constexpr uint8_t ERR_CRC_FAIL         = 0x04;
constexpr uint8_t ERR_FRAME_LEN        = 0x05;
constexpr uint8_t ERR_SIM_NOT_READY    = 0x06;
constexpr uint8_t ERR_DMA_ERROR        = 0x07;
constexpr uint8_t ERR_BUFFER_OVERFLOW  = 0x08;

//=============================================================================
// 参数 ID
//=============================================================================
constexpr uint16_t PARAM_L             = 0x0001;
constexpr uint16_t PARAM_C             = 0x0002;
constexpr uint16_t PARAM_R_LOAD        = 0x0003;
constexpr uint16_t PARAM_VIN           = 0x0004;
constexpr uint16_t PARAM_R_L           = 0x0005;
constexpr uint16_t PARAM_VF            = 0x0006;
constexpr uint16_t PARAM_F_SW          = 0x0007;
constexpr uint16_t PARAM_IL_MAX        = 0x0008;
constexpr uint16_t PARAM_VOUT_SCALE    = 0x0009;
constexpr uint16_t PARAM_IL_SCALE      = 0x000A;
constexpr uint16_t PARAM_DAC_UPDATE_DIV = 0x000B;
constexpr uint16_t PARAM_FW_VERSION    = 0x0100;
constexpr uint16_t PARAM_DEVICE_ID     = 0x0101;

//=============================================================================
// 触发源
//=============================================================================
constexpr uint8_t TRIG_SRC_SOFTWARE   = 0;
constexpr uint8_t TRIG_SRC_VOUT       = 1;
constexpr uint8_t TRIG_SRC_IL         = 2;

constexpr uint8_t TRIG_EDGE_RISING    = 0;
constexpr uint8_t TRIG_EDGE_FALLING   = 1;
constexpr uint8_t TRIG_EDGE_LEVEL     = 2;

//=============================================================================
// 采样点格式 (每点 8 字节, 大端)
//=============================================================================
struct Sample {
    uint16_t vout;   // Vout, 原始量化值
    uint16_t il;     // IL, 原始量化值
    uint16_t duty;   // Duty cycle, 原始量化值
    uint16_t ts;     // Timestamp, 低16位

    static Sample fromRaw(const uint8_t *data) {
        return {
            static_cast<uint16_t>((data[0] << 8) | data[1]),
            static_cast<uint16_t>((data[2] << 8) | data[3]),
            static_cast<uint16_t>((data[4] << 8) | data[5]),
            static_cast<uint16_t>((data[6] << 8) | data[7]),
        };
    }
};
static_assert(sizeof(Sample) == 8, "Sample must be 8 bytes");

//=============================================================================
// CRC16-CCITT
//=============================================================================
constexpr uint16_t CRC16_POLY = 0x1021;
constexpr uint16_t CRC16_INIT = 0xFFFF;

inline uint16_t crc16Init() { return CRC16_INIT; }

inline uint16_t crc16Update(uint16_t crc, uint8_t byte)
{
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (crc << 1) ^ CRC16_POLY : crc << 1;
    return crc;
}

inline uint16_t crc16Block(const uint8_t *data, uint16_t len)
{
    uint16_t crc = crc16Init();
    for (uint16_t i = 0; i < len; i++)
        crc = crc16Update(crc, data[i]);
    return crc;
}

} // namespace Protocol
