//=============================================================================
// tcp_server.c — lwIP RAW API TCP Server
//
// 单连接模型, 端口 5000。
// 接收: 逐字节喂帧解析器, 完整帧入 rx_queue
// 发送: 帧入 tx_queue, tcp_sent 回调驱动发送
//
// 条件编译:
//   - NO_XILINX_BSP: stub all lwIP calls (standalone syntax check)
//   - 否则: 完整 lwIP RAW API 实现
//=============================================================================

#include "tcp_server.h"
#include "protocol.h"
#include "logger.h"
#include "platform.h"
#ifndef NO_XILINX_BSP
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#endif
#include <string.h>
#include <stdlib.h>

//=============================================================================
// 全局上下文 (单实例)
//=============================================================================
static tcp_server_ctx_t *g_ctx = NULL;

//=============================================================================
// 每个连接的帧解析器
//=============================================================================
static frame_parser_t g_parser;

#ifdef NO_XILINX_BSP
/* =========================================================================
 * Standalone / No-BSP stub implementations
 * ========================================================================= */

void tcp_server_init(tcp_server_ctx_t *ctx)
{
    g_ctx = ctx;
    memset(ctx, 0, sizeof(*ctx));
    LOG_W("TCP server: stub (no lwIP)");
}

void tcp_server_process(tcp_server_ctx_t *ctx)
{
    (void)ctx;
    uint32_t now = platform_tick_ms();
    if (frame_parser_timed_out(&g_parser, now, FRAME_TIMEOUT_MS))
        frame_parser_init(&g_parser);
}

bool tcp_server_send(tcp_server_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx; (void)data; (void)len;
    return false;
}

bool tcp_server_connected(const tcp_server_ctx_t *ctx)
{
    (void)ctx;
    return false;
}

void tcp_server_disconnect(tcp_server_ctx_t *ctx)
{
    (void)ctx;
}

bool tcp_server_dequeue_rx(tcp_server_ctx_t *ctx, tcp_rx_frame_t *frame)
{
    if (ctx->rx_head == ctx->rx_tail) return false;
    *frame = ctx->rx_queue[ctx->rx_tail];
    ctx->rx_tail = (ctx->rx_tail + 1) % TCP_SEND_QUEUE_LEN;
    return true;
}

#else  /* ==================== Full lwIP implementation ==================== */

static bool rx_queue_push(tcp_server_ctx_t *ctx,
                          uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t next = (ctx->rx_head + 1) % TCP_SEND_QUEUE_LEN;
    if (next == ctx->rx_tail) return false;

    uint8_t *buf = (uint8_t *)malloc(len);
    if (buf == NULL) return false;
    memcpy(buf, data, len);

    ctx->rx_queue[ctx->rx_head].cmd  = cmd;
    ctx->rx_queue[ctx->rx_head].len  = len;
    ctx->rx_queue[ctx->rx_head].data = buf;
    ctx->rx_head = next;
    return true;
}

static void tx_do_send(tcp_server_ctx_t *ctx)
{
    if (ctx->tx_busy || ctx->client_pcb == NULL) return;
    if (ctx->tx_head == ctx->tx_tail) return;

    const uint8_t *data = ctx->tx_queue[ctx->tx_tail];
    uint16_t len = ctx->tx_queue_len[ctx->tx_tail];

    err_t err = tcp_write(ctx->client_pcb, data, len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        ctx->tx_busy = true;
        tcp_output(ctx->client_pcb);
    } else {
        LOG_W("tcp_write failed: err=%d", err);
        ctx->tx_tail = (ctx->tx_tail + 1) % TCP_SEND_QUEUE_LEN;
    }
}

/* ---- lwIP callbacks ---- */

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)err;
    tcp_server_ctx_t *ctx = (tcp_server_ctx_t *)arg;

    if (ctx->client_pcb != NULL) {
        LOG_W("TCP: rejected duplicate connection");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    ctx->client_pcb  = newpcb;
    ctx->conn_state  = CONN_ACCEPTED;
    ctx->conn_count++;

    tcp_recv(newpcb, tcp_recv_cb);
    tcp_err(newpcb,  tcp_err_cb);
    tcp_sent(newpcb, tcp_sent_cb);
    tcp_poll(newpcb, tcp_poll_cb, TCP_POLL_INTERVAL);
    tcp_arg(newpcb,  ctx);

    frame_parser_init(&g_parser);

    LOG_I("TCP client connected (total: %lu)",
          (unsigned long)ctx->conn_count);
    log_event(EVT_TCP_CONNECTED, ctx->conn_count);
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb,
                         struct pbuf *p, err_t err)
{
    (void)err;
    tcp_server_ctx_t *ctx = (tcp_server_ctx_t *)arg;

    if (p == NULL) {
        LOG_I("TCP: client disconnected (FIN)");
        log_event(EVT_TCP_DISCONNECTED, 0);
        ctx->client_pcb  = NULL;
        ctx->conn_state  = CONN_IDLE;
        tcp_close(tpcb);
        return ERR_OK;
    }

    struct pbuf *q = p;
    while (q != NULL) {
        for (uint16_t i = 0; i < q->len; i++) {
            uint8_t byte = ((uint8_t *)q->payload)[i];
            int result = frame_parse_byte(&g_parser, byte);

            if (result == 1) {
                if (rx_queue_push(ctx, g_parser.cmd,
                                  g_parser.payload, g_parser.len)) {
                    ctx->frames_rx++;
                } else {
                    LOG_W("TCP rx queue full, frame dropped");
                }
            } else if (result == -1) {
                LOG_D("Frame parse error, resync");
            }
        }
        q = q->next;
    }

    ctx->bytes_rx += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)tpcb;
    tcp_server_ctx_t *ctx = (tcp_server_ctx_t *)arg;

    ctx->bytes_tx += len;
    ctx->tx_busy   = false;
    ctx->tx_tail   = (ctx->tx_tail + 1) % TCP_SEND_QUEUE_LEN;

    if (ctx->tx_head != ctx->tx_tail)
        tx_do_send(ctx);

    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    tcp_server_ctx_t *ctx = (tcp_server_ctx_t *)arg;

    LOG_W("TCP error: %d", err);
    ctx->client_pcb  = NULL;
    ctx->conn_state  = CONN_IDLE;
    ctx->tx_busy     = false;
    log_event(EVT_TCP_DISCONNECTED, (uint32_t)err);
}

static err_t tcp_poll_cb(void *arg, struct tcp_pcb *tpcb)
{
    (void)tpcb;
    tcp_server_ctx_t *ctx = (tcp_server_ctx_t *)arg;

    if (!ctx->tx_busy && ctx->tx_head != ctx->tx_tail)
        tx_do_send(ctx);

    return ERR_OK;
}

/* ---- Public API ---- */

void tcp_server_init(tcp_server_ctx_t *ctx)
{
    g_ctx = ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn_state = CONN_IDLE;

    ctx->listen_pcb = tcp_new();
    if (ctx->listen_pcb == NULL) {
        LOG_F("Failed to create TCP PCB");
        return;
    }

    err_t err = tcp_bind(ctx->listen_pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err != ERR_OK) {
        LOG_F("TCP bind failed: %d", err);
        return;
    }

    ctx->listen_pcb = tcp_listen(ctx->listen_pcb);
    tcp_accept(ctx->listen_pcb, tcp_accept_cb);
    tcp_arg(ctx->listen_pcb, ctx);

    LOG_I("TCP server listening on port %d", TCP_SERVER_PORT);
}

void tcp_server_process(tcp_server_ctx_t *ctx)
{
    uint32_t now = platform_tick_ms();
    if (frame_parser_timed_out(&g_parser, now, FRAME_TIMEOUT_MS)) {
        frame_parser_init(&g_parser);
        LOG_D("Frame parser timeout, reset");
    }

    if (!ctx->tx_busy && ctx->tx_head != ctx->tx_tail)
        tx_do_send(ctx);
}

bool tcp_server_send(tcp_server_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    if (ctx->client_pcb == NULL) return false;

    uint8_t next = (ctx->tx_head + 1) % TCP_SEND_QUEUE_LEN;
    if (next == ctx->tx_tail) return false;

    ctx->tx_queue[ctx->tx_head]     = data;
    ctx->tx_queue_len[ctx->tx_head] = len;
    ctx->tx_head = next;

    if (!ctx->tx_busy) tx_do_send(ctx);
    return true;
}

bool tcp_server_connected(const tcp_server_ctx_t *ctx)
{
    return (ctx->client_pcb != NULL && ctx->conn_state == CONN_ACCEPTED);
}

void tcp_server_disconnect(tcp_server_ctx_t *ctx)
{
    if (ctx->client_pcb != NULL) {
        tcp_close(ctx->client_pcb);
        ctx->client_pcb  = NULL;
        ctx->conn_state  = CONN_IDLE;
        ctx->tx_busy     = false;
        LOG_I("TCP: forced disconnect");
    }
}

bool tcp_server_dequeue_rx(tcp_server_ctx_t *ctx, tcp_rx_frame_t *frame)
{
    if (ctx->rx_head == ctx->rx_tail) return false;
    *frame = ctx->rx_queue[ctx->rx_tail];
    ctx->rx_tail = (ctx->rx_tail + 1) % TCP_SEND_QUEUE_LEN;
    return true;
}

#endif /* NO_XILINX_BSP */
