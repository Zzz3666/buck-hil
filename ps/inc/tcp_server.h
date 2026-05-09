//=============================================================================
// tcp_server.h — lwIP RAW API TCP Server
//
// 单连接模型: 同时只允许一个 PC 客户端连接。
// 端口 5000, 接收回调逐字节喂帧解析器。
// 发送队列: 非阻塞, 通过 tcp_sent 回调确认后继续发送。
//=============================================================================

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#ifndef NO_XILINX_BSP
#include "lwip/tcp.h"
#else
/* Standalone build: forward-declare lwIP types */
struct tcp_pcb;
struct pbuf;
typedef unsigned short u16_t;
typedef signed char err_t;
#define ERR_OK  0
#define ERR_ABRT (-3)
#define TCP_WRITE_FLAG_COPY 0x01
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SERVER_PORT    5000
#define TCP_SEND_QUEUE_LEN 16        // 发送帧队列深度
#define TCP_POLL_INTERVAL  4          // lwIP poll interval (×500ms = 2s)

//=============================================================================
// 连接状态
//=============================================================================
typedef enum {
    CONN_IDLE,        // 无连接
    CONN_ACCEPTED,    // 已接受，等待接收
    CONN_CLOSING,     // 正在关闭
} tcp_conn_state_t;

//=============================================================================
// 接收帧条目 (cmd + payload)
//=============================================================================
typedef struct {
    uint8_t   cmd;
    uint16_t  len;
    uint8_t  *data;     // heap-allocated, caller frees after use
} tcp_rx_frame_t;

//=============================================================================
// TCP 服务器上下文
//=============================================================================
typedef struct tcp_server_ctx {
    struct tcp_pcb   *listen_pcb;
    struct tcp_pcb   *client_pcb;
    tcp_conn_state_t  conn_state;

    // 接收帧队列 (装满的帧供主循环处理)
    tcp_rx_frame_t rx_queue[TCP_SEND_QUEUE_LEN];
    volatile uint8_t rx_head;
    volatile uint8_t rx_tail;

    // 发送帧队列
    const uint8_t *tx_queue[TCP_SEND_QUEUE_LEN];
    uint16_t       tx_queue_len[TCP_SEND_QUEUE_LEN];
    volatile uint8_t tx_head;
    volatile uint8_t tx_tail;
    volatile bool   tx_busy;    // true = 正在发送

    // 统计
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t conn_count;
} tcp_server_ctx_t;

//=============================================================================
// API
//=============================================================================
void tcp_server_init(tcp_server_ctx_t *ctx);
void tcp_server_process(tcp_server_ctx_t *ctx);   // 主循环调用: 处理接收帧

// 入队发送帧 (非阻塞, 可从中断/回调上下文调用)
// 返回 true=入队成功, false=队列满
bool tcp_server_send(tcp_server_ctx_t *ctx, const uint8_t *data, uint16_t len);

// 是否已连接
bool tcp_server_connected(const tcp_server_ctx_t *ctx);

// 关闭连接
void tcp_server_disconnect(tcp_server_ctx_t *ctx);

// 获取接收队列中的下一帧
// 返回 true=取到, frame 被填充 (caller must free frame->data)
bool tcp_server_dequeue_rx(tcp_server_ctx_t *ctx,
                           tcp_rx_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif // TCP_SERVER_H
