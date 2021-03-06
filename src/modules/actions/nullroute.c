/*
 * nullroute.c
 * Purpose: Set and remove nullroutes on remote routers.
 *
 * Copyright (c) 2009 - 2012, TortoiseLabs LLC.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>

#include "stdinc.h"
#include "action.h"

#include <libssh2.h>

#ifdef DEBUG
#define DEBUG_VTY_RESPONSE
#endif

/* defaults */
static char *router_ssh_user, *router_ssh_pass, *router_host, *router_enable_pass;
static int router_port = 22, nullroute_tag = 666;
static char *router_pubkey, *router_privkey;

typedef enum {
	PROTO_TELNET,
	PROTO_SSH,
	PROTO_MAX
} transport_proto_t;

static transport_proto_t router_proto = PROTO_SSH;

typedef enum {
	RTR_CISCO,
	RTR_VYATTA,
	RTR_MAX,
} router_conversation_type_t;

static router_conversation_type_t router_type = RTR_CISCO;

typedef struct target_ {
	struct target_ *next;

	transport_proto_t proto;
	router_conversation_type_t rtr_type;

	char *host;
	char *user;
	char *pass;
	char *enable_pass;
	char *pubkey;
	char *privkey;

	uint16_t port;
	uint32_t nullroute_tag;
} target_t;

typedef struct {
	int sock;
	LIBSSH2_SESSION *ssh_session;
	LIBSSH2_CHANNEL *ssh_channel;
} transport_ssh_t;

typedef struct {
	int sock;
} transport_telnet_t;

typedef struct {
	transport_proto_t method;
	union {
		transport_ssh_t ssh;
		transport_telnet_t telnet;
	} transport;
} transport_session_t;

typedef transport_session_t *(*transport_session_setup_f)(target_t *target);
typedef int (*transport_session_writef_f)(transport_session_t *session, const char *fmt, ...);
typedef void (*transport_session_term_f)(transport_session_t *session);

typedef struct {
	transport_session_setup_f setup;
	transport_session_writef_f writef;
	transport_session_term_f term;
} transport_handlers_t;

typedef void (*router_conversation_func_f)(actiontype_t act, transport_session_t *session, transport_handlers_t *handlers, target_t *target, const char *ipbuf);

static target_t *target_list = NULL;

static int
open_socket(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	struct in_addr ia;
	int sock;

	inet_pton(AF_INET, host, &ia);

	if (!(sock = socket(AF_INET, SOCK_STREAM, 0)))
		return -1;

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = ia.s_addr;

	if (connect(sock, (struct sockaddr *) &sa, sizeof(sa)) == -1)
	{
		close(sock);
		return -1;
	}

	return sock;
}

/************************************************************************************
 * ssh2 transport definition                                                        *
 ************************************************************************************/
static transport_session_t *
ssh_session_setup(target_t *target)
{
	int ret;
	char readbuf[1024] = "";
	const char *reason = "closing session";
	transport_session_t *session;

	session = calloc(sizeof(transport_session_t), 1);
	session->method = PROTO_SSH;

	session->transport.ssh.sock = open_socket(target->host, target->port);

	session->transport.ssh.ssh_session = libssh2_session_init();
	if (libssh2_session_startup(session->transport.ssh.ssh_session, session->transport.ssh.sock))
		goto shutdown;

	if (target->pubkey != NULL && target->privkey != NULL)
	{
		if (libssh2_userauth_publickey_fromfile(session->transport.ssh.ssh_session,
							target->user, target->pubkey,
							target->privkey, target->pass))
		{
			if (libssh2_userauth_password(session->transport.ssh.ssh_session,
						      target->user, target->pass))
				goto shutdown;
		}
	}
	else if (libssh2_userauth_password(session->transport.ssh.ssh_session,
					   target->user, target->pass))
		goto shutdown;

	if (!(session->transport.ssh.ssh_channel = libssh2_channel_open_session(session->transport.ssh.ssh_session)))
		goto shutdown;

	if (libssh2_channel_request_pty(session->transport.ssh.ssh_channel, "vanilla"))
		goto no_shell;

	if (libssh2_channel_shell(session->transport.ssh.ssh_channel))
		goto no_shell;

	return session;

no_shell:
	reason = "no shell available";
	libssh2_channel_free(session->transport.ssh.ssh_channel);

shutdown:
	libssh2_session_disconnect(session->transport.ssh.ssh_session, reason);
	libssh2_session_free(session->transport.ssh.ssh_session);
	close(session->transport.ssh.sock);

	free(session);

	return NULL;
}

static void
ssh_session_readline(LIBSSH2_CHANNEL *channel, char *buf, size_t buflen)
{
	int ret, offs = 0;

	memset(buf, 0, buflen);

	while (strchr(buf, '\n') == NULL)
	{
		ret = libssh2_channel_read(channel, buf + offs, buflen - offs);
		offs += ret;
	}
}

static int
ssh_session_writef(transport_session_t *session, const char *fmt, ...)
{
	char *line;
	size_t len;
	va_list va;
	int ret;
	LIBSSH2_CHANNEL *channel;
	char readbuf[1024] = "";

	channel = session->transport.ssh.ssh_channel;

	va_start(va, fmt);
	len = vasprintf(&line, fmt, va);
	va_end(va);

	ssh_session_readline(channel, readbuf, 1024);
	DPRINTF("< %s\n", readbuf);

	DPRINTF("> %s\n", line);
	ret = libssh2_channel_write(channel, line, len);

	ssh_session_readline(channel, readbuf, 1024);
	DPRINTF("< %s\n", readbuf);

	free(line);

	return len;
}

static void
ssh_session_term(transport_session_t *session)
{
	libssh2_channel_send_eof(session->transport.ssh.ssh_channel);

	libssh2_channel_free(session->transport.ssh.ssh_channel);

	libssh2_session_disconnect(session->transport.ssh.ssh_session, "session finished");
	libssh2_session_free(session->transport.ssh.ssh_session);
	close(session->transport.ssh.sock);

	free(session);
}

/************************************************************************************
 * telnet transport definition                                                      *
 ************************************************************************************/
static transport_session_t *
telnet_session_setup(target_t *target)
{
	transport_session_t *session;

	session = calloc(sizeof(transport_session_t), 1);
	session->method = PROTO_TELNET;

	session->transport.telnet.sock = open_socket(target->host, target->port);

	if (write(session->transport.telnet.sock, target->user, strlen(target->user)) < 0)
		return NULL;

	if (write(session->transport.telnet.sock, "\n", 1) < 0)
		return NULL;

	if (write(session->transport.telnet.sock, target->pass, strlen(target->pass)) < 0)
		return NULL;

	if (write(session->transport.telnet.sock, "\n", 1) < 0)
		return NULL;

	return session;
}

static int
telnet_session_writef(transport_session_t *session, const char *fmt, ...)
{
	char *line;
	size_t len;
	va_list va;
	int ret;

	va_start(va, fmt);
	len = vasprintf(&line, fmt, va);
	va_end(va);

	DPRINTF("writing [\n%s] = ", line);
	ret = write(session->transport.telnet.sock, line, len);
	DPRINTF("%d\n", ret);

	free(line);

	return len;
}

static void
telnet_session_term(transport_session_t *session)
{
	char buf[1];

	while (read(session->transport.telnet.sock, buf, 1))
	{
#ifdef DEBUG_VTY_RESPONSE
		putchar(buf[0]);
#endif
	}

	close(session->transport.telnet.sock);

	free(session);
}

static transport_handlers_t transport_handlers[PROTO_MAX] = {
	[PROTO_SSH] = {
		.setup = ssh_session_setup,
		.writef = ssh_session_writef,
		.term = ssh_session_term,
	},
	[PROTO_TELNET] = {
		.setup = telnet_session_setup,
		.writef = telnet_session_writef,
		.term = telnet_session_term,
	},
};

static void
rtr_cisco_converse(actiontype_t act, transport_session_t *session, transport_handlers_t *handlers, target_t *target, const char *ipbuf)
{
	if (target->enable_pass != NULL)
	{
		handlers->writef(session, "enable\n");
		handlers->writef(session, "%s\n", target->enable_pass);
	}

	handlers->writef(session, "conf t\n");

	if (target->nullroute_tag != 0)
		handlers->writef(session, "%sip route %s 255.255.255.255 Null0 tag %d\n", act == ACTION_UNBAN ? "no " : "", ipbuf, target->nullroute_tag);
	else
		handlers->writef(session, "%sip route %s 255.255.255.255 Null0\n", act == ACTION_UNBAN ? "no " : "", ipbuf);

	handlers->writef(session, "exit\n");
	handlers->writef(session, "exit\n");
}

static void
rtr_vyatta_converse(actiontype_t act, transport_session_t *session, transport_handlers_t *handlers, target_t *target, const char *ipbuf)
{
	handlers->writef(session, "configure\n");
	handlers->writef(session, "%s protocols static route %s/32 %s\n", act == ACTION_UNBAN ? "delete" : "set", ipbuf, act == ACTION_BAN ? "blackhole" : "");
	handlers->writef(session, "commit\n");
	handlers->writef(session, "save\n");
	handlers->writef(session, "exit\n");
	handlers->writef(session, "exit\n");
}

static router_conversation_func_f conversation_funcs[RTR_MAX] = {
	[RTR_CISCO]  = rtr_cisco_converse,
	[RTR_VYATTA] = rtr_vyatta_converse,
};

static void
trigger_nullroute(actiontype_t act, triggertype_t type, packet_info_t *packet, banrecord_t *rec, void *data)
{
	char ipbuf[INET6_ADDRSTRLEN];
	target_t *it;

	inet_ntop(AF_INET, &packet->pkt_dst, ipbuf, INET6_ADDRSTRLEN);

	for (it = target_list; it != NULL; it = it->next)
	{
		transport_session_t *session;
		transport_handlers_t *handlers = &transport_handlers[it->proto];

		DPRINTF("setting up session for target %s\n", it->host);

		session = handlers->setup(it);
		if (session == NULL)
		{
			DPRINTF("session setup for target %p failed\n", it);
			continue;
		}

		conversation_funcs[it->rtr_type](act, session, handlers, it, ipbuf);

		handlers->term(session);
	}
}

/************************************************************************************
 * configuration                                                                    *
 ************************************************************************************/
static transport_proto_t
parse_proto(const char *protoname)
{
	if (!strcasecmp(protoname, "telnet"))
		return PROTO_TELNET;

	return PROTO_SSH;
}

static router_conversation_type_t
parse_type(const char *typename)
{
	if (!strcasecmp(typename, "vyatta"))
		return RTR_VYATTA;

	return RTR_CISCO;
}

static void
parse_target(const char *host, mowgli_config_file_entry_t *entry)
{
	mowgli_config_file_entry_t *ce;
	target_t *target;

	target = calloc(sizeof(target_t), 1);
	target->next = target_list;

	target->host = strdup(host);
	target->user = router_ssh_user;
	target->pass = router_ssh_pass;
	target->pubkey = router_pubkey;
	target->privkey = router_privkey;
	target->enable_pass = router_enable_pass;
	target->port = router_port;
	target->proto = router_proto;
	target->rtr_type = router_type;
	target->nullroute_tag = nullroute_tag;

	MOWGLI_ITER_FOREACH(ce, entry)
	{
		if (!strcasecmp(ce->varname, "user"))
			target->user = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "pass"))
			target->pass = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "pubkey"))
			target->pubkey = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "privkey"))
			target->privkey = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "enable_password"))
			target->enable_pass = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "port"))
			target->port = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "nullroute_tag"))
			target->nullroute_tag = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "protocol"))
			target->proto = parse_proto(ce->vardata);
		else if (!strcasecmp(ce->varname, "type"))
			target->rtr_type = parse_type(ce->vardata);
	}

	target_list = target;
}

void
module_cons(mowgli_eventloop_t *eventloop, mowgli_config_file_entry_t *entry)
{
	mowgli_config_file_entry_t *ce;

	MOWGLI_ITER_FOREACH(ce, entry)
	{
		if (!strcasecmp(ce->varname, "user"))
			router_ssh_user = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "pass"))
			router_ssh_pass = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "pubkey"))
			router_pubkey = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "privkey"))
			router_privkey = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "enable_password"))
			router_enable_pass = strdup(ce->vardata);
		else if (!strcasecmp(ce->varname, "port"))
			router_port = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "nullroute_tag"))
			nullroute_tag = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "protocol"))
			router_proto = parse_proto(ce->vardata);
		else if (!strcasecmp(ce->varname, "type"))
			router_type = parse_type(ce->vardata);
		else if (!strcasecmp(ce->varname, "target"))
			parse_target(ce->vardata, ce->entries);
	}

	action_register("nullroute", trigger_nullroute, NULL);
}
