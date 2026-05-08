# 3. 通信协议规范

## 3.1 概述

- **传输层**: TCP (PS 端为 Server，端口 5000)
- **数据格式**: 二进制帧，固定帧头/帧尾 + 长度字段
- **校验**: CRC16-CCITT (多项式 0x1021)
- **字节序**: 多字节字段全部采用**大端序 (Big-Endian)**

## 3.2 帧格式

```
┌──────┬──────┬──────┬──────────┬──────────┬──────┬──────┐
│ 0xAA │ 0x55 │ CMD  │ LEN (2B) │ PAYLOAD  │ CRC16│ 0x55 │
│ 2B   │      │ 1B   │ 2B       │ N 字节   │ 2B   │ 1B   │
│ 帧头  │      │ 命令 │ 负载长度  │  数据     │ 校验 │ 帧尾  │
└──────┴──────┴──────┴──────────┴──────────┴──────┴──────┘
```

| 字段 | 大小 | 说明 |
|------|------|------|
| HEAD1 | 1B | 固定值 0xAA |
| HEAD2 | 1B | 固定值 0x55 |
| CMD | 1B | 命令码 |
| LEN | 2B | PAYLOAD 字节数 (0~65535), 大端 |
| PAYLOAD | N | 负载数据 |
| CRC16 | 2B | HEAD2 到 PAYLOAD 末尾的 CRC16-CCITT, 大端 |
| TAIL | 1B | 固定值 0x55 |

**CRC 计算范围**：从 HEAD2 (含) 到 PAYLOAD 末尾 (含)。HEAD1 和 TAIL 不参与 CRC。

## 3.3 命令集

### 3.3.1 命令表

| CMD | 方向 | 名称 | 说明 |
|-----|------|------|------|
| `0x01` | PC→PS | CMD_WRITE_PARAM | 写单个参数 |
| `0x02` | PC→PS | CMD_READ_PARAM | 读单个参数 |
| `0x03` | PC→PS | CMD_WRITE_PARAM_BATCH | 批量写参数 |
| `0x04` | PC→PS | CMD_SET_TRIGGER | 设置触发条件 |
| `0x05` | PC→PS | CMD_SIM_CTRL | 仿真控制 (启动/停止/复位) |
| `0x06` | PC→PS | CMD_GET_STATUS | 获取系统状态 |
| `0x07` | PC→PS | CMD_SELF_TEST | 自检命令 |
| `0x10` | PS→PC | CMD_PARAM_RESP | 参数读应答 |
| `0x11` | PS→PC | CMD_TRIG_DATA | 触发数据块 |
| `0x12` | PS→PC | CMD_STREAM_DATA | 连续流数据 |
| `0x13` | PS→PC | CMD_STATUS_RESP | 状态应答 |
| `0x14` | PS→PC | CMD_SELF_TEST_RESP | 自检结果 |
| `0xFE` | PS→PC | CMD_ASYNC_EVENT | 异步事件 (上电/PWM丢失/…) |
| `0xFF` | PS→PC | CMD_ERROR | 错误响应 |

### 3.3.2 CMD_WRITE_PARAM (0x01)

**请求** (PC→PS):
```
 0  1  2  3  4  5  6  7
┌──┬──┬──┬──┬──┬──┬──┬──┐
│  PARAM_ID   │    VALUE      │
│  2B (大端)  │    4B (大端)   │
└──┴──┴──┴──┴──┴──┴──┴──┘
LEN = 6
```

**示例**：设置 L=25μH (寄存器 0x0001, 值=25000 nH)
```
AA 55 01 00 06   00 01   00 00 61 A8   CRC16  55
         ^CMD     ^ID     ^VALUE=25000
```

### 3.3.3 CMD_READ_PARAM (0x02)

**请求** (PC→PS):
```
┌──┬──┐
│ PARAM_ID │
│  2B      │
└──┴──┘
LEN = 2
```

**应答** CMD_PARAM_RESP (0x10):
```
┌──┬──┬──────────┐
│ PARAM_ID  │ VALUE     │
│  2B       │ 4B        │
└──┴──┴──────────┘
LEN = 6
```

### 3.3.4 CMD_WRITE_PARAM_BATCH (0x03)

批量写入，避免多次 TCP 小包。

```
┌──────┬─────────────────────────────────────────┐
│COUNT │ PARAM[0]: {ID:2B, VALUE:4B}            │
│ 1B   │ PARAM[1]: {ID:2B, VALUE:4B}            │
│      │ ...                                     │
└──────┴─────────────────────────────────────────┘
LEN = 1 + COUNT*6
```

### 3.3.5 CMD_SET_TRIGGER (0x04)

```
┌──────┬──────┬──────────┬──────┐
│ SRC  │ EDGE │  LEVEL   │ PRE  │
│ 1B   │ 1B   │  2B      │ 2B   │
└──────┴──────┴──────────┴──────┘
LEN = 6

SRC:  0=软件触发  1=Vout  2=I_L
EDGE: 0=上升沿    1=下降沿  2=电平(高于LEVEL触发)
LEVEL: 触发阈值 (取决于 SRC 的工程单位 ×1000)
PRE:   pre-trigger 采样点数 (0~4096)
```

**示例**：Vout 上升沿触发，阈值 5.0V，预触发 512 点
```
AA 55 04 00 06   01   00   13 88   02 00   CRC16  55
                  ^Vout ^上升  ^5000mV  ^512
```

### 3.3.6 CMD_SIM_CTRL (0x05)

```
┌──────┐
│ CTRL │
│ 1B   │
└──────┘
LEN = 1

CTRL: 0x00=停止仿真  0x01=启动仿真  0x02=复位求解器  0x03=软件触发
```

### 3.3.7 CMD_GET_STATUS (0x06)

**请求**: 无 PAYLOAD, LEN=0

**应答** CMD_STATUS_RESP (0x13):
```
┌──────┬──────┬──────────┬──────────┬──────────┬──────────┐
│STATE │FLAGS │PWM_FREQ  │ VOUT_MV  │  IL_MA   │  DUTY    │
│ 1B   │ 1B   │  2B      │  2B      │  2B      │  2B      │
└──────┴──────┴──────────┴──────────┴──────────┴──────────┘
LEN = 10

STATE:  0=空闲  1=运行  2=错误  3=触发中
FLAGS:  bit0=PWM丢失  bit1=过流  bit2=过压  bit3=DAC通信错误
```

### 3.3.8 CMD_TRIG_DATA (0x11)

PS→PC，触发发生后的数据块。

```
┌──────┬──────┬──────────┬──────────────────────────┐
│SEQ#  │COUNT │CH_MASK   │ SAMPLE[0..COUNT-1]       │
│ 4B   │ 2B   │ 1B       │ 每个采样 8B              │
└──────┴──────┴──────────┴──────────────────────────┘

LEN = 7 + COUNT*8

每个采样 (8B, 大端):
┌──────────┬──────────┬──────────┬──────────┐
│ VOUT_16  │  IL_16   │ DUTY_16  │  TS_16   │
└──────────┴──────────┴──────────┴──────────┘
各字段为原始 AD5686 量化值

CH_MASK: bit0=Vout有效 bit1=IL有效 bit2=Duty有效
```

### 3.3.9 CMD_ERROR (0xFF)

```
┌──────┬──────────┐
│CODE  │ MESSAGE  │
│ 1B   │ N 字节   │
└──────┴──────────┘

错误码:
  0x01 - 无效命令
  0x02 - 无效参数ID
  0x03 - 参数值超范围
  0x04 - CRC 校验失败
  0x05 - 帧长度错误
  0x06 - 仿真未就绪
  0x07 - DMA 传输错误
  0x08 - 内部缓冲区溢出
```

## 3.4 参数寄存器定义

| ID | 名称 | 单位 | 数据类型 | 范围 | R/W | 说明 |
|----|------|------|----------|------|-----|------|
| 0x0001 | L | nH | uint32 | 100~1000000 | R/W | 电感值 |
| 0x0002 | C | pF | uint32 | 100~100000000 | R/W | 输出电容 |
| 0x0003 | R_LOAD | mΩ | uint32 | 10~1000000 | R/W | 负载电阻 |
| 0x0004 | VIN | mV | uint32 | 0~50000 | R/W | 输入电压 |
| 0x0005 | R_L | mΩ | uint32 | 0~100000 | R/W | 电感寄生电阻 |
| 0x0006 | VF | mV | uint32 | 0~2000 | R/W | 二极管正向压降 |
| 0x0007 | F_SW | Hz | uint32 | 10000~1000000 | R/W | 开关频率 |
| 0x0008 | IL_MAX | mA | uint32 | 0~20000 | R/W | 电感电流上限 |
| 0x0009 | VOUT_SCALE | - | uint32 | — | R | DAC 缩放因子 |
| 0x000A | IL_SCALE | - | uint32 | — | R | IL 缩放因子 |
| 0x000B | DAC_UPDATE_DIV | - | uint32 | 1~1000 | R/W | DAC 更新分频 (每N求解步更新一次) |
| 0x0100 | FW_VERSION | - | uint32 | — | R | 固件版本号 (BCD) |
| 0x0101 | DEVICE_ID | - | uint32 | — | R | 设备唯一ID |

## 3.5 CRC16 实现

```c
// CRC16-CCITT, 多项式 0x1021, 初始值 0xFFFF
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc = crc << 1;
        }
    }
    return crc;
}
```

## 3.6 通信时序示例

### 3.6.1 典型会话

```
PC:  [CMD_GET_STATUS]         → PS (请求当前状态)
PS:  [CMD_STATUS_RESP: IDLE]  → PC (系统空闲)

PC:  [CMD_WRITE_PARAM: L=100μH]
PS:  [CMD_PARAM_RESP: OK]

PC:  [CMD_WRITE_PARAM: C=47μF]
PS:  [CMD_PARAM_RESP: OK]

PC:  [CMD_WRITE_PARAM: R_LOAD=10Ω]
PS:  [CMD_PARAM_RESP: OK]

PC:  [CMD_WRITE_PARAM: VIN=12V]
PS:  [CMD_PARAM_RESP: OK]

PC:  [CMD_SET_TRIGGER: Vout上升沿 5V pre=512]
PS:  [CMD_PARAM_RESP: OK]     (触发设置确认)

PC:  [CMD_SIM_CTRL: START]
PS:  [CMD_STATUS_RESP: RUNNING]

PS:  [CMD_STREAM_DATA: ...]   → PC (连续波形数据, 可选)
PS:  [CMD_STREAM_DATA: ...]

... (等待触发条件)

PS:  [CMD_TRIG_DATA: seq=1, 4096点]  → PC
PS:  [CMD_TRIG_DATA: seq=2, 4096点]  → PC (如果触发条件继续满足)

PC:  [CMD_SIM_CTRL: STOP]
PS:  [CMD_STATUS_RESP: IDLE]
```

### 3.6.2 错误恢复

```
PC:  [CMD_WRITE_PARAM: L=100μH]
     ... 网络丢包 ...

PC:  [超时 500ms, 重发 CMD_WRITE_PARAM]
PS:  [CMD_PARAM_RESP: OK]
     ◆ 重试成功，PC 端不报告错误
```

## 3.7 超时与重传

| 参数 | 值 | 说明 |
|------|-----|------|
| 请求超时 | 500 ms | PC 发出请求后等待应答的最长时间 |
| 最大重试次数 | 3 | 超时后重试，3 次后报错 |
| 连续流超时 | 2000 ms | 超过此时间未收到流数据 → 认为链路中断 |
| 心跳间隔 | 1000 ms | PS 端周期性发送 `CMD_ASYNC_EVENT: HEARTBEAT` |
| 心跳丢失 | 连续丢失 3 个心跳 → PC 报断线 |
| TCP Keep-Alive | 启用, 间隔 5s | OS 层保活 |

## 3.8 帧解析状态机 (接收端参考实现)

```
          ┌────────┐
          │ SYNC1  │ ◄── 搜索 0xAA
          └───┬────┘
              │ 收到 0xAA
              ▼
          ┌────────┐
          │ SYNC2  │ ◄── 期望 0x55, 否则回到 SYNC1
          └───┬────┘
              │ 收到 0x55
              ▼
          ┌────────┐
          │ CMD    │ ◄── 读取 CMD (1B)
          └───┬────┘
              │
              ▼
          ┌────────┐
          │ LEN_H  │ ◄── 读取 LEN 高字节
          └───┬────┘
              │
              ▼
          ┌────────┐
          │ LEN_L  │ ◄── 读取 LEN 低字节
          └───┬────┘
              │
              ▼
          ┌────────────┐
          │ PAYLOAD    │ ◄── 接收 LEN 字节
          │ (循环收N字节)│
          └─────┬──────┘
                │
                ▼
          ┌────────────┐
          │ CRC_H      │ ◄── 接收 CRC 高字节
          └─────┬──────┘
                │
                ▼
          ┌────────────┐
          │ CRC_L      │ ◄── 接收 CRC 低字节
          └─────┬──────┘
                │
                ▼
          ┌────────────┐
          │ TAIL       │ ◄── 期望 0x55
          └─────┬──────┘
                │ 0x55 → 校验 CRC → 若通过则处理帧
                │ 非0x55 → 错误，回到 SYNC1
                ▼
          [处理帧]
          然后回到 SYNC1
```

**关键设计原则**：
1. 每个状态都有超时保护（500ms 无数据 → 回 SYNC1）
2. 状态机逐字节推进，天然支持粘包/半包
3. 非预期的 0xAA/0x55 会触发重新同步，但不会误判
4. CRC 错误丢弃帧，不清空状态 → 自动恢复同步
