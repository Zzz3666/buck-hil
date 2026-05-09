//=============================================================================
// platform.c — ZU3EG (Cortex-A53) Baremetal 平台初始化
//
// 职责: MMU/Cache, GIC v2, TTC (系统节拍), PL 加载
//
// 依赖: Xilinx BSP (xil_mmu.h, xscugic.h, xil_cache.h, xttcps.h, xfpga.h)
// 无 BSP 时提供最小回退实现
//=============================================================================

#include "platform.h"
#include "logger.h"

//=============================================================================
// 条件编译: 完整 BSP 实现 vs 最小回退
//=============================================================================
#ifdef XPAR_CPU_CORTEXA53_0_TIMESTAMP_CLK_FREQ

//=============================================================================
// 完整 BSP 实现
//=============================================================================
#include "xil_mmu.h"
#include "xil_cache.h"
#include "xscugic.h"
#include "xttcps.h"
#include "xil_exception.h"
#include "xpseudo_asm.h"

//=============================================================================
// 全局变量
//=============================================================================
static XScuGic g_intc;
static XTtcPs  g_ttc;

static volatile uint32_t g_tick_ms = 0;

//=============================================================================
// TTC 定时器中断 (1ms) → 系统节拍
//=============================================================================
static void ttc_isr(void *arg)
{
    (void)arg;
    uint32_t status = XTtcPs_GetInterruptStatus(&g_ttc);
    if (status & XTTCPS_IXR_INTERVAL_MASK) {
        g_tick_ms++;
        XTtcPs_ClearInterruptStatus(&g_ttc, XTTCPS_IXR_INTERVAL_MASK);
    }
}

//=============================================================================
// MMU 配置: 为 PL AXI-Lite 和 DMA 缓冲区设置 Device/normal-cacheable 属性
//=============================================================================
static void mmu_setup(void)
{
    Xil_SetTlbAttributes(PL_REG_BASE, NORM_NONCACHE | PRIV_RW_USER_RW);

    // DDR 可缓存 (0x00000000 ~ 0x1FFFFFFF, 512MB)
    // 默认 DDR 已在 translation table 中标记为 normal memory
    Xil_ICacheEnable();
    Xil_DCacheEnable();
}

//=============================================================================
// GIC 初始化
//=============================================================================
static void gic_setup(void)
{
    XScuGic_Config *cfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    if (cfg == NULL) {
        LOG_F("GIC: LookupConfig failed");
        return;
    }

    int status = XScuGic_CfgInitialize(&g_intc, cfg, cfg->CpuBaseAddress);
    if (status != XST_SUCCESS) {
        LOG_F("GIC: CfgInitialize failed");
        return;
    }

    // 注册异常处理
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                  (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                  &g_intc);

    // 注册 TTC 中断
    XScuGic_SetPriorityTriggerType(&g_intc, XPAR_XTTCPS_0_INTR,
                                    0xA0, 0x03);
    status = XScuGic_Connect(&g_intc, XPAR_XTTCPS_0_INTR,
                              (Xil_InterruptHandler)ttc_isr, &g_ttc);
    if (status != XST_SUCCESS) {
        LOG_F("GIC: TTC connect failed");
        return;
    }

    LOG_I("GIC initialized");
}

//=============================================================================
// TTC 系统节拍初始化 (1ms)
//=============================================================================
static void ttc_setup(void)
{
    XTtcPs_Config *cfg = XTtcPs_LookupConfig(XPAR_XTTCPS_0_DEVICE_ID);
    if (cfg == NULL) {
        LOG_F("TTC: LookupConfig failed");
        return;
    }

    int status = XTtcPs_CfgInitialize(&g_ttc, cfg, cfg->BaseAddress);
    if (status != XST_SUCCESS) {
        LOG_F("TTC: CfgInitialize failed");
        return;
    }

    // 预分频: 使 TTC 每 1ms 产生一次中断
    // TTC 时钟 = CPU_1x = ~100MHz (实际由 BSP 设定)
    // 1ms @ 100MHz = 100000 计数
    uint32_t prescaler = 0;
    uint32_t interval  = XPAR_CPU_CORTEXA53_0_TIMESTAMP_CLK_FREQ / 1000 - 1;

    XTtcPs_SetPrescaler(&g_ttc, prescaler);
    XTtcPs_SetMatchValue(&g_ttc, 0, interval);
    XTtcPs_SetOptions(&g_ttc, XTTCPS_OPTION_INTERVAL_MODE
                              | XTTCPS_OPTION_WAVE_DISABLE);

    // 使能中断
    XTtcPs_SetInterruptEnable(&g_ttc, XTTCPS_IXR_INTERVAL_MASK);
    XScuGic_Enable(&g_intc, XPAR_XTTCPS_0_INTR);

    // 启动
    XTtcPs_Start(&g_ttc);

    LOG_I("TTC timer started: interval=%lu, freq=%lu Hz",
          (unsigned long)interval,
          (unsigned long)XPAR_CPU_CORTEXA53_0_TIMESTAMP_CLK_FREQ);
}

//=============================================================================
// FPU 使能
//=============================================================================
static void fpu_enable(void)
{
    // Cortex-A53: 设置 CPACR 使能 FPU
    // CPACR_EL1: bit[21:20] = 0b11 → Enable FPU/SIMD
    uint64_t cpacr;
    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3 << 20);
    asm volatile("msr cpacr_el1, %0" : : "r"(cpacr));
    asm volatile("isb");
}

//=============================================================================
// 平台初始化
//=============================================================================
void platform_init(void)
{
    // 1. FPU
    fpu_enable();

    // 2. MMU / Cache
    mmu_setup();

    // 3. GIC
    gic_setup();

    // 4. TTC 节拍
    ttc_setup();

    LOG_I("Platform initialized: ZU3EG Cortex-A53, GICv2, TTC 1ms");
}

//=============================================================================
// PL 初始化
//=============================================================================
void platform_init_pl(void)
{
#ifdef XPAR_XFPGA_NUM_INSTANCES
    XFpga fpga;
    XFpga_Config *cfg = XFpga_LookupConfig(XPAR_XFPGA_0_DEVICE_ID);
    if (cfg == NULL) {
        LOG_F("FPGA: LookupConfig failed");
        return;
    }

    int status = XFpga_CfgInitialize(&fpga, cfg);
    if (status != XST_SUCCESS) {
        LOG_F("FPGA: CfgInitialize failed");
        return;
    }

    // 加载 bitstream (PCAP)
    // 注: 实际 bitstream 地址由 FSBL 或 BootROM 决定
    // 如果 bitstream 已由 BootROM 加载，此步骤可跳过
    LOG_I("FPGA bitstream load requested");

    // 等待 PL INIT_DONE
    // while (!XFpga_IsInitDone(&fpga)) {}
#endif

    // 复位 PL 模块
    pl_write32(PL_REG_CTRL, 0x2u);  // assert reset
    for (volatile int i = 0; i < 1000; i++) {}
    pl_write32(PL_REG_CTRL, 0x0u);  // de-assert reset

    LOG_I("PL initialized and reset released");
    log_event(EVT_PL_READY, 0);
}

//=============================================================================
// 中断使能
//=============================================================================
void platform_init_interrupts(void)
{
    Xil_ExceptionEnable();
    LOG_D("Interrupts enabled");
}

void platform_enable_interrupts(void)
{
    Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);
}

void platform_disable_interrupts(void)
{
    Xil_ExceptionDisableMask(XIL_EXCEPTION_IRQ);
}

//=============================================================================
// 系统节拍
//=============================================================================
uint32_t platform_tick_ms(void)
{
    return g_tick_ms;
}

uint64_t platform_tick_us(void)
{
    // PMU Cycle Counter (64-bit @ CPU freq)
    uint64_t cycles;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cycles));

    // 转换为微秒
    return cycles / (XPAR_CPU_CORTEXA53_0_TIMESTAMP_CLK_FREQ / 1000000u);
}

//=============================================================================
// 延时
//=============================================================================
void platform_delay_ms(uint32_t ms)
{
    uint32_t start = platform_tick_ms();
    while ((platform_tick_ms() - start) < ms) {
        // spin
    }
}

void platform_delay_us(uint32_t us)
{
    uint64_t start = platform_tick_us();
    while ((platform_tick_us() - start) < us) {
        // spin
    }
}

//=============================================================================
// 软件复位
//=============================================================================
void platform_soft_reset(void)
{
    LOG_F("Soft reset triggered");

    // 关中断
    platform_disable_interrupts();

    // 清除 DMA
    pl_write32(PL_REG_CTRL, 0x0u);  // stop simulation

    // 系统复位 (写入 SLCR)
    // ZynqMP: CRL_APB RESET_CTRL 寄存器
    volatile uint32_t *crl_reset = (volatile uint32_t *)0xFF5E0238u;
    *crl_reset = 0x01000000u;

    while (1) {}
}

//=============================================================================
// 看门狗
//=============================================================================
void platform_wdt_start(uint32_t timeout_ms)
{
    (void)timeout_ms;
    // TODO: 使用 PS SWDT (System Watchdog Timer)
    // CSU SWDT: base 0xFFCB0000
}

void platform_wdt_kick(void)
{
    // TODO: 再触发 SWDT
}

#else  /* 无 BSP — 最小回退实现 */

//=============================================================================
// 无 BSP 环境下的占位实现
// 允许代码在不完整工具链下编译通过, 功能受限
//=============================================================================

static volatile uint32_t g_tick_ms_sim = 0;

void platform_init(void)
{
    LOG_W("Platform: no Xilinx BSP — running in simulation/fallback mode");
}

void platform_init_pl(void) {}

void platform_init_interrupts(void) {}
void platform_enable_interrupts(void) {}
void platform_disable_interrupts(void) {}

uint32_t platform_tick_ms(void)
{
    return g_tick_ms_sim++;
}

uint64_t platform_tick_us(void)
{
    return (uint64_t)g_tick_ms_sim * 1000u;
}

void platform_delay_ms(uint32_t ms)
{
    g_tick_ms_sim += ms;
}

void platform_delay_us(uint32_t us)
{
    g_tick_ms_sim += (us / 1000u) + 1u;
}

void platform_soft_reset(void)
{
    while (1) {}
}

void platform_wdt_start(uint32_t timeout_ms) { (void)timeout_ms; }
void platform_wdt_kick(void) {}

#endif /* XPAR_CPU_CORTEXA53_0_TIMESTAMP_CLK_FREQ */
