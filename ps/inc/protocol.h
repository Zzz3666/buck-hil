//=============================================================================
// protocol.h — Buck HIL 通信协议定义与帧解析器接口
//
// 帧格式: 0xAA | 0x55 | CMD(1B) | LEN(2B,BE) | PAYLOAD | CRC16(2B,BE) | 0x55
// CRC16-CCITT, poly=0x1021, init=0xFFFF, 覆盖 HEAD2..PAYLOAD_END
//=============================================================================

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// 帧常量
//=============================================================================
#define PROTO_HEAD1        0xAAu
#define PROTO_HEAD2        0x55u
#define PROTO_TAIL         0x55u
#define MAX_PAYLOAD        2048
#define FRAME_TIMEOUT_MS   500       // 帧字节间最大间隔

//=============================================================================
// 命令码 (direction: PC→PS = host→device, PS→PC = device→host)
//=============================================================================
typedef enum {
    CMD_WRITE_PARAM       = 0x01,  // PC→PS: 写单个参数 (ID:2B + VAL:4B)
    CMD_READ_PARAM        = 0x02,  // PC→PS: 读单个参数 (ID:2B)
    CMD_WRITE_PARAM_BATCH = 0x03,  // PC→PS: 批量写参数 (COUNT:1B + {ID,VAL}*)
    CMD_SET_TRIGGER       = 0x04,  // PC→PS: 设置触发条件
    CMD_SIM_CTRL          = 0x05,  // PC→PS: 仿真控制 0=停 1=启 2=复位 3=软触发
    CMD_GET_STATUS        = 0x06,  // PC→PS: 获取系统状态
    CMD_SELF_TEST         = 0x07,  // PC→PS: 自检命令
    CMD_PARAM_RESP        = 0x10,  // PS→PC: 参数读应答 (ID:2B + VAL:4B)
    CMD_TRIG_DATA         = 0x11,  // PS→PC: 触发数据块
    CMD_STREAM_DATA       = 0x12,  // PS→PC: 连续流数据
    CMD_STATUS_RESP       = 0x13,  // PS→PC: 状态应答
    CMD_SELF_TEST_RESP    = 0x14,  // PS→PC: 自检结果
    CMD_ASYNC_EVENT       = 0xFE,  // PS→PC: 异步事件 (心跳/PWM丢失等)
    CMD_ERROR             = 0xFF,  // PS→PC: 错误响应
} protocol_cmd_t;

//=============================================================================
// 错误码
//=============================================================================
typedef enum {
    ERR_NONE             = 0x00,
    ERR_INVALID_CMD      = 0x01,
    ERR_INVALID_PARAM_ID = 0x02,
    ERR_PARAM_RANGE      = 0x03,
    ERR_CRC_FAIL         = 0x04,
    ERR_FRAME_LEN        = 0x05,
    ERR_SIM_NOT_READY    = 0x06,
    ERR_DMA_ERROR        = 0x07,
    ERR_BUFFER_OVERFLOW  = 0x08,
} protocol_err_t;

//=============================================================================
// 帧解析状态机状态
//=============================================================================
typedef enum {
    STATE_SYNC1,
    STATE_SYNC2,
    STATE_CMD,
    STATE_LEN_H,
    STATE_LEN_L,
    STATE_PAYLOAD,
    STATE_CRC_H,
    STATE_CRC_L,
    STATE_TAIL,
} frame_state_t;

//=============================================================================
// 帧解析器 (每连接一个实例)
//=============================================================================
typedef struct {
    frame_state_t state;
    uint8_t       cmd;
    uint16_t      len;
    uint16_t      payload_idx;
    uint8_t       payload[MAX_PAYLOAD];
    uint16_t      crc_local;
    uint16_t      crc_received;
    uint32_t      last_byte_tick;     // 超时检测用
    uint32_t      frames_ok;
    uint32_t      frames_err;
} frame_parser_t;

//=============================================================================
// 帧发送缓冲
//=============================================================================
typedef struct {
    uint8_t  buf[MAX_PAYLOAD + 8];    // 帧头 2 + PAYLOAD + CRC 2 + 帧尾 1 余量
    uint16_t len;                     // 完整帧长度
} frame_out_t;

//=============================================================================
// 命令处理上下文 (传递给 dispatch)
//=============================================================================
typedef struct {
    uint8_t  cmd;
    uint16_t payload_len;
    const uint8_t *payload;
} cmd_context_t;

//=============================================================================
// API: CRC16
//=============================================================================
uint16_t crc16_init(void);
uint16_t crc16_update(uint16_t crc, uint8_t byte);
uint16_t crc16_block(const uint8_t *data, uint16_t len);

//=============================================================================
// API: 帧解析 (逐字节)
// 返回: 0=继续接收, 1=帧完整且 CRC 正确, -1=帧错误/超时
//=============================================================================
void     frame_parser_init(frame_parser_t *fp);
int      frame_parse_byte(frame_parser_t *fp, uint8_t byte);
bool     frame_parser_timed_out(const frame_parser_t *fp, uint32_t now_tick,
                                uint32_t timeout_ms);

//=============================================================================
// API: 帧构建 (发送用)
//=============================================================================
void     frame_build(frame_out_t *f, uint8_t cmd,
                     const uint8_t *payload, uint16_t payload_len);

//=============================================================================
// API: 命令分发 (由主循环调用)
// 返回: 是否需要立即发送应答 (写帧到 out), 以及 out 是否已填充
//=============================================================================
bool     cmd_dispatch(const cmd_context_t *ctx, frame_out_t *out);

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_H
