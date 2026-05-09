//=============================================================================
// dma_ctrl.h — AXI DMA S2MM 采集管理
//
// capture_manager (PL) → AXI-Stream → AXI DMA S2MM → DDR
// BD 环形链表: 16 个 BD, 每 BD 64KB, 总计 1MB 缓冲区
// ISR 只置标志位, 主循环处理 BD 回收和数据拷贝
//=============================================================================

#ifndef DMA_CTRL_H
#define DMA_CTRL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMA_BD_COUNT    16
#define DMA_BD_SIZE     65536       // 64KB per BD
#define DMA_TRIG_SIZE   32768       // 触发抓取 32KB (4096 采样 × 8B)

// DMA 完成标志 (ISR 中置位, 主循环处理并清零)
extern volatile bool g_dma_done_flag;

//=============================================================================
// API
//=============================================================================

// 初始化 DMA (设置 BD 链表, 注册中断, 启动)
void dma_init(uintptr_t axi_dma_base, uint32_t s2mm_intr_id);

// 主循环处理 DMA 完成 (在非中断上下文中调用)
void dma_process(void);

// 获取最新完成的 BD 缓冲区指针 (调用者负责在下次 dma_process 前消费)
const uint8_t *dma_get_last_buffer(uint32_t *bytes_available);

// 获取 DMA 统计
void dma_get_stats(uint32_t *transfers, uint32_t *errors);

// 复位 DMA (错误恢复)
void dma_reset(void);

#ifdef __cplusplus
}
#endif

#endif // DMA_CTRL_H
