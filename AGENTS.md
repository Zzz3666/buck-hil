# Buck-HIL 项目上下文

> 此文件供 Hermes Agent 在该项目目录下工作时自动加载，包含全部关键约束、架构决策和技术规范。

## 项目标识

- **名称**: Buck 变换器 HIL 仿真平台
- **路径**: `~/projects/buck-hil/`
- **仓库**: https://github.com/Zzz3666/buck-hil
- **许可证**: MIT

## 核心参数（不可随意改）

| 参数 | 值 | 约束来源 |
|------|-----|----------|
| 电源拓扑 | Buck DC-DC | 用户指定 |
| 功率 | 2W | 用户指定 |
| 开关频率 | 200 kHz | 用户指定 |
| 仿真步长 | 100 ns | 设计决策（开关周期 1/50） |
| FPGA | Xilinx ZU3EG (XCZU3EG-1SFVC784I) | 用户已有硬件 |
| DAC | DAC80508 (16-bit, 8-ch, SPI 32-bit, 0~5V out, ±1LSB INL, 5μs settling) | 设计选型 (替换 AD5686) |
| 模型类型 | 开关状态模型 (Switched-State, 非平均值) | 设计决策 |
| 离散化 | 前向欧拉 | 设计决策（100ns 步长下误差 < 0.1%） |
| PL 时钟 | 400MHz (捕获) / 100MHz (求解) | 设计决策 |
| 定点格式 | Q16.16 (i_L, v_C, Vin...) / Q8.24 (L, C 等预计算) | 设计决策 |
| PS 运行环境 | Baremetal + lwIP | 设计决策（中断延迟确定性优先） |
| 上位数方案 | **Qt 6 / C++** (非 C#) | 用户指定，已重写文档 |
| 通信协议 | TCP:5000, 自定义二进制帧, CRC16-CCITT | 设计决策 |

## 架构铁律

1. **UI 线程零阻塞** — 上位机 GUI 线程不做 socket read、文件 I/O、sleep
2. **ISR 只做标志位** — PS 端中断只置 flag，主循环处理
3. **跨时钟域必同步** — 所有 CDC 路径必须经同步链或异步 FIFO
4. **帧解析逐字节状态机** — 不依赖单次 Read 恰好收完一帧（粘包/半包安全）
5. **参数双缓冲** — PL 参数更新在 PWM 周期边界生效，防止半新半旧
6. **求解器带钳位** — i_L 不能为负（二极管特性），v_C 不能为负
7. **CRC 校验每一帧** — 通信协议帧必须 CRC，错误帧丢弃不 panic

## 文档索引

| 文件 | 内容 |
|------|------|
| `docs/1-system-architecture.md` | 系统拓扑、时钟域、数据流、资源估算、风险矩阵 |
| `docs/2-pl-fpga-design.md` | PWM捕获、Buck求解器、DAC接口、AXI-Lite、约束文件 |
| `docs/3-communication-protocol.md` | 帧格式、命令集、参数寄存器表、CRC16、帧解析状态机 |
| `docs/4-ps-software-design.md` | Baremetal架构、lwIP TCP回调、DMA管理、参数预计算 |
| `docs/5-host-application.md` | Qt 6 / C++、QThread模型、FrameParser、QCustomPlot、CMake |
| `docs/6-hardware-interface.md` | 引脚分配、模拟前端、电源树、BOM、验证清单 |

## 目录结构

```
buck-hil/
├── README.md
├── AGENTS.md                       # ← 本文件
├── .gitignore
├── docs/                           # 架构设计文档（完整）
│   ├── 1-system-architecture.md
│   ├── 2-pl-fpga-design.md
│   ├── 3-communication-protocol.md
│   ├── 4-ps-software-design.md
│   ├── 5-host-application.md
│   └── 6-hardware-interface.md
├── fpga/                           # PL 端 RTL（待实现）
│   ├── rtl/   (pwm_capture.v, buck_solver.v, dac_interface.v, axi_mm_regs.v)
│   ├── constraints/ (zu3eg.xdc)
│   └── tb/    (buck_solver_tb.sv)
├── ps/                             # PS 端固件（待实现）
│   ├── src/   (main.c, protocol.c, tcp_server.c, dma_ctrl.c, params.c)
│   └── inc/   (protocol.h, tcp_server.h, dma_ctrl.h, params.h)
├── host/                           # Qt 上位机（待实现）
│   ├── CMakeLists.txt
│   ├── 3rdparty/qcustomplot/
│   ├── src/   (main.cpp, MainWindow, CommunicationWorker, FrameParser, DataProcessor, RingBuffer.h)
│   └── protocol/ (Protocol.h)
└── hardware/                       # 接口板（待设计）
    ├── sch/   (analog_frontend.sch)
    └── layout/
```

## 通信协议速查

```
帧格式:  0xAA  | 0x55 | CMD(1B) | LEN(2B,大端) | PAYLOAD | CRC16(2B,大端) | 0x55
CRC 范围: 从 0x55(含) 到 PAYLOAD 末尾
CRC 多项式: 0x1021, 初始值 0xFFFF
```

**常用命令**:

| CMD | 方向 | 含义 |
|-----|------|------|
| 0x01 | →PS | 写参数 |
| 0x02 | →PS | 读参数 |
| 0x04 | →PS | 设置触发 |
| 0x05 | →PS | 仿真控制 (0x00停/0x01启/0x02复位) |
| 0x06 | →PS | 获取状态 |
| 0x10 | →PC | 参数应答 |
| 0x11 | →PC | 触发数据块 |
| 0x12 | →PC | 连续流数据 |
| 0xFF | →PC | 错误 |

**参数寄存器**:

| ID | 名称 | 单位 |
|----|------|------|
| 0x0001 | L | nH |
| 0x0002 | C | pF |
| 0x0003 | R_load | mΩ |
| 0x0004 | Vin | mV |
| 0x0005 | R_L | mΩ |
| 0x0006 | Vf | mV |
| 0x0007 | F_sw | Hz |
| 0x0008 | IL_MAX | mA |

## 求解器核心方程

```
SW=ON:  di_L/dt = (Vin - Vout - i_L*R_L) / L
         dv_C/dt = (i_L - v_C/R_load) / C

SW=OFF: di_L/dt = (-Vf - Vout - i_L*R_L) / L
         dv_C/dt = (i_L - v_C/R_load) / C

离散化 (前向欧拉, Δt=100ns):
  i_L[n+1] = i_L[n] + Δt * di_L/dt
  v_C[n+1] = v_C[n] + Δt * dv_C/dt

参数预计算 (PS 端负责):
  dt_over_L = 100.0 / L_nh  (Q8.24)
  dt_over_C = 100000.0 / C_pf  (Q8.24)
```

## 数据流路径

```
PWM_IN → pwm_capture(400MHz) → duty_q16 → buck_solver(100MHz) → v_C, i_L
                                                                    ├→ dac_interface(32b SPI) → DAC80508 → 模拟前端(G×2.4) → DUT
                                                                    └→ capture_mgr(BRAM) → DMA → PS DDR → TCP → Host
```

## 闭环延迟

| 环节 | 延迟 |
|------|------|
| PWM 边沿 → duty | ~10 ns |
| duty → 求解器输出 | 100 ns |
| 求解器 → DAC 模拟输出 | ~500 ns |
| 模拟前端传播 | ~100 ns |
| **总延迟** | **< 1 μs** (远小于 5μs 开关周期) |

## 关键风险与对策

| 风险 | 对策 |
|------|------|
| PWM 毛刺 | 5ns 数字滤波 + 3 周期一致性检查 |
| DAC 台阶高频杂散 | 2 阶 RC 重建滤波 (fc=500kHz) |
| 求解器发散 | i_L 下界钳位 (≥0), v_C 下界钳位 |
| TCP 粘包/半包 | 逐字节状态机，不依赖 Read 边界 |
| 模拟前端地回路 | 单点接地，AGND/DGND 经 0Ω |
| ZU3EG 电源纹波 → DAC | 独立 LDO (LP5907-5.0) 给 DAC80508 AVDD |

## 上位机技术栈（Qt）

```
线程模型: MainThread(GUI) + CommThread(QThread) + ProcThread(QThread)
通信方式: 信号槽 QueuedConnection（自动跨线程安全）

关键类:
  FrameParser      — 逐字节状态机 (QObject, emit frameReady)
  CommWorker       — QTcpSocket 异步 (QThread::moveToThread)
  DataProcessor    — RingBuffer 管理 (QThread::moveToThread)
  RingBuffer<T>    — 预分配, atomic writePos
  MainWindow       — QCustomPlot, QTimer 33ms → updatePlot()

CMake: Qt6::Widgets + Qt6::Network + qcustomplot (静态链接)
跨平台: Windows / Linux / macOS
```

## 编码规范

### Verilog/SystemVerilog
- 模块名: `lower_snake_case`
- 信号名: `lower_snake_case`
- 参数: `UPPER_SNAKE_CASE`
- 时钟: `clk` 或 `clk_<freq>`
- 复位: `rst_n` (低有效)
- 所有输出必须寄存器化

### C (PS 固件)
- C99, `snake_case` 函数名
- `typedef struct {} xxx_t;` 风格
- ISR 只用标志位，主循环处理逻辑

### C++ (Qt 上位机)
- C++17, PascalCase 类名, camelCase 方法
- QObject 派生类用 `Q_OBJECT` 宏
- `m_` 前缀成员变量
- 线程创建: `worker->moveToThread(thread)` 模式

## TODO / 下一步

当前阶段：架构设计完成，待进入代码实现。

优先级：
1. `fpga/rtl/buck_solver.v` + testbench
2. `fpga/rtl/pwm_capture.v`
3. `fpga/rtl/dac_interface.v`
4. `ps/src/` 固件
5. `host/src/` 上位机
