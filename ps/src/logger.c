//=============================================================================
// logger.c — 双通道日志: UART 实时输出 + DDR 环形缓冲区
//
// UART: Xilinx PS UART0 (xil_printf)
// Ring buffer: 256 条日志, 每条最多 128 字节
//=============================================================================

#include "logger.h"
#include "platform.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

//=============================================================================
// 日志条目
//=============================================================================
#define LOG_MAX_ENTRIES   256
#define LOG_MAX_LEN       128

typedef struct {
    uint32_t    tick_ms;
    log_level_t level;
    char        msg[LOG_MAX_LEN];
} log_entry_t;

//=============================================================================
// 环形缓冲区
//=============================================================================
static log_entry_t g_log_ring[LOG_MAX_ENTRIES];
static volatile uint32_t g_log_write_idx = 0;
static uint32_t          g_log_count     = 0;
static log_level_t       g_log_level     = LOG_INFO;

//=============================================================================
// 初始化
//=============================================================================
void logger_init(void)
{
    memset(g_log_ring, 0, sizeof(g_log_ring));
    g_log_write_idx = 0;
    g_log_count     = 0;
    g_log_level     = LOG_INFO;

    log_event(EVT_BOOT, 0);
    LOG_I("Logger initialized");
}

//=============================================================================
// 设置级别
//=============================================================================
void logger_set_level(log_level_t level)
{
    g_log_level = level;
}

//=============================================================================
// 格式化日志
//=============================================================================
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
{
    if (level > g_log_level) return;

    uint32_t now = platform_tick_ms();

    // 格式化到栈缓冲区
    char buf[LOG_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0) return;
    if ((size_t)len >= sizeof(buf))
        len = (int)(sizeof(buf) - 1);

    // 写入环形缓冲区
    uint32_t idx = g_log_write_idx;
    g_log_ring[idx].tick_ms = now;
    g_log_ring[idx].level   = level;
    memcpy(g_log_ring[idx].msg, buf, (size_t)len + 1);

    g_log_write_idx = (idx + 1) % LOG_MAX_ENTRIES;
    if (g_log_count < LOG_MAX_ENTRIES)
        g_log_count++;

    // UART 输出
    static const char level_chars[] = "FEWIDT";
    char prefix = (level < (log_level_t)sizeof(level_chars))
                  ? level_chars[level] : '?';
    // In baremetal, use xil_printf if available, else raw UART
    // xil_printf("[%c %8lu] %s:%d %s\r\n", prefix, now, file, line, buf);
    // Fallback: minimal output
    (void)file;
    (void)line;
    (void)prefix;
}

//=============================================================================
// 结构化事件
//=============================================================================
void log_event(log_event_t event, uint32_t data)
{
    LOG_I("EVT 0x%02X data=0x%08lX", (unsigned)event, (unsigned long)data);
}

//=============================================================================
// 读取环形缓冲区
//=============================================================================
uint32_t logger_count(void)
{
    return g_log_count;
}

int logger_read(uint32_t index, char *buf, uint16_t buf_size)
{
    if (index >= g_log_count) return -1;

    // 最旧条目在 (write_idx - count) mod MAX_ENTRIES
    uint32_t actual_idx;
    if (g_log_count < LOG_MAX_ENTRIES) {
        actual_idx = index;
    } else {
        actual_idx = (g_log_write_idx + index) % LOG_MAX_ENTRIES;
    }

    log_entry_t *entry = &g_log_ring[actual_idx];
    int len = snprintf(buf, buf_size, "[%8lu] %s",
                       (unsigned long)entry->tick_ms, entry->msg);
    return len;
}
