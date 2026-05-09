//=============================================================================
// logger.h — 双通道日志: UART 主动输出 + DDR 环形缓冲区
//
// UART: 实时调试输出 (115200-8N1)
// Ring buffer: 保留最近 N 条日志，可通过协议命令读取
//=============================================================================

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// 日志级别
//=============================================================================
typedef enum {
    LOG_FATAL = 0,
    LOG_ERROR,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE,
} log_level_t;

//=============================================================================
// 事件类型 (用于结构化日志)
//=============================================================================
typedef enum {
    EVT_BOOT            = 0x01,
    EVT_PL_READY        = 0x02,
    EVT_TCP_CONNECTED   = 0x03,
    EVT_TCP_DISCONNECTED= 0x04,
    EVT_SIM_STARTED     = 0x05,
    EVT_SIM_STOPPED     = 0x06,
    EVT_TRIG_OCCURRED   = 0x07,
    EVT_PWM_LOST        = 0x08,
    EVT_PWM_RECOVERED   = 0x09,
    EVT_PROTOCOL_ERROR  = 0x0A,
    EVT_DMA_ERROR       = 0x0B,
    EVT_WDT_TIMEOUT     = 0x0C,
    EVT_HEARTBEAT       = 0x0D,
    EVT_SELF_TEST       = 0x0E,
} log_event_t;

//=============================================================================
// API
//=============================================================================
void logger_init(void);
void logger_set_level(log_level_t level);

// 格式化日志 (printf 风格)
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

// 结构化事件日志
void log_event(log_event_t event, uint32_t data);

// 获取环形缓冲区中日志条目数
uint32_t logger_count(void);

// 读取环形缓冲区中第 index 条日志 (0 = 最旧)
// 返回实际写入的字节数
int logger_read(uint32_t index, char *buf, uint16_t buf_size);

#define LOG_F(fmt, ...) log_write(LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) log_write(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) log_write(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) log_write(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_D(fmt, ...) log_write(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // LOGGER_H
