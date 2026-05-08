# Buck 变换器 HIL (Hardware-In-the-Loop) 系统

**基于 Xilinx Zynq UltraScale+ ZU3EG 的 Buck 变换器硬件在环仿真平台**

## 项目概述

本项目实现一个完整的 Buck DC-DC 变换器 HIL 仿真系统，使用 FPGA 以 **100ns 步长**实时求解开关状态模型，通过 DAC 输出模拟电压/电流信号给被测控制器，形成闭环仿真。

### 关键参数

| 参数 | 值 |
|------|-----|
| 被测对象 | Buck DC-DC 变换器 |
| 功率等级 | 2W |
| 开关频率 | 200 kHz |
| 仿真步长 | 100 ns |
| FPGA 平台 | Xilinx ZU3EG (Zynq UltraScale+) |
| 模型类型 | 开关状态模型 (Switched-State) |
| 接口 | PWM 捕获 (400MHz 定时器) + DAC 输出 (16-bit AD5686) |
| 通信 | TCP/IP (1Gbps Ethernet), 自定义二进制帧协议 + CRC16 |
| 上位机 | Qt 6 (C++), QCustomPlot, CMake |

### 系统架构图

```
┌─────────────────────────────────────────────────────────┐
│                   上位机 (Qt 6 / C++)                       │
│  [波形显示] [参数配置] [测试脚本] [数据记录]                │
└──────────────────────┬──────────────────────────────────┘
                       │ TCP/IP (1Gbps)
┌──────────────────────┴──────────────────────────────────┐
│                Zynq UltraScale+ ZU3EG                    │
│  ┌────────────────────────────────────────────────┐     │
│  │  PS (ARM Cortex-A53): Baremetal + lwIP          │     │
│  │  • TCP Server (Port 5000)                       │     │
│  │  • 协议解析 & 参数分发                           │     │
│  │  • DMA 数据流控                                  │     │
│  └──────────────┬─────────────────────────────────┘     │
│                 │ AXI GP / AXI HP (DMA)                  │
│  ┌──────────────┴─────────────────────────────────┐     │
│  │  PL (可编程逻辑)                                 │     │
│  │  • PWM 捕获 (400MHz 计数器)                     │     │
│  │  • Buck 求解器 (100ns 步长, 定点 Q16.16)        │     │
│  │  • DAC 接口 (SPI → AD5686)                      │     │
│  │  • 数据捕获 (8K 环形缓冲, 硬件触发)              │     │
│  └──────────────────────┬──────────────────────────┘     │
│                         │ DAC SPI                         │
│                  ┌──────┴──────┐                          │
│                  │ 模拟前端调理  │                          │
│                  │ 运放 + 滤波   │                          │
│                  └──────┬──────┘                          │
└─────────────────────────┼─────────────────────────────────┘
                          │ 模拟信号 (Vout, I_L)
                   ┌──────┴──────┐
                   │  被测控制器   │
                   │  (MCU/DSP)  │
                   │  ADC ←      │
                   │  PWM →      │
                   └─────────────┘
```

## 文档索引

| 文档 | 内容 |
|------|------|
| [docs/1-system-architecture.md](docs/1-system-architecture.md) | 系统架构总览、时钟域规划、资源估算 |
| [docs/2-pl-fpga-design.md](docs/2-pl-fpga-design.md) | PL 端模块设计（PWM捕获、求解器、DAC接口） |
| [docs/3-communication-protocol.md](docs/3-communication-protocol.md) | PS↔上位机通信协议完整定义 |
| [docs/4-ps-software-design.md](docs/4-ps-software-design.md) | PS 端软件架构（Baremetal + lwIP） |
| [docs/5-host-application.md](docs/5-host-application.md) | 上位机架构（Qt 6 / C++，QThread 线程模型，QCustomPlot） |
| [docs/6-hardware-interface.md](docs/6-hardware-interface.md) | 硬件接口规范（引脚分配、模拟前端、电源） |

## 目录结构

```
buck-hil/
├── README.md
├── docs/                         # 架构设计文档
│   ├── 1-system-architecture.md
│   ├── 2-pl-fpga-design.md
│   ├── 3-communication-protocol.md
│   ├── 4-ps-software-design.md
│   ├── 5-host-application.md
│   └── 6-hardware-interface.md
├── fpga/                         # PL 端代码 (待实现)
│   ├── rtl/
│   │   ├── pwm_capture.v
│   │   ├── buck_solver.v
│   │   ├── dac_interface.v
│   │   └── axi_mm_regs.v
│   ├── constraints/
│   │   └── zu3eg.xdc
│   └── tb/
│       └── buck_solver_tb.sv
├── ps/                           # PS 端代码 (待实现)
│   ├── src/
│   │   ├── main.c
│   │   ├── protocol.c
│   │   ├── tcp_server.c
│   │   └── dma_ctrl.c
│   └── inc/
│       ├── protocol.h
│       └── dma_ctrl.h
├── host/                         # 上位机代码 (待实现)
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── MainWindow.h / .cpp
│   │   ├── CommunicationWorker.h / .cpp
│   │   ├── FrameParser.h / .cpp
│   │   ├── RingBuffer.h
│   │   └── ParameterPanel.h / .cpp
│   └── protocol/
│       └── Protocol.h
└── hardware/                     # 硬件设计 (待实现)
    ├── sch/
    │   └── analog_frontend.sch
    └── layout/
        └── interface_board.pcb
```

## 快速开始

### 环境要求

- **FPGA**: Vivado 2024.1+ (支持 ZU3EG)
- **PS**: Vitis IDE 2024.1+ (Baremetal 工具链)
- **上位机**: Qt 6.5+, CMake 3.20+, C++17 编译器 (GCC 11+/Clang 14+/MSVC 2022+)
- **外部硬件**: AD5686 DAC 模块, 运放调理板

### 构建步骤

1. Vivado 打开 `fpga/` 工程，综合 + 实现，生成 bitstream
2. Vitis 打开 `ps/` 工程，编译固件，生成 BOOT.bin
3. 上位机构建:
   ```bash
   cd host && mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --parallel
   ```
4. 加载 bitstream + 固件到 ZU3EG
5. 启动上位机，连接 TCP 端口 5000

## 许可证

MIT License
