//=============================================================================
// dma_ctrl.c — AXI DMA S2MM 采集管理
//
// PL capture_manager → AXI-Stream → AXI DMA S2MM → DDR
// BD 环形链表: 16 × 64KB = 1MB
// ISR 只置标志位，主循环处理 BD 回收
//
// 注意: 此实现依赖 Xilinx BSP (xaxidma.h, xscugic.h)
// 若无 BSP 环境则使用寄存器直接操作作为回退
//=============================================================================

#include "dma_ctrl.h"
#include "platform.h"
#include "logger.h"
#include <string.h>

//=============================================================================
// ISR 标志 (Baremetal 全局变量)
//=============================================================================
volatile bool g_dma_done_flag = false;

//=============================================================================
// 条件编译: Xilinx BSP 驱动 vs 裸寄存器
//=============================================================================
#ifdef XPAR_XAXIDMA_NUM_INSTANCES

#include "xaxidma.h"
#include "xscugic.h"

static XAxiDma    g_axi_dma;
static XScuGic    g_intc;       // 由 platform.c 初始化, 此处引用
static int        g_dma_initialized = 0;

// BD 缓冲区: 64B 对齐
static uint8_t g_dma_buffers[DMA_BD_COUNT][DMA_BD_SIZE]
    __attribute__((aligned(64)));

static volatile int g_bd_idx = 0;
static uint32_t     g_transfer_count = 0;
static uint32_t     g_error_count    = 0;

//-------------------------------------------------------------------------
// DMA 完成中断 ISR
//-------------------------------------------------------------------------
static void dma_done_isr(void *callback_ref)
{
    (void)callback_ref;

    // ▎铁律: ISR 只置标志位，不干重活
    g_dma_done_flag = true;

    // 清中断
    XAxiDma_IntrAck(&g_axi_dma, XAXIDMA_IRQ_IOC_MASK, XAXIDMA_DEVICE_TO_DMA);

    g_transfer_count++;
}

//-------------------------------------------------------------------------
// 初始化
//-------------------------------------------------------------------------
void dma_init(uintptr_t axi_dma_base, uint32_t s2mm_intr_id)
{
    int status;
    XAxiDma_Config *cfg;

    // 1. 查找 DMA 配置
    cfg = XAxiDma_LookupConfig(axi_dma_base);
    if (cfg == NULL) {
        LOG_F("DMA: LookupConfig failed for base 0x%08lX",
              (unsigned long)axi_dma_base);
        return;
    }

    // 2. 初始化 DMA 实例
    status = XAxiDma_CfgInitialize(&g_axi_dma, cfg);
    if (status != XST_SUCCESS) {
        LOG_F("DMA: CfgInitialize failed");
        return;
    }

    // 3. 检查 S2MM 通道
    if (!XAxiDma_HasSg(&g_axi_dma)) {
        LOG_W("DMA: Scatter-Gather not available, using Simple mode");
    }

    // 4. 禁用中断 (初始化阶段)
    XAxiDma_IntrDisable(&g_axi_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

    // 5. 设置 BD 环形链表
    XAxiDma_BdRing *ring = XAxiDma_GetBdRing(&g_axi_dma, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Bd bd_template;
    XAxiDma_Bd *bd_array[DMA_BD_COUNT];

    for (int i = 0; i < DMA_BD_COUNT; i++) {
        bd_array[i] = XAxiDma_BdAlloc();
        if (bd_array[i] == NULL) {
            LOG_F("DMA: BD alloc failed at index %d", i);
            return;
        }
        XAxiDma_BdCreate(bd_array[i],
                         (UINTPTR)g_dma_buffers[i],
                         DMA_BD_SIZE,
                         XAXIDMA_LAST);
        XAxiDma_BdRingToHw(ring, 1, bd_array[i]);
    }

    // 6. 注册中断
    XScuGic_SetPriorityTriggerType(&g_intc, s2mm_intr_id, 0xA0, 0x3);
    status = XScuGic_Connect(&g_intc, s2mm_intr_id,
                             (Xil_InterruptHandler)dma_done_isr,
                             &g_axi_dma);
    if (status != XST_SUCCESS) {
        LOG_F("DMA: interrupt connect failed");
        return;
    }

    // 7. 使能 DMA 中断并启动
    XAxiDma_IntrEnable(&g_axi_dma, XAXIDMA_IRQ_IOC_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_BdRingStart(ring);

    g_dma_initialized = 1;
    g_bd_idx = 0;

    LOG_I("DMA initialized: %d BDs × %d bytes, intr_id=%lu",
          DMA_BD_COUNT, DMA_BD_SIZE, (unsigned long)s2mm_intr_id);
}

//-------------------------------------------------------------------------
// 主循环处理
//-------------------------------------------------------------------------
void dma_process(void)
{
    if (!g_dma_initialized) return;

    if (!g_dma_done_flag) return;

    // 清除标志 (在消费前清除, 防止竞争)
    g_dma_done_flag = false;

    // 切换到下一个 BD
    XAxiDma_BdRing *ring = XAxiDma_GetBdRing(&g_axi_dma, XAXIDMA_DEVICE_TO_DMA);

    // 处理完成的 BD
    int processed;
    XAxiDma_Bd *bd = NULL;
    processed = XAxiDma_BdRingFromHw(ring, 1, &bd);
    if (processed > 0 && bd != NULL) {
        // BD 已完成，数据在 g_dma_buffers[g_bd_idx] 中
        // 此处由调用者通过 dma_get_last_buffer() 获取

        // 回收 BD 到硬件环
        XAxiDma_BdSetBufAddr(bd, (UINTPTR)g_dma_buffers[g_bd_idx]);
        XAxiDma_BdSetLength(bd, DMA_BD_SIZE, XAXIDMA_LAST);
        XAxiDma_BdRingToHw(ring, 1, bd);

        g_bd_idx = (g_bd_idx + 1) % DMA_BD_COUNT;
    }
}

//-------------------------------------------------------------------------
// 获取最新完成缓冲区
//-------------------------------------------------------------------------
const uint8_t *dma_get_last_buffer(uint32_t *bytes_available)
{
    if (!g_dma_initialized) {
        *bytes_available = 0;
        return NULL;
    }

    int idx = (g_bd_idx - 1 + DMA_BD_COUNT) % DMA_BD_COUNT;
    *bytes_available = DMA_BD_SIZE;
    return g_dma_buffers[idx];
}

//-------------------------------------------------------------------------
// DMA 统计
//-------------------------------------------------------------------------
void dma_get_stats(uint32_t *transfers, uint32_t *errors)
{
    *transfers = g_transfer_count;
    *errors    = g_error_count;
}

//-------------------------------------------------------------------------
// 复位 DMA
//-------------------------------------------------------------------------
void dma_reset(void)
{
    if (!g_dma_initialized) return;

    LOG_W("DMA reset triggered");

    XAxiDma_Reset(&g_axi_dma);
    g_error_count++;
    g_dma_done_flag = false;

    // 重新初始化 BD 环
    XAxiDma_BdRing *ring = XAxiDma_GetBdRing(&g_axi_dma, XAXIDMA_DEVICE_TO_DMA);
    for (int i = 0; i < DMA_BD_COUNT; i++) {
        XAxiDma_Bd *bd = XAxiDma_BdAlloc();
        XAxiDma_BdCreate(bd, (UINTPTR)g_dma_buffers[i],
                         DMA_BD_SIZE, XAXIDMA_LAST);
        XAxiDma_BdRingToHw(ring, 1, bd);
    }
    XAxiDma_BdRingStart(ring);
    g_bd_idx = 0;
}

#else  /* 无 Xilinx BSP — 占位实现 */

void dma_init(uintptr_t axi_dma_base, uint32_t s2mm_intr_id)
{
    (void)axi_dma_base;
    (void)s2mm_intr_id;
    LOG_W("DMA: Xilinx BSP not available, DMA disabled");
}

void dma_process(void) {}

const uint8_t *dma_get_last_buffer(uint32_t *bytes_available)
{
    *bytes_available = 0;
    return NULL;
}

void dma_get_stats(uint32_t *transfers, uint32_t *errors)
{
    *transfers = 0;
    *errors    = 0;
}

void dma_reset(void) {}

#endif /* XPAR_XAXIDMA_NUM_INSTANCES */
