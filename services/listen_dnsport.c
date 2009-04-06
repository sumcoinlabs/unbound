/*
 * services/listen_dnsport.c - listen on port 53 for incoming DNS queries.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file has functions to get queries from clients.
 */
#include "config.h"
#include "services/listen_dnsport.h"
#include "services/outside_network.h"
#include "util/netevent.h"
#include "util/log.h"
#include "util/config_file.h"
#include "util/net_help.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <fcntl.h>

/** number of queued TCP connections for listen() */
#define TCP_BACKLOG 5 

/**
 * Debug print of the getaddrinfo returned address.
 * @param addr: the address returned.
 */
static void
verbose_print_addr(struct addrinfo *addr)
{
	if(verbosity >= VERB_ALGO) {
		char buf[100];
		void* sinaddr = &((struct sockaddr_in*)addr->ai_addr)->sin_addr;
#ifdef INET6
		if(addr->ai_family == AF_INET6)
			sinaddr = &((struct sockaddr_in6*)addr->ai_addr)->
				sin6_addr;
#endif /* INET6 */
		if(inet_ntop(addr->ai_family, sinaddr, buf,
			(socklen_t)sizeof(buf)) == 0) {
			strncpy(buf, "(null)", sizeof(buf));
		}
		buf[sizeof(buf)-1] = 0;
		verbose(VERB_ALGO, "creating %s%s socket %s %d", 
			addr->ai_socktype==SOCK_DGRAM?"udp":
			addr->ai_socktype==SOCK_STREAM?"tcp":"otherproto",
			addr->ai_family==AF_INET?"4":
			addr->ai_family==AF_INET6?"6":
			"_otherfam", buf, 
			ntohs(((struct sockaddr_in*)addr->ai_addr)->sin_port));
	}
}

int
create_udp_sock(int family, int socktype, struct sockaddr* addr,
        socklen_t addrlen, int v6only, int* inuse, int* noproto)
{
	int s;
# if defined(IPV6_USE_MIN_MTU)
	int on=1;
# else
	(void)v6only;
# endif
	if((s = socket(family, socktype, 0)) == -1) {
		*inuse = 0;
#ifndef USE_WINSOCK
		if(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) {
			*noproto = 1;
			return -1;
		}
		log_err("can't create socket: %s", strerror(errno));
#else
		if(WSAGetLastError() == WSAEAFNOSUPPORT || 
			WSAGetLastError() == WSAEPROTONOSUPPORT) {
			*noproto = 1;
			return -1;
		}
		log_err("can't create socket: %s", 
			wsa_strerror(WSAGetLastError()));
#endif
		*noproto = 0;
		return -1;
	}
	if(family == AF_INET6) {
# if defined(IPV6_V6ONLY)
		if(v6only) {
			int val=(v6only==2)?0:1;
			if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, 
				(void*)&val, (socklen_t)sizeof(val)) < 0) {
#ifndef USE_WINSOCK
				log_err("setsockopt(..., IPV6_V6ONLY"
					", ...) failed: %s", strerror(errno));
#else
				log_err("setsockopt(..., IPV6_V6ONLY"
					", ...) failed: %s", 
					wsa_strerror(WSAGetLastError()));
#endif
				close(s);
				*noproto = 0;
				*inuse = 0;
				return -1;
			}
		}
# endif
# if defined(IPV6_USE_MIN_MTU)
		/*
		 * There is no fragmentation of IPv6 datagrams
		 * during forwarding in the network. Therefore
		 * we do not send UDP datagrams larger than
		 * the minimum IPv6 MTU of 1280 octets. The
		 * EDNS0 message length can be larger if the
		 * network stack supports IPV6_USE_MIN_MTU.
		 */
		if (setsockopt(s, IPPROTO_IPV6, IPV6_USE_MIN_MTU,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
#ifndef USE_WINSOCK
			log_err("setsockopt(..., IPV6_USE_MIN_MTU, "
				"...) failed: %s", strerror(errno));
#else
			log_err("setsockopt(..., IPV6_USE_MIN_MTU, "
				"...) failed: %s", 
				wsa_strerror(WSAGetLastError()));
#endif
			close(s);
			*noproto = 0;
			*inuse = 0;
			return -1;
		}
# endif
	}
	if(bind(s, (struct sockaddr*)addr, addrlen) != 0) {
		*noproto = 0;
#ifndef USE_WINSOCK
#ifdef EADDRINUSE
		*inuse = (errno == EADDRINUSE);
		/* detect freebsd jail with no ipv6 permission */
		if(family==AF_INET6 && errno==EINVAL)
			*noproto = 1;
		else if(errno != EADDRINUSE)
			log_err("can't bind socket: %s", strerror(errno));
#endif
#else /* USE_WINSOCK */
		if(WSAGetLastError() != WSAEADDRINUSE &&
			WSAGetLastError() != WSAEADDRNOTAVAIL)
			log_err("can't bind socket: %s", 
				wsa_strerror(WSAGetLastError()));
#endif
		close(s);
		return -1;
	}
	if(!fd_set_nonblock(s)) {
		*noproto = 0;
		*inuse = 0;
		close(s);
		return -1;
	}
	return s;
}

int
create_tcp_accept_sock(struct addrinfo *addr, int v6only, int* noproto)
{
	int s;
#if defined(SO_REUSEADDR) || defined(IPV6_V6ONLY)
	int on = 1;
#endif /* SO_REUSEADDR || IPV6_V6ONLY */
	verbose_print_addr(addr);
	*noproto = 0;
	if((s = socket(addr->ai_family, addr->ai_socktype, 0)) == -1) {
#ifndef USE_WINSOCK
		if(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) {
			*noproto = 1;
			return -1;
		}
		log_err("can't create socket: %s", strerror(errno));
#else
		if(WSAGetLastError() == WSAEAFNOSUPPORT ||
			WSAGetLastError() == WSAEPROTONOSUPPORT) {
			*noproto = 1;
			return -1;
		}
		log_err("can't create socket: %s", 
			wsa_strerror(WSAGetLastError()));
#endif
		return -1;
	}
#ifdef SO_REUSEADDR
	if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&on, 
		(socklen_t)sizeof(on)) < 0) {
#ifndef USE_WINSOCK
		log_err("setsockopt(.. SO_REUSEADDR ..) failed: %s",
			strerror(errno));
#else
		log_err("setsockopt(.. SO_REUSEADDR ..) failed: %s",
			wsa_strerror(WSAGetLastError()));
#endif
		return -1;
	}
#endif /* SO_REUSEADDR */
#if defined(IPV6_V6ONLY)
	if(addr->ai_family == AF_INET6 && v6only) {
		if(setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, 
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
#ifndef USE_WINSOCK
			log_err("setsockopt(..., IPV6_V6ONLY, ...) failed: %s",
				strerror(errno));
#else
			log_err("setsockopt(..., IPV6_V6ONLY, ...) failed: %s",
				wsa_strerror(WSAGetLastError()));
#endif
			return -1;
		}
	}
#else
	(void)v6only;
#endif /* IPV6_V6ONLY */
	if(bind(s, addr->ai_addr, addr->ai_addrlen) != 0) {
#ifndef USE_WINSOCK
		/* detect freebsd jail with no ipv6 permission */
		if(addr->ai_family==AF_INET6 && errno==EINVAL)
			*noproto = 1;
		else log_err("can't bind socket: %s", strerror(errno));
#else
		log_err("can't bind socket: %s", 
			wsa_strerror(WSAGetLastError()));
#endif
		return -1;
	}
	if(!fd_set_nonblock(s)) {
		return -1;
	}
	if(listen(s, TCP_BACKLOG) == -1) {
#ifndef USE_WINSOCK
		log_err("can't listen: %s", strerror(errno));
#else
		log_err("can't listen: %s", wsa_strerror(WSAGetLastError()));
#endif
		return -1;
	}
	return s;
}

/**
 * Create socket from getaddrinfo results
 */
static int
make_sock(int stype, const char* ifname, const char* port, 
	struct addrinfo *hints, int v6only, int* noip6)
{
	struct addrinfo *res = NULL;
	int r, s, inuse, noproto;
	hints->ai_socktype = stype;
	*noip6 = 0;
	if((r=getaddrinfo(ifname, port, hints, &res)) != 0 || !res) {
		log_err("node %s:%s getaddrinfo: %s %s", 
			ifname?ifname:"default", port, gai_strerror(r),
#ifdef EAI_SYSTEM
			r==EAI_SYSTEM?(char*)strerror(errno):""
#else
			""
#endif
		);
		return -1;
	}
	if(stype == SOCK_DGRAM) {
		verbose_print_addr(res);
		s = create_udp_sock(res->ai_family, res->ai_socktype,
			(struct sockaddr*)res->ai_addr, 
			res->ai_addrlen, v6only, &inuse, &noproto);
		if(s == -1 && inuse) {
			log_err("bind: address already in use");
		} else if(s == -1 && noproto && hints->ai_family == AF_INET6){
			*noip6 = 1;
		}
	} else	{
		s = create_tcp_accept_sock(res, v6only, &noproto);
		if(s == -1 && noproto && hints->ai_family == AF_INET6){
			*noip6 = 1;
		}
	}
	freeaddrinfo(res);
	return s;
}

/**
 * Add port to open ports list.
 * @param list: list head. changed.
 * @param s: fd.
 * @param ftype: if fd is UDP.
 * @return false on failure. list in unchanged then.
 */
static int
port_insert(struct listen_port** list, int s, enum listen_type ftype)
{
	struct listen_port* item = (struct listen_port*)malloc(
		sizeof(struct listen_port));
	if(!item)
		return 0;
	item->next = *list;
	item->fd = s;
	item->ftype = ftype;
	*list = item;
	return 1;
}

/** set fd to receive source address packet info */
static int
set_recvpktinfo(int s, int family) 
{
#if defined(IPV6_RECVPKTINFO) || defined(IPV6_PKTINFO) || defined(IP_RECVDSTADDR) || defined(IP_PKTINFO)
	int on = 1;
#else
	(void)s;
#endif
	if(family == AF_INET6) {
#           ifdef IPV6_RECVPKTINFO
		if(setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			log_err("setsockopt(..., IPV6_RECVPKTINFO, ...) failed: %s",
				strerror(errno));
			return 0;
		}
#           elif defined(IPV6_PKTINFO)
		if(setsockopt(s, IPPROTO_IPV6, IPV6_PKTINFO,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			log_err("setsockopt(..., IPV6_PKTINFO, ...) failed: %s",
				strerror(errno));
			return 0;
		}
#           else
		log_err("no IPV6_RECVPKTINFO and no IPV6_PKTINFO option, please "
			"disable interface-automatic in config");
		return 0;
#           endif /* defined IPV6_RECVPKTINFO */

	} else if(family == AF_INET) {
#           ifdef IP_RECVDSTADDR
		if(setsockopt(s, IPPROTO_IP, IP_RECVDSTADDR,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			log_err("setsockopt(..., IP_RECVDSTADDR, ...) failed: %s",
				strerror(errno));
			return 0;
		}
#           elif defined(IP_PKTINFO)
		if(setsockopt(s, IPPROTO_IP, IP_PKTINFO,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			log_err("setsockopt(..., IP_PKTINFO, ...) failed: %s",
				strerror(errno));
			return 0;
		}
#           else
		log_err("no IP_RECVDSTADDR or IP_PKTINFO option, please disable "
			"interface-automatic in config");
		return 0;
#           endif /* IP_PKTINFO */

	}
	return 1;
}

/**
 * Helper for ports_open. Creates one interface (or NULL for default).
 * @param ifname: The interface ip address.
 * @param do_auto: use automatic interface detection.
 * 	If enabled, then ifname must be the wildcard name.
 * @param do_udp: if udp should be used.
 * @param do_tcp: if udp should be used.
 * @param hints: for getaddrinfo. family and flags have to be set by caller.
 * @param port: Port number to use (as string).
 * @param list: list of open ports, appended to, changed to point to list head.
 * @return: returns false on error.
 */
static int
ports_create_if(const char* ifname, int do_auto, int do_udp, int do_tcp, 
	struct addrinfo *hints, const char* port, struct listen_port** list)
{
	int s, noip6=0;
	if(!do_udp && !do_tcp)
		return 0;
	if(do_auto) {
		if((s = make_sock(SOCK_DGRAM, ifname, port, hints, 1, 
			&noip6)) == -1) {
			if(noip6) {
				log_warn("IPv6 protocol not available");
				return 1;
			}
			return 0;
		}
		/* getting source addr packet info is highly non-portable */
		if(!set_recvpktinfo(s, hints->ai_family))
			return 0;
		if(!port_insert(list, s, listen_type_udpancil)) {
			close(s);
			return 0;
		}
	} else if(do_udp) {
		/* regular udp socket */
		if((s = make_sock(SOCK_DGRAM, ifname, port, hints, 1, 
			&noip6)) == -1) {
			if(noip6) {
				log_warn("IPv6 protocol not available");
				return 1;
			}
			return 0;
		}
		if(!port_insert(list, s, listen_type_udp)) {
			close(s);
			return 0;
		}
	}
	if(do_tcp) {
		if((s = make_sock(SOCK_STREAM, ifname, port, hints, 1, 
			&noip6)) == -1) {
			if(noip6) {
				/*log_warn("IPv6 protocol not available");*/
				return 1;
			}
			return 0;
		}
		if(!port_insert(list, s, listen_type_tcp)) {
			close(s);
			return 0;
		}
	}
	return 1;
}

/** 
 * Add items to commpoint list in front.
 * @param c: commpoint to add.
 * @param front: listen struct.
 * @return: false on failure.
 */
static int
listen_cp_insert(struct comm_point* c, struct listen_dnsport* front)
{
	struct listen_list* item = (struct listen_list*)malloc(
		sizeof(struct listen_list));
	if(!item)
		return 0;
	item->com = c;
	item->next = front->cps;
	front->cps = item;
	return 1;
}

struct listen_dnsport* 
listen_create(struct comm_base* base, struct listen_port* ports,
	size_t bufsize, int tcp_accept_count,
	comm_point_callback_t* cb, void *cb_arg)
{
	struct listen_dnsport* front = (struct listen_dnsport*)
		malloc(sizeof(struct listen_dnsport));
	if(!front)
		return NULL;
	front->cps = NULL;
	front->udp_buff = ldns_buffer_new(bufsize);
	if(!front->udp_buff) {
		free(front);
		return NULL;
	}
	
	/* create comm points as needed */
	while(ports) {
		struct comm_point* cp = NULL;
		if(ports->ftype == listen_type_udp) 
			cp = comm_point_create_udp(base, ports->fd, 
				front->udp_buff, cb, cb_arg);
		else if(ports->ftype == listen_type_tcp)
			cp = comm_point_create_tcp(base, ports->fd, 
				tcp_accept_count, bufsize, cb, cb_arg);
		else if(ports->ftype == listen_type_udpancil) 
			cp = comm_point_create_udp_ancil(base, ports->fd, 
				front->udp_buff, cb, cb_arg);
		if(!cp) {
			log_err("can't create commpoint");	
			listen_delete(front);
			return NULL;
		}
		cp->do_not_close = 1;
		if(!listen_cp_insert(cp, front)) {
			log_err("malloc failed");
			comm_point_delete(cp);
			listen_delete(front);
			return NULL;
		}
		ports = ports->next;
	}
	if(!front->cps) {
		log_err("Could not open sockets to accept queries.");
		listen_delete(front);
		return NULL;
	}

	return front;
}

void
listen_list_delete(struct listen_list* list)
{
	struct listen_list *p = list, *pn;
	while(p) {
		pn = p->next;
		comm_point_delete(p->com);
		free(p);
		p = pn;
	}
}

void 
listen_delete(struct listen_dnsport* front)
{
	if(!front) 
		return;
	listen_list_delete(front->cps);
	ldns_buffer_free(front->udp_buff);
	free(front);
}

void listen_pushback(struct listen_dnsport* listen)
{
	struct listen_list *p;
	log_assert(listen);
	for(p = listen->cps; p; p = p->next)
	{
		if(p->com->type != comm_udp &&
			p->com->type != comm_tcp_accept)
			continue;
		comm_point_stop_listening(p->com);
	}
}

void listen_resume(struct listen_dnsport* listen)
{
	struct listen_list *p;
	log_assert(listen);
	for(p = listen->cps; p; p = p->next)
	{
		if(p->com->type != comm_udp &&
			p->com->type != comm_tcp_accept)
			continue;
		comm_point_start_listening(p->com, -1, -1);
	}
}

struct listen_port* 
listening_ports_open(struct config_file* cfg)
{
	struct listen_port* list = NULL;
	struct addrinfo hints;
	int i, do_ip4, do_ip6;
	int do_tcp, do_auto;
	char portbuf[32];
	snprintf(portbuf, sizeof(portbuf), "%d", cfg->port);
	do_ip4 = cfg->do_ip4;
	do_ip6 = cfg->do_ip6;
	do_tcp = cfg->do_tcp;
	do_auto = cfg->if_automatic && cfg->do_udp;
	if(cfg->incoming_num_tcp == 0)
		do_tcp = 0;

	/* getaddrinfo */
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_PASSIVE;
	/* no name lookups on our listening ports */
	if(cfg->num_ifs > 0)
		hints.ai_flags |= AI_NUMERICHOST;
	hints.ai_family = AF_UNSPEC;
#ifndef INET6
	do_ip6 = 0;
#endif
	if(!do_ip4 && !do_ip6) {
		return NULL;
	}
	if(do_auto && (!do_ip4 || !do_ip6)) {
		log_warn("interface_automatic option does not work when either do-ip4 or do-ip6 is not enabled. Disabling option.");
		do_auto = 0;
	}
	/* create ip4 and ip6 ports so that return addresses are nice. */
	if(do_auto || cfg->num_ifs == 0) {
		if(do_ip6) {
			hints.ai_family = AF_INET6;
			if(!ports_create_if(do_auto?"::0":"::1", 
				do_auto, cfg->do_udp, do_tcp, 
				&hints, portbuf, &list)) {
				listening_ports_free(list);
				return NULL;
			}
		}
		if(do_ip4) {
			hints.ai_family = AF_INET;
			if(!ports_create_if(do_auto?"0.0.0.0":"127.0.0.1", 
				do_auto, cfg->do_udp, do_tcp, 
				&hints, portbuf, &list)) {
				listening_ports_free(list);
				return NULL;
			}
		}
	} else for(i = 0; i<cfg->num_ifs; i++) {
		if(str_is_ip6(cfg->ifs[i])) {
			if(!do_ip6)
				continue;
			hints.ai_family = AF_INET6;
			if(!ports_create_if(cfg->ifs[i], 0, cfg->do_udp, 
				do_tcp, &hints, portbuf, &list)) {
				listening_ports_free(list);
				return NULL;
			}
		} else {
			if(!do_ip4)
				continue;
			hints.ai_family = AF_INET;
			if(!ports_create_if(cfg->ifs[i], 0, cfg->do_udp, 
				do_tcp, &hints, portbuf, &list)) {
				listening_ports_free(list);
				return NULL;
			}
		}
	}
	return list;
}

void listening_ports_free(struct listen_port* list)
{
	struct listen_port* nx;
	while(list) {
		nx = list->next;
		if(list->fd != -1)
			close(list->fd);
		free(list);
		list = nx;
	}
}

size_t listen_get_mem(struct listen_dnsport* listen)
{
	size_t s = sizeof(*listen) + sizeof(*listen->base) + 
		sizeof(*listen->udp_buff) + 
		ldns_buffer_capacity(listen->udp_buff);
	struct listen_list* p;
	for(p = listen->cps; p; p = p->next) {
		s += sizeof(*p);
		s += comm_point_get_mem(p->com);
	}
	return s;
}
