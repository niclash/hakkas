/*
 * POSIX PAL sockets (01 §0.2): plain BSD sockets, every one non-blocking and
 * close-on-exec. pal_poll is in wakeup.c, next to the wakeup it also waits on.
 *
 * Decisions (plan task 12): numeric IPv4/IPv6 addresses only (no name
 * resolution); SO_REUSEADDR on listeners; IPV6_V6ONLY off, so "::" serves
 * IPv4 too; TCP_NODELAY on accepted connections; MSG_NOSIGNAL on every send
 * instead of ignoring SIGPIPE process-wide; every call is retried on EINTR.
 */
#include "platform/pal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static enum pal_err from_errno(int err)
{
	switch (err) {
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
	case ECONNABORTED:
	case EPROTO:
		return PAL_ERR_WOULD_BLOCK;
	case EPIPE:
	case ECONNRESET:
	case ENOTCONN:
	case ETIMEDOUT:
	case EHOSTUNREACH:
	case ENETUNREACH:
	case ENETDOWN:
		return PAL_ERR_CLOSED;
	case EADDRINUSE:
		return PAL_ERR_ADDR_IN_USE;
	case EACCES:
	case EPERM:
		return PAL_ERR_DENIED;
	case EMFILE:
	case ENFILE:
	case ENOBUFS:
	case ENOMEM:
	case EAFNOSUPPORT:
	case EADDRNOTAVAIL:
		return PAL_ERR_UNAVAILABLE;
	case EINVAL:
	case EBADF:
	case ENOTSOCK:
		return PAL_ERR_INVALID;
	default:
		return PAL_ERR_IO;
	}
}

/* Parses a numeric address into *addr (IPv4 first, then IPv6). */
static bool parse_addr(const char *host, uint16_t port, struct sockaddr_storage *addr,
		       socklen_t *len)
{
	memset(addr, 0, sizeof(*addr));
	struct sockaddr_in *in4 = (struct sockaddr_in *)addr;
	if (inet_pton(AF_INET, host, &in4->sin_addr) == 1) {
		in4->sin_family = AF_INET;
		in4->sin_port = htons(port);
		*len = sizeof(*in4);
		return true;
	}
	struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)addr;
	if (inet_pton(AF_INET6, host, &in6->sin6_addr) == 1) {
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(port);
		*len = sizeof(*in6);
		return true;
	}
	return false;
}

enum pal_err pal_socket_listen(const char *host, uint16_t port, int backlog, pal_socket *out)
{
	*out = PAL_SOCKET_NONE;
	struct sockaddr_storage addr;
	socklen_t len;
	if (host == NULL || !parse_addr(host, port, &addr, &len) || backlog < 1)
		return PAL_ERR_INVALID;
	int fd = socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return from_errno(errno);
	int one = 1, zero = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
	    (addr.ss_family == AF_INET6 &&
	     setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) != 0) ||
	    bind(fd, (struct sockaddr *)&addr, len) != 0 || listen(fd, backlog) != 0) {
		enum pal_err err = from_errno(errno);
		close(fd);
		return err;
	}
	*out = fd;
	return PAL_OK;
}

enum pal_err pal_socket_local_port(pal_socket sock, uint16_t *port)
{
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	if (getsockname(sock, (struct sockaddr *)&addr, &len) != 0)
		return from_errno(errno);
	if (addr.ss_family == AF_INET)
		*port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
	else if (addr.ss_family == AF_INET6)
		*port = ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
	else
		return PAL_ERR_INVALID;
	return PAL_OK;
}

/* The text form of a peer address; IPv4-mapped IPv6 as plain IPv4. */
static void peer_text(const struct sockaddr_storage *addr, struct pal_peer *peer)
{
	memset(peer, 0, sizeof(*peer));
	if (addr->ss_family == AF_INET) {
		const struct sockaddr_in *in4 = (const struct sockaddr_in *)addr;
		inet_ntop(AF_INET, &in4->sin_addr, peer->addr, sizeof(peer->addr));
		peer->port = ntohs(in4->sin_port);
	} else if (addr->ss_family == AF_INET6) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
		if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr))
			inet_ntop(AF_INET, &in6->sin6_addr.s6_addr[12], peer->addr,
				  sizeof(peer->addr));
		else
			inet_ntop(AF_INET6, &in6->sin6_addr, peer->addr, sizeof(peer->addr));
		peer->port = ntohs(in6->sin6_port);
	}
}

enum pal_err pal_socket_accept(pal_socket listener, pal_socket *out, struct pal_peer *peer)
{
	*out = PAL_SOCKET_NONE;
	struct sockaddr_storage addr;
	socklen_t len;
	int fd;
	do {
		len = sizeof(addr);
		fd = accept4(listener, (struct sockaddr *)&addr, &len,
			     SOCK_NONBLOCK | SOCK_CLOEXEC);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0)
		return from_errno(errno);
	int one = 1;
	/* Best effort: only latency depends on it. */
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if (peer != NULL)
		peer_text(&addr, peer);
	*out = fd;
	return PAL_OK;
}

enum pal_err pal_socket_read(pal_socket sock, void *buf, size_t cap, size_t *len)
{
	*len = 0;
	if (buf == NULL || cap == 0)
		return PAL_ERR_INVALID;
	ssize_t n;
	do {
		n = recv(sock, buf, cap, 0);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return from_errno(errno);
	if (n == 0)
		return PAL_ERR_CLOSED;
	*len = (size_t)n;
	return PAL_OK;
}

enum pal_err pal_socket_write(pal_socket sock, const void *data, size_t len, size_t *written)
{
	*written = 0;
	if (data == NULL || len == 0)
		return PAL_ERR_INVALID;
	ssize_t n;
	do {
		n = send(sock, data, len, MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return from_errno(errno);
	if (n == 0)
		return PAL_ERR_WOULD_BLOCK;
	*written = (size_t)n;
	return PAL_OK;
}

void pal_socket_close(pal_socket sock)
{
	/* No retry on EINTR: on Linux the descriptor is released anyway, and a
	 * retry could close one reused by now. */
	if (sock >= 0)
		(void)close(sock);
}
