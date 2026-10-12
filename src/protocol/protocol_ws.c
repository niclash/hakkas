/*
 * The protocol dispatcher on the WebSocket server (protocol/dispatcher.h,
 * engine/ws_server.h): the WsHandler whose ctx is the Protocol, and the
 * ProtoTransport over a WsConn. A session is the connection's user context
 * (ws_set_user).
 */
#include "protocol/dispatcher.h"

static bool t_send_text(void *conn, const char *text, size_t len)
{
	return ws_send_text(conn, text, len);
}

static size_t t_send_space(void *conn)
{
	return ws_send_space(conn);
}

static size_t t_send_capacity(void *conn)
{
	return ws_send_capacity(conn);
}

static size_t t_send_pending(void *conn)
{
	return ws_send_pending(conn);
}

static void t_close(void *conn, uint16_t code, const char *reason)
{
	ws_close(conn, code, reason);
}

static const ProtoTransport ws_transport = {t_send_text, t_send_space, t_send_capacity,
						t_send_pending, t_close};

static void h_open(void *ctx, WsConn *conn)
{
	ProtoConn *c = protocol_conn_open(ctx, &ws_transport, conn, ws_peer(conn)->addr);
	if (c == NULL) {
		ws_close(conn, WS_CLOSE_INTERNAL_ERROR, "out of memory");
		return;
	}
	ws_set_user(conn, c);
}

static void h_message(void *ctx, WsConn *conn, const char *text, size_t len)
{
	(void)ctx;
	protocol_conn_message(ws_user(conn), text, len);
}

static void h_close(void *ctx, WsConn *conn)
{
	(void)ctx;
	protocol_conn_close(ws_user(conn));
	ws_set_user(conn, NULL);
}

static void h_drained(void *ctx, WsConn *conn)
{
	(void)ctx;
	protocol_conn_drained(ws_user(conn));
}

static const WsHandler handler = {h_open, h_message, h_close, h_drained};

const WsHandler *protocol_ws_handler(void)
{
	return &handler;
}
