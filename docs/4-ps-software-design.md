# 4. PS 端软件设计

## 4.1 运行环境选择

**推荐：Baremetal (Standalone) + lwIP**

| 对比维度 | Baremetal + lwIP | Linux + PREEMPT_RT |
|----------|------------------|---------------------|
| 中断延迟 | < 1 μs | 10~50 μs |
| TCP 吞吐 | lwIP 可达 400Mbps+ | Linux 原生 900Mbps |
| 内存占用 | ~1 MB | ~50 MB |
| 调度抖动 | 无 (单线程轮询) | 存在 |
| 开发复杂度 | 低 (无内核调试) | 中 (设备树、驱动) |
| 文件系统 | 无 (除非 FatFs) | 原生 |
| DMA 控制 | 裸寄存器操作 | 内核驱动 |

**结论**: 对于 HIL 场景的触发锁存需求，Baremetal 的中断确定性不可替代。如果后续需要 Web 远程监控或复杂文件系统，再切 Linux。

## 4.2 内存布局

ZU3EG PS DDR (假设 512MB, 物理基址 0x00000000):

```
0x00000000 ┌────────────────────┐
           │ 程序代码段 (.text)   │  ~128 KB
           │ 只读数据 (.rodata)   │  ~16 KB
           ├────────────────────┤
           │ 数据段 (.data)       │  ~8 KB
           │ BSS (.bss)          │  ~32 KB
           ├────────────────────┤
           │ 堆 (Heap)           │  ~256 KB
           │  lwIP 内存池        │
           │  协议缓冲           │
           ├────────────────────┤
           │ 数据环形缓冲        │  1 MB  (64K × 16B samples)
           │  (触发数据暂存区)    │
           ├────────────────────┤
0x00200000 │ TCP 发送缓冲        │  64 KB  (lwIP TCP_WND)
           │ lwIP PBUF 池       │  512 KB
           ├────────────────────┤
           │ 未使用              │  ~510 MB
           │                     │
0x20000000 └────────────────────┘
```

**DMA 地址对齐要求**：数据环形缓冲必须 64 字节对齐 (AXI HP 要求)。

## 4.3 软件模块

```
main()
 ├── init_platform()
 │    ├── 初始化 MMU / Cache
 │    ├── 配置中断控制器 (GIC)
 │    ├── 使能 FPU (浮点辅助，仅用于参数计算)
 │    └── 初始化性能计数器 (Cycle Counter)
 │
 ├── init_pl()
 │    ├── 加载 FPGA bitstream (PCAP)
 │    ├── 复位 PL 模块
 │    └── 等待 PL 就绪 (检查 INIT_DONE)
 │
 ├── init_dma()
 │    ├── 配置 AXI DMA (S2MM 通道)
 │    ├── 设置 BD (Buffer Descriptor) 链表
 │    ├── 注册 DMA 完成中断 ISR
 │    └── 使能 DMA 通道
 │
 ├── init_network()
 │    ├── lwip_init()
 │    ├── 配置 MAC 地址 / IP (静态 or DHCP)
 │    ├── 创建 TCP PCB (Port 5000)
 │    └── 注册 TCP 回调 (accept/sent/recv/err/poll)
 │
 ├── init_protocol()
 │    ├── 初始化参数寄存器镜像
 │    ├── 设置默认值 (L=100μH, C=100μF, R=10Ω, Vin=12V, Fs=200kHz)
 │    ├── 预计算 dt/L, dt/C 并写入 PL 寄存器
 │    └── 初始化帧解析状态机
 │
 └── main_loop()
      ├── tcp_tmr()              // lwIP 定时器处理 (每 250ms)
      ├── protocol_process()     // 非阻塞，处理接收缓冲中的帧
      ├── trigger_check()        // 检查 PL 触发标志
      ├── dma_process()          // DMA 完成 → 拷贝到 TCP 发送队列
      ├── heartbeat_check()      // 1Hz 心跳发送
      └── wdt_kick()             // 踢看门狗
```

## 4.4 关键模块详解

### 4.4.1 协议处理 (`protocol.c`)

```c
// 帧解析状态机
typedef enum {
    STATE_SYNC1,
    STATE_SYNC2,
    STATE_CMD,
    STATE_LEN_H,
    STATE_LEN_L,
    STATE_PAYLOAD,
    STATE_CRC_H,
    STATE_CRC_L,
    STATE_TAIL
} frame_state_t;

typedef struct {
    frame_state_t state;
    uint8_t       cmd;
    uint16_t      len;
    uint16_t      payload_idx;
    uint8_t       payload[MAX_PAYLOAD];
    uint16_t      crc_received;
    uint32_t      last_byte_time;  // 超时检测
} frame_parser_t;

// 每收到一个字节调用一次
// 返回: 0=继续 1=帧完成 -1=错误
int frame_parse_byte(frame_parser_t *fp, uint8_t byte) {
    fp->last_byte_time = get_tick_count();

    switch (fp->state) {
    case STATE_SYNC1:
        if (byte == 0xAA) fp->state = STATE_SYNC2;
        break;
    case STATE_SYNC2:
        if (byte == 0x55) {
            fp->state = STATE_CMD;
            fp->crc = crc16_init();
            fp->crc = crc16_update(fp->crc, byte);
        } else {
            fp->state = STATE_SYNC1;
        }
        break;
    case STATE_CMD:
        fp->cmd = byte;
        fp->crc = crc16_update(fp->crc, byte);
        fp->state = STATE_LEN_H;
        break;
    case STATE_LEN_H:
        fp->len = (uint16_t)byte << 8;
        fp->crc = crc16_update(fp->crc, byte);
        fp->state = STATE_LEN_L;
        break;
    case STATE_LEN_L:
        fp->len |= byte;
        fp->crc = crc16_update(fp->crc, byte);
        if (fp->len > MAX_PAYLOAD) {
            fp->state = STATE_SYNC1;  // 长度异常
            return -1;
        }
        fp->payload_idx = 0;
        fp->state = (fp->len == 0) ? STATE_CRC_H : STATE_PAYLOAD;
        break;
    case STATE_PAYLOAD:
        fp->payload[fp->payload_idx++] = byte;
        fp->crc = crc16_update(fp->crc, byte);
        if (fp->payload_idx >= fp->len)
            fp->state = STATE_CRC_H;
        break;
    case STATE_CRC_H:
        fp->crc_received = (uint16_t)byte << 8;
        fp->state = STATE_CRC_L;
        break;
    case STATE_CRC_L:
        fp->crc_received |= byte;
        fp->state = STATE_TAIL;
        break;
    case STATE_TAIL:
        fp->state = STATE_SYNC1;
        if (byte == 0x55 && fp->crc == fp->crc_received)
            return 1;  // 帧完成且 CRC 正确
        return -1;     // 帧错误
    }
    return 0;
}
```

### 4.4.2 TCP 服务器 (`tcp_server.c`)

```c
// lwIP TCP 回调
static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err) {
    // 注册回调
    tcp_recv(newpcb, tcp_recv_callback);
    tcp_err(newpcb,  tcp_err_callback);
    tcp_poll(newpcb, tcp_poll_callback, 4);  // 每 2s poll 一次
    tcp_arg(newpcb,  conn_state);
    return ERR_OK;
}

static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb,
                                struct pbuf *p, err_t err) {
    if (p == NULL) {
        // 连接关闭
        tcp_close(tpcb);
        return ERR_OK;
    }

    // 迭代 pbuf 链，逐字节喂给帧解析器
    struct pbuf *q = p;
    while (q != NULL) {
        for (uint16_t i = 0; i < q->len; i++) {
            uint8_t byte = ((uint8_t*)q->payload)[i];
            if (frame_parse_byte(&parser, byte) == 1) {
                // 帧完整，入处理队列
                enqueue_frame(&parser);
            }
        }
        q = q->next;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
```

### 4.4.3 DMA 管理 (`dma_ctrl.c`)

```c
// AXI DMA S2MM 配置
#define DMA_BD_COUNT  16
#define DMA_BD_SIZE   65536  // 每 BD 64KB

typedef struct {
    uint32_t buffer[DMA_BD_SIZE / 4] __attribute__((aligned(64)));
} dma_buffer_t;

// BD 链表配置 (环形)
dma_buffer_t dma_buffers[DMA_BD_COUNT];

void dma_init(void) {
    // 1. 复位 DMA
    XAxiDma_Reset(&axi_dma);

    // 2. 配置 S2MM 通道
    XAxiDma_CfgInitialize(&axi_dma, &dma_config);

    // 3. 设置 BD 链表 (环形)
    for (int i = 0; i < DMA_BD_COUNT; i++) {
        XAxiDma_BdRing *ring = XAxiDma_GetBdRing(&axi_dma, XAXIDMA_DEVICE_TO_DMA);
        XAxiDma_Bd bd;
        XAxiDma_BdCreate(&bd, (UINTPTR)dma_buffers[i].buffer,
                         DMA_BD_SIZE, XAXIDMA_LAST);
        XAxiDma_BdRingToHw(ring, 1, &bd);
    }

    // 4. 注册中断
    XScuGic_Connect(&intc, XPAR_FABRIC_AXI_DMA_0_S2MM_INT_INTR,
                    (Xil_InterruptHandler)dma_done_isr, &axi_dma);

    // 5. 启动
    XAxiDma_BdRingStart(ring);
}

// DMA 完成中断
void dma_done_isr(void *callback_ref) {
    // 仅置标志位，不处理重活
    g_dma_done_flag = 1;

    // 切换到下一个 BD
    dma_bd_idx = (dma_bd_idx + 1) % DMA_BD_COUNT;
    XAxiDma_BdRingFromHw(ring, 1, &bd);
    XAxiDma_BdRingToHw(ring, 1, &bd);
}
```

### 4.4.4 参数管理 (`params.c`)

```c
// 参数镜像 (DDR 中的副本, 与 PL 寄存器保持同步)
typedef struct {
    uint32_t l_nh;        // 0x0001: nH
    uint32_t c_pf;        // 0x0002: pF
    uint32_t r_load_mohm; // 0x0003: mΩ
    uint32_t vin_mv;      // 0x0004: mV
    uint32_t r_l_mohm;    // 0x0005: mΩ
    uint32_t vf_mv;       // 0x0006: mV
    uint32_t f_sw_hz;     // 0x0007: Hz
    uint32_t il_max_ma;   // 0x0008: mA
} param_mirror_t;

static param_mirror_t g_params;

// 预计算 dt/L 和 dt/C (Q8.24 定点)
// dt = 100e-9 s, L 以 nH 输入
// dt/L = 100e-9 / (L * 1e-9) = 100/L
// 在 Q8.24 中: 100.0 → 100 << 24 = 0x64000000
uint32_t calc_dt_over_L(uint32_t l_nh) {
    if (l_nh == 0) return 0xFFFFFFFF;  // 保护
    uint64_t num = (uint64_t)100 << 24; // 100.0 in Q8.24
    return (uint32_t)(num / l_nh);
}

uint32_t calc_dt_over_C(uint32_t c_pf) {
    if (c_pf == 0) return 0xFFFFFFFF;
    // dt/C = 100e-9 / (C * 1e-12) = 100000/C
    uint64_t num = (uint64_t)100000 << 24;
    return (uint32_t)(num / c_pf);
}

// 写参数 (PC→PS 协议处理调用)
int param_write(uint16_t id, uint32_t value) {
    switch (id) {
    case 0x0001:
        if (value < 100 || value > 1000000) return ERR_RANGE;
        g_params.l_nh = value;
        write_pl_reg(REG_L, value);            // 写 L 到 PL
        write_pl_reg(REG_DT_OVER_L, calc_dt_over_L(value));
        break;
    case 0x0002:
        if (value < 100 || value > 100000000) return ERR_RANGE;
        g_params.c_pf = value;
        write_pl_reg(REG_C, value);
        write_pl_reg(REG_DT_OVER_C, calc_dt_over_C(value));
        break;
    // ... 其他参数
    case 0x0007:
        if (value < 10000 || value > 1000000) return ERR_RANGE;
        g_params.f_sw_hz = value;
        write_pl_reg(REG_FSW, value);
        break;
    default:
        return ERR_INVALID_ID;
    }
    return OK;
}
```

## 4.5 中断向量表

| 中断源 | 优先级 | ISR 行为 | 备注 |
|--------|--------|----------|------|
| AXI DMA S2MM 完成 | 最高 (0) | 置标志位，切换 BD | 数据处理链最关键环节 |
| GEM 以太网中断 | 高 (1) | lwIP 内部处理 | 保证 TCP 响应 |
| PL 触发中断 | 中 (2) | 置标志位，记录时间戳 | 只在主循环处理 |
| 看门狗定时器 | 低 (3) | 踢狗 | 系统监控 |
| Triple Timer Counter | 低 (4) | 心跳定时 | 1Hz 心跳 |

**铁律**: ISR 只做标志位和关键数据搬运，不在中断里做协议解析、TCP 发送、参数计算。

## 4.6 看门狗策略

```c
// 系统看门狗 (PS 内置 SWDT)
// 超时: 2 秒
// 踢狗位置: main_loop() 每轮迭代

// 软件看门狗 (监控各子系统)
typedef struct {
    uint32_t last_protocol_activity;  // 协议层最后活动时间
    uint32_t last_dma_activity;       // DMA 最后传输时间
    uint32_t last_heartbeat;          // 最后心跳时间
    uint32_t error_count;            // 累计错误计数
} sw_watchdog_t;

// 在主循环中检查
void sw_wdt_check(void) {
    uint32_t now = get_tick_count();

    // 协议层无活动 > 30s → 日志告警
    if (now - sw_wdt.last_protocol_activity > 30000) {
        log_event(EVT_PROTOCOL_TIMEOUT);
    }

    // DMA 无活动 > 5s (运行状态) → 复位 DMA
    if (g_sim_running && (now - sw_wdt.last_dma_activity > 5000)) {
        dma_reset();
    }
}
```

## 4.7 构建系统

**Vitis 工程结构**:
```
ps/
├── src/
│   ├── main.c              # 入口 + 主循环
│   ├── protocol.c          # 帧解析 + 命令处理
│   ├── tcp_server.c        # lwIP TCP 服务端
│   ├── dma_ctrl.c          # AXI DMA 控制
│   ├── params.c            # 参数管理 + 预计算
│   ├── logger.c            # 日志 (UART + 内存环)
│   └── platform.c          # 平台初始化
├── inc/
│   ├── protocol.h
│   ├── tcp_server.h
│   ├── dma_ctrl.h
│   ├── params.h
│   └── logger.h
├── lscript.ld              # 链接脚本
└── vitis.prj               # Vitis 工程文件
```

**链接脚本关键段**:
```
DDR_MEMORY : ORIGIN = 0x00100000, LENGTH = 0x1FF00000  // 511MB 可用

.text   : { *(.text*) } > DDR_MEMORY
.rodata : { *(.rodata*) } > DDR_MEMORY
.data   : { *(.data*) } > DDR_MEMORY
.bss    : { *(.bss*) } > DDR_MEMORY

.dma_buffers ALIGN(64) : {
    _dma_buffer_start = .;
    . += 1M;  // 预留 1MB DMA 缓冲
    _dma_buffer_end = .;
} > DDR_MEMORY
```
