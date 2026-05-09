//=============================================================================
// platform.h — ZU3EG Baremetal 平台初始化
//
// 职责: MMU/Cache 配置, GIC 中断控制器, FPU, 性能计数器, PL 加载
//=============================================================================

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// PL 寄存器基址 (由 Vivado Address Editor 分配, 此处为占位符)
// 最终值在 Vivado 导出 .xsa 后确定, 通过 xparameters.h 自动生成
//=============================================================================
#ifndef XPAR_AXI_LITE_REGS_BASEADDR
#define PL_REG_BASE          0xA0000000u  // 占位, 运行时从 xparameters.h 覆盖
#else
#define PL_REG_BASE          XPAR_AXI_LITE_REGS_BASEADDR
#endif

#ifndef XPAR_AXI_DMA_0_BASEADDR
#define AXI_DMA_BASE         0xA0010000u
#else
#define AXI_DMA_BASE         XPAR_AXI_DMA_0_BASEADDR
#endif

#ifndef XPAR_FABRIC_AXI_DMA_0_S2MM_INTR
#define DMA_S2MM_INTR_ID     61u
#else
#define DMA_S2MM_INTR_ID     XPAR_FABRIC_AXI_DMA_0_S2MM_INTR
#endif

//=============================================================================
// AXI-Lite 寄存器读写 (32-bit, 4字节对齐)
//=============================================================================
static inline uint32_t pl_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(PL_REG_BASE + offset);
}

static inline void pl_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(PL_REG_BASE + offset) = value;
}

//=============================================================================
// 系统节拍 (1ms 分辨率)
//=============================================================================
uint32_t platform_tick_ms(void);
uint64_t platform_tick_us(void);

//=============================================================================
// API
//=============================================================================
void platform_init(void);               // MMU, Cache, GIC, FPU, TTC
void platform_init_pl(void);            // PL 加载与复位
void platform_init_interrupts(void);    // 注册全部中断 (DMA, Timer, GEM)
void platform_enable_interrupts(void);  // 全局开中断
void platform_disable_interrupts(void); // 全局关中断

// 忙等待延时
void platform_delay_ms(uint32_t ms);
void platform_delay_us(uint32_t us);

// 软件复位
void platform_soft_reset(void) __attribute__((noreturn));

// 看门狗
void platform_wdt_start(uint32_t timeout_ms);
void platform_wdt_kick(void);

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_H
