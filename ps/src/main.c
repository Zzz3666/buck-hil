//=============================================================================
// main.c — Buck HIL PS 固件入口
//
// Baremetal + lwIP, 单线程主循环模式。
//
// 主循环周期: 无限循环, 每轮:
//   1. lwIP 定时器 (tcp_tmr, 250ms)
//   2. TCP 服务器处理 (接收帧出队, 命令分发, 应答入队)
//   3. DMA 处理 (BD 回收, 数据拷贝)
//   4. 触发检查 (PL trigger → 打包 CMD_TRIG_DATA 发送)
//   5. 心跳 (1Hz CMD_ASYNC_EVENT)
//   6. 看门狗踢狗
//=============================================================================

#include "platform.h"
#include "protocol.h"
#include "params.h"
#include "tcp_server.h"
#include "dma_ctrl.h"
#include "logger.h"

#ifndef NO_XILINX_BSP
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/xadapter.h"
#endif

#include <string.h>
#include <stdlib.h>

//=============================================================================
// 常量
//=============================================================================
#define HEARTBEAT_INTERVAL_MS   1000
#define TCP_TMR_INTERVAL_MS     250
#define WDT_TIMEOUT_MS          2000

//=============================================================================
// 全局
//=============================================================================
static tcp_server_ctx_t g_tcp;
static frame_out_t      g_frame_out;
static bool             g_sim_running = false;

// 软件看门狗
typedef struct {
    uint32_t last_proto_activity;
    uint32_t last_dma_activity;
    uint32_t last_heartbeat;
    uint32_t error_count;
} sw_wdt_t;

static sw_wdt_t g_sw_wdt;

//=============================================================================
// 网络初始化 (lwIP + Xilinx EMAC)
//=============================================================================
#ifndef NO_XILINX_BSP
static struct netif g_netif;

static void network_init(void)
{
    // 1. lwIP 内核初始化
    lwip_init();

    // 2. 添加网络接口 (Xilinx ZynqMP GEM)
    struct ip4_addr ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr,  192, 168, 1, 100);  // 默认静态 IP
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    if (!xemacps_add(&g_netif, &ipaddr, &netmask, &gw,
                     NULL, 0, 0)) {
        LOG_F("Network: failed to add EMAC interface");
        return;
    }

    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    LOG_I("Network initialized: IP=" IP4STR, IP42STR(ipaddr));
}
#else
static void network_init(void)
{
    LOG_W("Network: lwIP not available (standalone build)");
}
#endif

//=============================================================================
// 心跳
//=============================================================================
static void heartbeat_send(uint32_t now)
{
    if (now - g_sw_wdt.last_heartbeat < HEARTBEAT_INTERVAL_MS)
        return;

    g_sw_wdt.last_heartbeat = now;

    if (!tcp_server_connected(&g_tcp))
        return;

    // 构建心跳帧: CMD_ASYNC_EVENT, payload=0x0D (HEARTBEAT)
    uint8_t payload[2];
    payload[0] = 0x0D;  // EVT_HEARTBEAT
    payload[1] = (uint8_t)(g_sim_running ? 1 : 0);
    frame_build(&g_frame_out, CMD_ASYNC_EVENT, payload, 2);

    tcp_server_send(&g_tcp, g_frame_out.buf, g_frame_out.len);
    log_event(EVT_HEARTBEAT, g_sim_running ? 1u : 0u);
}

//=============================================================================
// 触发检查: 轮询 PL_REG_TRIG_STATUS
//=============================================================================
static void trigger_check(void)
{
    uint32_t trig_status = pl_read32(PL_REG_TRIG_STATUS);
    if (!(trig_status & 0x1u))
        return;  // 未触发

    // 清除触发标志
    pl_write32(PL_REG_TRIG_STATUS, 0x1u);

    LOG_I("Trigger occurred!");

    if (!tcp_server_connected(&g_tcp))
        return;

    // 读取 DMA 缓冲区中的触发数据
    // capture_manager 在触发后将 4096 采样写入 DMA 缓冲区
    // 此处简化: 发送一个触发通知帧
    uint32_t bytes_avail;
    const uint8_t *buf = dma_get_last_buffer(&bytes_avail);

    if (buf != NULL && bytes_avail >= 8) {
        // 构建 CMD_TRIG_DATA 帧
        // 实际实现中应循环发送完整 4096 点
        // 此处发送一个简化数据块作为示例
        uint8_t payload[128];
        uint16_t sample_count = (bytes_avail < 120) ? (uint16_t)(bytes_avail / 8) : 15u;
        // 每 8B 一个采样: VOUT(2B) + IL(2B) + DUTY(2B) + TS(2B)

        payload[0] = 0x00;  // seq# 低字节 (简化)
        payload[1] = 0x00;
        payload[2] = 0x00;
        payload[3] = 0x01;  // seq# = 1

        payload[4] = (uint8_t)(sample_count >> 8);  // COUNT 高
        payload[5] = (uint8_t)(sample_count & 0xFF); // COUNT 低

        payload[6] = 0x03;  // CH_MASK: Vout + IL valid

        // 拷贝采样数据 (大端)
        for (uint16_t i = 0; i < sample_count && (7 + i * 8 + 8) <= 127; i++) {
            memcpy(&payload[7 + i * 8], &buf[i * 8], 8);
        }

        frame_build(&g_frame_out, CMD_TRIG_DATA,
                    payload, 7 + sample_count * 8);
        tcp_server_send(&g_tcp, g_frame_out.buf, g_frame_out.len);
    }

    log_event(EVT_TRIG_OCCURRED, 0);
}

//=============================================================================
// 命令处理 (主循环中消费接收队列)
//=============================================================================
static void process_rx_commands(tcp_server_ctx_t *tcp)
{
    tcp_rx_frame_t rx_frame;

    while (tcp_server_dequeue_rx(tcp, &rx_frame)) {
        g_sw_wdt.last_proto_activity = platform_tick_ms();

        // 构建命令上下文并分发
        cmd_context_t ctx;
        ctx.cmd         = rx_frame.cmd;
        ctx.payload_len = rx_frame.len;
        ctx.payload     = rx_frame.data;

        bool need_send = cmd_dispatch(&ctx, &g_frame_out);
        if (need_send) {
            tcp_server_send(tcp, g_frame_out.buf, g_frame_out.len);
        }

        free(rx_frame.data);  // 消费完毕释放
    }
}

//=============================================================================
// 软件看门狗检查
//=============================================================================
static void sw_wdt_check(uint32_t now)
{
    // 协议层无活动 > 30s
    if (now - g_sw_wdt.last_proto_activity > 30000u) {
        LOG_W("Protocol activity timeout");
    }

    // DMA 无活动 > 5s 且仿真运行
    if (g_sim_running && (now - g_sw_wdt.last_dma_activity > 5000u)) {
        LOG_W("DMA activity timeout, resetting");
        dma_reset();
        g_sw_wdt.last_dma_activity = now;
        g_sw_wdt.error_count++;
    }
}

//=============================================================================
// 主函数
//=============================================================================
int main(void)
{
    // ---- 1. 平台初始化 ----
    platform_init();

    // ---- 2. 日志 ----
    logger_init();
    LOG_I("============================================");
    LOG_I("Buck HIL PS Firmware v1.0.0");
    LOG_I("ZU3EG Cortex-A53 — Baremetal + lwIP");
    LOG_I("============================================");

    // ---- 3. PL 初始化 ----
    platform_init_pl();

    // ---- 4. 参数初始化 ----
    params_init();

    // ---- 5. 网络 ----
    network_init();

    // ---- 6. TCP 服务器 ----
    tcp_server_init(&g_tcp);

    // ---- 7. DMA ----
    dma_init(AXI_DMA_BASE, DMA_S2MM_INTR_ID);

    // ---- 8. 中断使能 ----
    platform_init_interrupts();
    platform_enable_interrupts();

    // ---- 9. 看门狗 ----
    platform_wdt_start(WDT_TIMEOUT_MS);

    LOG_I("All subsystems initialized. Entering main loop.");

    // ---- 10. 主循环 ----
    uint32_t last_tcp_tmr = platform_tick_ms();
#ifdef NO_XILINX_BSP
    (void)last_tcp_tmr;  // only used with lwIP
#endif

    while (1) {
        uint32_t now = platform_tick_ms();

        // --- lwIP 定时器 (每 250ms) ---
#ifndef NO_XILINX_BSP
        if (now - last_tcp_tmr >= TCP_TMR_INTERVAL_MS) {
            tcp_tmr();
            last_tcp_tmr = now;
        }
#endif

        // --- TCP 处理 ---
        tcp_server_process(&g_tcp);

        // --- 命令处理 ---
        process_rx_commands(&g_tcp);

        // --- DMA 处理 ---
        dma_process();

        // --- 触发检查 ---
        trigger_check();

        // --- 心跳 ---
        heartbeat_send(now);

        // --- 软件看门狗 ---
        sw_wdt_check(now);

        // --- 硬件看门狗 ---
        platform_wdt_kick();
    }

    return 0;
}
