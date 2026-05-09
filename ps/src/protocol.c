//=============================================================================
// protocol.c — 帧解析状态机 + CRC16 + 命令分包与响应构建
//=============================================================================

#include "protocol.h"
#include "params.h"
#include "logger.h"
#include "platform.h"
#include <string.h>   // memcpy

//=============================================================================
// CRC16-CCITT: poly=0x1021, init=0xFFFF
//=============================================================================
uint16_t crc16_init(void)
{
    return 0xFFFFu;
}

uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000u)
            crc = (crc << 1) ^ 0x1021u;
        else
            crc = crc << 1;
    }
    return crc;
}

uint16_t crc16_block(const uint8_t *data, uint16_t len)
{
    uint16_t crc = crc16_init();
    for (uint16_t i = 0; i < len; i++)
        crc = crc16_update(crc, data[i]);
    return crc;
}

//=============================================================================
// 帧解析器
//=============================================================================
void frame_parser_init(frame_parser_t *fp)
{
    fp->state          = STATE_SYNC1;
    fp->cmd            = 0;
    fp->len            = 0;
    fp->payload_idx    = 0;
    fp->crc_local      = 0;
    fp->crc_received   = 0;
    fp->last_byte_tick = 0;
    fp->frames_ok      = 0;
    fp->frames_err     = 0;
}

int frame_parse_byte(frame_parser_t *fp, uint8_t byte)
{
    fp->last_byte_tick = platform_tick_ms();

    switch (fp->state) {

    case STATE_SYNC1:
        if (byte == PROTO_HEAD1)
            fp->state = STATE_SYNC2;
        // else: stay in SYNC1, silently skip garbage
        break;

    case STATE_SYNC2:
        if (byte == PROTO_HEAD2) {
            fp->state     = STATE_CMD;
            fp->crc_local = crc16_init();
            fp->crc_local = crc16_update(fp->crc_local, byte);
        } else {
            fp->state = STATE_SYNC1;  // invalid head2 → restart
        }
        break;

    case STATE_CMD:
        fp->cmd        = byte;
        fp->crc_local  = crc16_update(fp->crc_local, byte);
        fp->state      = STATE_LEN_H;
        break;

    case STATE_LEN_H:
        fp->len        = (uint16_t)byte << 8;
        fp->crc_local  = crc16_update(fp->crc_local, byte);
        fp->state      = STATE_LEN_L;
        break;

    case STATE_LEN_L:
        fp->len       |= byte;
        fp->crc_local  = crc16_update(fp->crc_local, byte);
        if (fp->len > MAX_PAYLOAD) {
            fp->state = STATE_SYNC1;
            fp->frames_err++;
            return -1;
        }
        fp->payload_idx = 0;
        fp->state       = (fp->len == 0) ? STATE_CRC_H : STATE_PAYLOAD;
        break;

    case STATE_PAYLOAD:
        fp->payload[fp->payload_idx++] = byte;
        fp->crc_local = crc16_update(fp->crc_local, byte);
        if (fp->payload_idx >= fp->len)
            fp->state = STATE_CRC_H;
        break;

    case STATE_CRC_H:
        fp->crc_received = (uint16_t)byte << 8;
        fp->state        = STATE_CRC_L;
        break;

    case STATE_CRC_L:
        fp->crc_received |= byte;
        fp->state        = STATE_TAIL;
        break;

    case STATE_TAIL:
        fp->state = STATE_SYNC1;
        if (byte == PROTO_TAIL && fp->crc_local == fp->crc_received) {
            fp->frames_ok++;
            return 1;  // 完整帧, CRC 正确
        }
        fp->frames_err++;
        return -1;     // 帧尾或 CRC 错误
    }

    return 0;  // 继续接收
}

bool frame_parser_timed_out(const frame_parser_t *fp, uint32_t now_tick,
                            uint32_t timeout_ms)
{
    if (fp->state == STATE_SYNC1)
        return false;  // SYNC1 状态下不超时 (等待数据)
    return (now_tick - fp->last_byte_tick) > timeout_ms;
}

//=============================================================================
// 帧构建 (发送用)
//=============================================================================
void frame_build(frame_out_t *f, uint8_t cmd,
                 const uint8_t *payload, uint16_t payload_len)
{
    // 帧头
    f->buf[0] = PROTO_HEAD1;
    f->buf[1] = PROTO_HEAD2;

    // CMD
    f->buf[2] = cmd;

    // LEN (大端)
    f->buf[3] = (uint8_t)(payload_len >> 8);
    f->buf[4] = (uint8_t)(payload_len & 0xFF);

    // PAYLOAD
    if (payload_len > 0 && payload != NULL)
        memcpy(&f->buf[5], payload, payload_len);

    // CRC (大端), 覆盖 HEAD2..PAYLOAD_END
    uint16_t crc = crc16_init();
    crc = crc16_update(crc, f->buf[1]);  // HEAD2
    crc = crc16_update(crc, f->buf[2]);  // CMD
    crc = crc16_update(crc, f->buf[3]);  // LEN_H
    crc = crc16_update(crc, f->buf[4]);  // LEN_L
    for (uint16_t i = 0; i < payload_len; i++)
        crc = crc16_update(crc, payload[i]);

    uint16_t crc_pos = 5 + payload_len;
    f->buf[crc_pos]     = (uint8_t)(crc >> 8);
    f->buf[crc_pos + 1] = (uint8_t)(crc & 0xFF);

    // TAIL
    f->buf[crc_pos + 2] = PROTO_TAIL;

    f->len = crc_pos + 3;  // HEAD(2) + CMD(1) + LEN(2) + PAYLOAD + CRC(2) + TAIL(1)
}

//=============================================================================
// 辅助: 构建错误响应帧
//=============================================================================
static void build_error(frame_out_t *out, uint8_t code, const char *msg)
{
    uint8_t payload[64];
    uint8_t msg_len = 0;
    if (msg != NULL) {
        msg_len = (uint8_t)strlen(msg);
        if (msg_len > 60) msg_len = 60;
    }
    payload[0] = code;
    if (msg_len > 0)
        memcpy(&payload[1], msg, msg_len);
    frame_build(out, CMD_ERROR, payload, 1 + msg_len);
}

//=============================================================================
// 辅助: 构建参数应答帧 (CMD_PARAM_RESP)
//=============================================================================
static void build_param_resp(frame_out_t *out, uint16_t id, uint32_t value)
{
    uint8_t payload[6];
    payload[0] = (uint8_t)(id >> 8);
    payload[1] = (uint8_t)(id & 0xFF);
    payload[2] = (uint8_t)(value >> 24);
    payload[3] = (uint8_t)(value >> 16);
    payload[4] = (uint8_t)(value >> 8);
    payload[5] = (uint8_t)(value & 0xFF);
    frame_build(out, CMD_PARAM_RESP, payload, 6);
}

//=============================================================================
// 辅助: 构建状态应答帧 (CMD_STATUS_RESP)
//=============================================================================
static void build_status_resp(frame_out_t *out,
                              uint8_t state, uint8_t flags,
                              uint16_t pwm_freq, uint16_t vout_mv,
                              uint16_t il_ma, uint16_t duty)
{
    uint8_t payload[10];
    payload[0] = state;
    payload[1] = flags;
    payload[2] = (uint8_t)(pwm_freq >> 8);
    payload[3] = (uint8_t)(pwm_freq & 0xFF);
    payload[4] = (uint8_t)(vout_mv >> 8);
    payload[5] = (uint8_t)(vout_mv & 0xFF);
    payload[6] = (uint8_t)(il_ma >> 8);
    payload[7] = (uint8_t)(il_ma & 0xFF);
    payload[8] = (uint8_t)(duty >> 8);
    payload[9] = (uint8_t)(duty & 0xFF);
    frame_build(out, CMD_STATUS_RESP, payload, 10);
}

//=============================================================================
// 命令分发 (主循环调用)
// 返回 true = out 已填充应答帧，需要发送
//=============================================================================
bool cmd_dispatch(const cmd_context_t *ctx, frame_out_t *out)
{
    switch (ctx->cmd) {

    //-----------------------------------------------------------------
    // CMD_WRITE_PARAM (0x01): ID(2B) + VALUE(4B), LEN=6
    //-----------------------------------------------------------------
    case CMD_WRITE_PARAM:
        if (ctx->payload_len != 6) {
            build_error(out, ERR_FRAME_LEN, "WRITE_PARAM expects 6B payload");
            return true;
        }
        {
            uint16_t id = ((uint16_t)ctx->payload[0] << 8)
                        |  ctx->payload[1];
            uint32_t val = ((uint32_t)ctx->payload[2] << 24)
                         | ((uint32_t)ctx->payload[3] << 16)
                         | ((uint32_t)ctx->payload[4] << 8)
                         |  ctx->payload[5];

            int err = params_write((param_id_t)id, val);
            if (err == 0) {
                bool ok;
                int32_t resp_val = params_read((param_id_t)id, &ok);
                build_param_resp(out, id, (uint32_t)resp_val);
            } else {
                build_error(out, (uint8_t)err, "param write failed");
            }
        }
        return true;

    //-----------------------------------------------------------------
    // CMD_READ_PARAM (0x02): ID(2B), LEN=2
    //-----------------------------------------------------------------
    case CMD_READ_PARAM:
        if (ctx->payload_len != 2) {
            build_error(out, ERR_FRAME_LEN, "READ_PARAM expects 2B payload");
            return true;
        }
        {
            uint16_t id = ((uint16_t)ctx->payload[0] << 8)
                        |  ctx->payload[1];
            bool ok;
            int32_t val = params_read((param_id_t)id, &ok);
            if (ok) {
                build_param_resp(out, id, (uint32_t)val);
            } else {
                build_error(out, ERR_INVALID_PARAM_ID, "unknown param id");
            }
        }
        return true;

    //-----------------------------------------------------------------
    // CMD_WRITE_PARAM_BATCH (0x03): COUNT(1B) + COUNT×{ID(2B)+VALUE(4B)}
    //-----------------------------------------------------------------
    case CMD_WRITE_PARAM_BATCH:
        if (ctx->payload_len < 1) {
            build_error(out, ERR_FRAME_LEN, "BATCH: missing count byte");
            return true;
        }
        {
            uint8_t count = ctx->payload[0];
            if (ctx->payload_len != 1 + count * 6) {
                build_error(out, ERR_FRAME_LEN, "BATCH: length mismatch");
                return true;
            }
            int err = params_write_batch(&ctx->payload[1], count);
            if (err == 0) {
                // 批量写成功 → 空应答
                frame_build(out, CMD_PARAM_RESP, NULL, 0);
            } else {
                build_error(out, (uint8_t)err, "batch write failed");
            }
        }
        return true;

    //-----------------------------------------------------------------
    // CMD_SET_TRIGGER (0x04): SRC(1B) + EDGE(1B) + LEVEL(2B) + PRE(2B), LEN=6
    //-----------------------------------------------------------------
    case CMD_SET_TRIGGER:
        if (ctx->payload_len != 6) {
            build_error(out, ERR_FRAME_LEN, "SET_TRIGGER expects 6B payload");
            return true;
        }
        {
            uint8_t  src   = ctx->payload[0];
            uint8_t  edge  = ctx->payload[1];
            uint16_t level = ((uint16_t)ctx->payload[2] << 8) | ctx->payload[3];
            uint16_t pre   = ((uint16_t)ctx->payload[4] << 8) | ctx->payload[5];

            // 编码触发源+边沿到 PL 寄存器
            // SRC: 0=软件 1=Vout 2=IL
            // EDGE: 0=上升沿 1=下降沿 2=电平
            uint32_t trig_src_code = ((uint32_t)src << 2) | (edge & 0x03);
            pl_write32(PL_REG_TRIG_SRC,   trig_src_code);
            pl_write32(PL_REG_TRIG_LEVEL, level);
            pl_write32(PL_REG_TRIG_ARM,   (pre > 0) ? 1u : 0u);

            LOG_I("Trigger set: src=%d edge=%d level=%u pre=%u",
                  src, edge, level, pre);

            frame_build(out, CMD_PARAM_RESP, NULL, 0);
        }
        return true;

    //-----------------------------------------------------------------
    // CMD_SIM_CTRL (0x05): CTRL(1B), LEN=1
    // CTRL: 0=停止 1=启动 2=复位 3=软触发
    //-----------------------------------------------------------------
    case CMD_SIM_CTRL:
        if (ctx->payload_len != 1) {
            build_error(out, ERR_FRAME_LEN, "SIM_CTRL expects 1B payload");
            return true;
        }
        {
            uint8_t ctrl = ctx->payload[0];
            switch (ctrl) {
            case 0x00:  // STOP
                pl_write32(PL_REG_CTRL, 0x0u);
                LOG_I("Simulation stopped");
                break;
            case 0x01:  // START
                pl_write32(PL_REG_CTRL, 0x1u);  // bit0 = run
                LOG_I("Simulation started");
                break;
            case 0x02:  // RESET
                pl_write32(PL_REG_CTRL, 0x2u);  // bit1 = reset
                // 等待一个周期再清除 reset
                for (volatile int i = 0; i < 10; i++) {}
                pl_write32(PL_REG_CTRL, 0x0u);
                LOG_I("Solver reset");
                break;
            case 0x03:  // SOFT_TRIGGER
                pl_write32(PL_REG_TRIG_SRC, 0u); // 软件触发
                pl_write32(PL_REG_TRIG_ARM, 1u);
                LOG_I("Software trigger");
                break;
            default:
                build_error(out, ERR_INVALID_CMD, "unknown SIM_CTRL subcommand");
                return true;
            }
            frame_build(out, CMD_PARAM_RESP, NULL, 0);
        }
        return true;

    //-----------------------------------------------------------------
    // CMD_GET_STATUS (0x06): 无 payload
    //-----------------------------------------------------------------
    case CMD_GET_STATUS:
        {
            uint32_t status = pl_read32(PL_REG_STATUS);
            uint8_t  state  = (status & 0x1u) ? 1u : 0u;  // 0=idle 1=running
            uint8_t  flags  = 0u;
            if (!(status & 0x2u)) flags |= 0x01u;  // PWM lost
            if (status & 0x4u)   flags |= 0x02u;   // over-current
            // duty, vout, il ← 这里从 PL 读取实际值
            // 简化实现: 读取 PL 状态寄存器低 16 位作为占位值
            uint16_t pwm_freq = 200;   // TODO: 从 pwm_capture 读取
            uint16_t vout_mv  = 0;     // TODO: 从求解器输出读取
            uint16_t il_ma    = 0;
            uint16_t duty     = 0;

            build_status_resp(out, state, flags,
                              pwm_freq, vout_mv, il_ma, duty);
        }
        return true;

    //-----------------------------------------------------------------
    // CMD_SELF_TEST (0x07): 无 payload
    //-----------------------------------------------------------------
    case CMD_SELF_TEST:
        {
            // 简化自检: 检查 PL 寄存器可读写, DAC 初始化状态
            uint32_t test_val = 0xDEADBEEFu;
            pl_write32(PL_REG_FW_VERSION, test_val);
            uint32_t readback = pl_read32(PL_REG_FW_VERSION);
            pl_write32(PL_REG_FW_VERSION, 0x00010000u);  // 恢复

            uint8_t payload[4];
            bool pl_ok = (readback == test_val);
            uint8_t flags = (pl_ok ? 0x01u : 0x00u);
            payload[0] = flags;      // bit0=PL accessible
            payload[1] = 0;          // reserved
            payload[2] = 0;          // reserved
            payload[3] = 0;          // reserved

            frame_build(out, CMD_SELF_TEST_RESP, payload, 4);
            LOG_I("Self-test: PL %s", pl_ok ? "OK" : "FAIL");
        }
        return true;

    //-----------------------------------------------------------------
    // Unknown command
    //-----------------------------------------------------------------
    default:
        build_error(out, ERR_INVALID_CMD, "unknown command");
        return true;
    }
}
