// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <error.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/udp.h>
#include <stdint.h>

#define PORT 5000
#define BUF_SIZE 256

#define UDP_REPAIR		2
#define UDP_REPAIR_QUEUE	3
#define UDP_REPAIR_CORK		4
#define UDP_SEGMENT		103

enum {
	UDP_NO_QUEUE,
	UDP_RECV_QUEUE,
	UDP_SEND_QUEUE,
	UDP_QUEUES_NR
};

struct user_cork {
	unsigned short		ttl;
	unsigned short		gso_size;
	short			tos;
	unsigned short		tsflags;
};

char send_buf[BUF_SIZE];
struct udp_dump {
	union {
		struct sockaddr_in addr_v4;
		struct sockaddr_in6 addr_v6;
	};
	char buf[BUF_SIZE];
	struct user_cork ucork;
};

struct sockaddr_in addr_v4;
struct sockaddr_in6 addr_v6;

volatile int server_setup_complete = 0;

int udp_server(int is_udp4)
{
	int sock, ret;
	unsigned short family;
	struct sockaddr *server_addr;
	unsigned int addr_len;

	if (is_udp4) {
		family = AF_INET;
		server_addr = (struct sockaddr *) &addr_v4;
		addr_len = sizeof(addr_v4);
	} else {
		family = AF_INET6;
		server_addr = (struct sockaddr *) &addr_v6;
		addr_len = sizeof(addr_v6);
	}

	sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		error(1, errno, "socket server");

	ret = bind(sock, server_addr, addr_len);
	if (ret < 0)
		error(1, errno, "bind server socket");

	return sock;
}

void server_recv(int sock)
{
	char recv_buf[BUF_SIZE];
	int ret;

	ret = recv(sock, recv_buf, sizeof(recv_buf), 0);
	if (ret < 0)
		error(1, errno, "recv in server");

	if (memcmp(recv_buf, send_buf, BUF_SIZE))
		error(1, 0, "recv: mismatch data");
}

int create_corked_udp_client(int is_udp4)
{
	int sock, ret, val = 1;
	unsigned short family = is_udp4 ? AF_INET : AF_INET6;

	sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		error(1, errno, "socket client");

	ret = setsockopt(sock, SOL_UDP, UDP_CORK, &val, sizeof(val));
	if (ret < 0)
		error(1, errno, "setsockopt cork udp");

	return sock;
}

struct udp_dump *checkpoint(int sock, int is_udp4)
{
	int ret, val;
	unsigned int addr_len;
	struct udp_dump *dump;
	struct sockaddr *addr;
	struct user_cork *ucork;
	socklen_t cork_len;

	dump = malloc(sizeof(*dump));
	if (!dump)
		error(1, 0, "malloc");

	if (is_udp4) {
		addr = (struct sockaddr *) &dump->addr_v4;
		addr_len = sizeof(dump->addr_v4);
	} else {
		addr = (struct sockaddr *) &dump->addr_v6;
		addr_len = sizeof(dump->addr_v6);
	}
	ucork = &dump->ucork;

	val = 1;
	ret = setsockopt(sock, SOL_UDP, UDP_REPAIR, &val, sizeof(val));
	if (ret < 0)
		error(1, errno, "setsockopt udp_repair");

	cork_len = sizeof(*ucork);
	ret = getsockopt(sock, SOL_UDP, UDP_REPAIR_CORK, ucork, &cork_len);
	if (ret < 0)
		error(1, errno, "getsockopt udp_repair_cork");

	val = UDP_SEND_QUEUE;
	ret = setsockopt(sock, SOL_UDP, UDP_REPAIR_QUEUE, &val, sizeof(val));
	if (ret < 0)
		error(1, errno, "setsockopt udp_repair_queue");

	ret = recvfrom(sock, dump->buf, BUF_SIZE / 2, MSG_PEEK,
		       addr, &addr_len);
	if (ret < 0)
		error(1, errno, "dumping send queue");

	ret = recvfrom(sock, dump->buf + BUF_SIZE / 2,
		       BUF_SIZE - BUF_SIZE / 2, MSG_PEEK,
		       addr, &addr_len);
	if (ret < 0)
		error(1, errno, "dumping send queue");

	if(memcmp(dump->buf, send_buf, BUF_SIZE))
		error(1, 0, "dump: data mismatch");

	return dump;
}

void send_one(int sock, struct udp_dump *dump, int is_udp4)
{
	struct msghdr msg = {0};
	struct iovec iov = {0};
	struct cmsghdr *cm;
	int ret, controllen;
	struct user_cork *ucork = &dump->ucork;
	char *control;

	iov.iov_base = dump->buf;
	iov.iov_len = BUF_SIZE;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (is_udp4) {
		msg.msg_name = (struct sockaddr *) &dump->addr_v4;
		msg.msg_namelen = sizeof(dump->addr_v4);
	} else {
		msg.msg_name = (struct sockaddr *) &dump->addr_v6;
		msg.msg_namelen = sizeof(dump->addr_v6);
	}

	controllen = CMSG_SPACE(sizeof(uint16_t)) + CMSG_SPACE(sizeof(uint32_t));
	if (ucork->tos >= 0)
		controllen += CMSG_SPACE(sizeof(int));

	if (ucork->ttl)
		controllen += CMSG_SPACE(sizeof(int));

	control = malloc(controllen);
	if (!control)
		error(1, 0, "malloc");

	msg.msg_control = control;
	msg.msg_controllen = controllen;

	cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_UDP;
	cm->cmsg_type = UDP_SEGMENT;
	cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
	*((uint16_t *) CMSG_DATA(cm)) = ucork->gso_size;

	if (ucork->ttl) {
		cm = CMSG_NXTHDR(&msg, cm);
		cm->cmsg_level = SOL_IP;
		cm->cmsg_type = IP_TTL;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		*((int *) CMSG_DATA(cm)) = ucork->ttl;
	}

	if (ucork->tos >= 0) {
		cm = CMSG_NXTHDR(&msg, cm);
		cm->cmsg_level = SOL_IP;
		cm->cmsg_type = IP_TOS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		*((int *) CMSG_DATA(cm)) = ucork->tos;
	}

	cm = CMSG_NXTHDR(&msg, cm);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SO_TIMESTAMPING_OLD;
	cm->cmsg_len = CMSG_LEN(sizeof(uint32_t));
	*((uint32_t *) CMSG_DATA(cm)) = ucork->tsflags;

	ret = sendmsg(sock, &msg, 0);
	if (ret < 0)
		error(1, errno, "send data");
	free(control);
}

void restore(int sock, struct udp_dump *dump, int is_udp4)
{
	int val;

	send_one(sock, dump, is_udp4);

	val = 0;
	if (setsockopt(sock, SOL_UDP, UDP_CORK, &val, sizeof(val)) < 0)
		error(1, errno, "setsockopt un-cork udp");
}

void run_test(int is_udp4_sock, int is_udp4_packet)
{
	int server_sock, client_sock, ret;
	struct udp_dump *dump;
	struct sockaddr *addr;
	unsigned int addr_len;

	if (is_udp4_packet) {
		addr = (struct sockaddr *) &addr_v4;
		addr_len = sizeof(addr_v4);
	} else {
		addr = (struct sockaddr *) &addr_v6;
		addr_len = sizeof(addr_v6);
	}

	server_sock = udp_server(is_udp4_packet);
	client_sock = create_corked_udp_client(is_udp4_sock);

	ret = sendto(client_sock, send_buf, sizeof(send_buf), 0,
	       addr, addr_len);
	if (ret < 0)
		error(1, errno, "send data");

	dump = checkpoint(client_sock, is_udp4_sock);
	close(client_sock);

	client_sock = create_corked_udp_client(is_udp4_sock);
	restore(client_sock, dump, is_udp4_sock);
	int val;
	val = 0;
	setsockopt(client_sock, SOL_UDP, UDP_CORK, &val, sizeof(val));
	server_recv(server_sock);

	close(server_sock);
	close(client_sock);
	free(dump);
}

void init()
{
	addr_v4.sin_family	= AF_INET;
	addr_v4.sin_port	= htons(PORT);
	addr_v4.sin_addr.s_addr	= inet_addr("127.0.0.1");

	addr_v6.sin6_family	= AF_INET6;
	addr_v6.sin6_port	= htons(PORT);
	inet_pton(AF_INET6, "::1", &addr_v6.sin6_addr);

	memset(send_buf, 'A', BUF_SIZE / 2);
	memset(send_buf + BUF_SIZE / 2, 'B', BUF_SIZE - BUF_SIZE / 2);
}

int main()
{
	init();
	fprintf(stderr, "Test udp4 socket\n");
	run_test(1, 1);
	fprintf(stderr, "Test udp6 socket sending udp4 packet\n");
	run_test(0, 1);
	fprintf(stderr, "Test udp6 socket sending udp6 packet\n");
	run_test(0, 0);
	fprintf(stderr, "Ok\n");
	return 0;
}
