/*
 * Copyright (C) 2017-2018 Felix Weinrank
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
extern "C" {
#include "usrsctp.h"
}

#define MAX_PACKET_SIZE (1 << 16)

#define FUZZ_FAST
//#define FUZZ_INTERLEAVING
//#define FUZZ_EXPLICIT_EOR

static int fd_c, fd_s;
static struct socket *s_c, *s_s, *s_l;
static pthread_t tid_c, tid_s;

static char *s_cheader[12];
static char *c_cheader[12];

static int
conn_output(void *addr, void *buf, size_t length, uint8_t tos, uint8_t set_df)
{
	int *fdp = (int *)addr;

	// copy header from client and server
	if (*fdp == fd_c) {
		memcpy(c_cheader, buf, 12);
	} else if (*fdp == fd_s) {
		memcpy(s_cheader, buf, 12);
	}

	if (send(*fdp, buf, length, 0) < 0) {
		return (errno);
	} else {
		return (0);
	}
}

static int
receive_cb(struct socket* sock, union sctp_sockstore addr, void* data, size_t datalen, struct sctp_rcvinfo rcv, int flags, void* ulp_info)
{
	//printf("Message %p received on sock = %p.\n", data, (void*)sock);
	if (data) {
		if ((flags & MSG_NOTIFICATION) == 0 && 0) {
			printf("Messsage of length %d received via %p:%u on stream %d with SSN %u and TSN %u, PPID %u, context %u, flags %x.\n",
				(int)datalen,
				addr.sconn.sconn_addr,
				ntohs(addr.sconn.sconn_port),
				rcv.rcv_sid,
				rcv.rcv_ssn,
				rcv.rcv_tsn,
				ntohl(rcv.rcv_ppid),
				rcv.rcv_context,
				flags);
		}
		free(data);
	} else {
		//usrsctp_deregister_address(ulp_info);
		usrsctp_close(sock);
	}
	return (1);
}

static void*
handle_packets(void* arg)
{
	int* fdp;
	ssize_t length;
	char buf[MAX_PACKET_SIZE];

	fdp = (int*)arg;
	for (;;) {
		length = recv(*fdp, buf, MAX_PACKET_SIZE, 0);
		if (length > 0) {
			usrsctp_conninput(fdp, buf, (size_t)length, 0);
		}
	}
	return (NULL);
}

void debug_printf(const char* format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
}

int init_fuzzer(void)
{
	static uint8_t initialized = 0;
	struct sockaddr_in sin_s, sin_c;
	socklen_t name_len;

#if defined(FUZZ_FAST)
	if (initialized) {
		return 0;
	}
#endif

	usrsctp_init(0, conn_output, debug_printf);
	usrsctp_enable_crc32c_offload();

	/* set up a connected UDP socket */
	if ((fd_c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}
	if ((fd_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}
	memset(&sin_c, 0, sizeof(struct sockaddr_in));
	sin_c.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
	sin_c.sin_len = sizeof(struct sockaddr_in);
#endif
	sin_c.sin_port = htons(0);
	sin_c.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	memset(&sin_s, 0, sizeof(struct sockaddr_in));
	sin_s.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
	sin_s.sin_len = sizeof(struct sockaddr_in);
#endif
	sin_s.sin_port = htons(0);
	sin_s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(fd_c, (struct sockaddr*)&sin_c, sizeof(struct sockaddr_in)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	name_len = (socklen_t) sizeof(struct sockaddr_in);
	if (getsockname(fd_c, (struct sockaddr*)&sin_c, &name_len)) {
		perror("getsockname");
		exit(EXIT_FAILURE);
	}

	if (bind(fd_s, (struct sockaddr*)&sin_s, sizeof(struct sockaddr_in)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	name_len = (socklen_t) sizeof(struct sockaddr_in);
	if (getsockname(fd_s, (struct sockaddr*)&sin_s, &name_len)) {
		perror("getsockname");
		exit(EXIT_FAILURE);
	}

	if (connect(fd_c, (struct sockaddr*)&sin_s, sizeof(struct sockaddr_in)) < 0) {
		perror("connect");
		exit(EXIT_FAILURE);
	}
	if (connect(fd_s, (struct sockaddr*)&sin_c, sizeof(struct sockaddr_in)) < 0) {
		perror("connect");
		exit(EXIT_FAILURE);
	}
	if (pthread_create(&tid_c, NULL, &handle_packets, (void*)&fd_c)) {
		perror("pthread_create tid_c");
		exit(EXIT_FAILURE);
	}

	if (pthread_create(&tid_s, NULL, &handle_packets, (void*)&fd_s)) {
		perror("pthread_create tid_s");
		exit(EXIT_FAILURE);
	};

#ifdef SCTP_DEBUG
	usrsctp_sysctl_set_sctp_debug_on(SCTP_DEBUG_ALL);
#endif

	usrsctp_register_address((void*)&fd_c);
	usrsctp_register_address((void*)&fd_s);

	initialized = 1;

	return 0;
}

#if defined(FUZZING_MODE)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t data_size)
{
#else // defined(FUZZING_MODE)
int main(int argc, char *argv[])
{
	char *data_sample = "SCTPSCTPSCTPSCTPSCTPSCTPSCTP!!!!";
	char *data = data_sample;
	size_t data_size = strlen(data);
	FILE *file;

	if (argc > 1) {
		file = fopen(argv[1], "rb");

		if (!file) {
			perror("fopen");
			exit(EXIT_FAILURE);
		}

		fseek(file, 0, SEEK_END);
		data_size = ftell(file);
		fseek(file, 0, SEEK_SET);
		data = (char*)malloc(data_size);
		fread(data, data_size, 1, file);
		fclose(file);
	}
#endif // defined(FUZZING_MODE)

	struct sockaddr_conn sconn;
	static uint16_t port = 1;
	struct linger so_linger;
	int enable;
	char *pkt;
	struct sctp_assoc_value assoc_val;

	init_fuzzer();
	port = (port % 32768) + 1;

	if ((s_c = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, receive_cb, NULL, 0, &fd_c)) == NULL) {
		perror("usrsctp_socket");
		exit(EXIT_FAILURE);
	}

	if ((s_l = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, receive_cb, NULL, 0, &fd_s)) == NULL) {
		perror("usrsctp_socket");
		exit(EXIT_FAILURE);
	}

#if 0
	so_linger.l_onoff = 1;
	so_linger.l_linger = 0;
	if (usrsctp_setsockopt(s_c, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(struct linger)) < 0) {
		perror("usrsctp_setsockopt 1");
		exit(EXIT_FAILURE);
	}
#endif

#if defined(FUZZ_EXPLICIT_EOR)
	enable = 1;
	if (usrsctp_setsockopt(s_c, IPPROTO_SCTP, SCTP_EXPLICIT_EOR, &enable, sizeof(enable)) < 0) {
		perror("setsockopt SCTP_EXPLICIT_EOR");
		exit(EXIT_FAILURE);
	}

	enable = 1;
	if (usrsctp_setsockopt(s_l, IPPROTO_SCTP, SCTP_EXPLICIT_EOR, &enable, sizeof(enable)) < 0) {
		perror("setsockopt SCTP_EXPLICIT_EOR");
		exit(EXIT_FAILURE);
	}
#endif // defined(FUZZ_EXPLICIT_EOR)

#if defined(FUZZ_STREAM_RESET)
	assoc_val.assoc_id = SCTP_ALL_ASSOC;
	assoc_val.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
	if (usrsctp_setsockopt(s_c, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &assoc_val, sizeof(struct sctp_assoc_value)) < 0) {
		perror("setsockopt SCTP_ENABLE_STREAM_RESET");
		exit(EXIT_FAILURE);
	}
	/* Allow resetting streams. */
	assoc_val.assoc_id = SCTP_ALL_ASSOC;
	assoc_val.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
	if (usrsctp_setsockopt(s_l, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &assoc_val, sizeof(struct sctp_assoc_value)) < 0) {
		perror("setsockopt SCTP_ENABLE_STREAM_RESET");
		exit(EXIT_FAILURE);
	}
#endif //defined(FUZZ_STREAM_RESET)


#if defined(FUZZ_INTERLEAVING)

#if !defined(SCTP_INTERLEAVING_SUPPORTED)
#define SCTP_INTERLEAVING_SUPPORTED 0x00001206
#endif // !defined(SCTP_INTERLEAVING_SUPPORTED)

	enable = 2;
	if (usrsctp_setsockopt(s_c, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE, &enable, sizeof(enable)) < 0) {
		perror("usrsctp_setsockopt 1");
		exit(EXIT_FAILURE);
	}

	memset(&assoc_val, 0, sizeof(assoc_val));
	assoc_val.assoc_value = 1;
	if (usrsctp_setsockopt(s_c, IPPROTO_SCTP, SCTP_INTERLEAVING_SUPPORTED, &assoc_val, sizeof(assoc_val)) < 0) {
		perror("usrsctp_setsockopt 2");
		exit(EXIT_FAILURE);
	}

	enable = 2;
	if (usrsctp_setsockopt(s_l, IPPROTO_SCTP, SCTP_FRAGMENT_INTERLEAVE, &enable, sizeof(enable)) < 0) {
		perror("usrsctp_setsockopt 3");
		exit(EXIT_FAILURE);
	}

	assoc_val.assoc_value = 1;
	if (usrsctp_setsockopt(s_l, IPPROTO_SCTP, SCTP_INTERLEAVING_SUPPORTED, &assoc_val, sizeof(assoc_val)) < 0) {
		perror("usrsctp_setsockopt 4");
		exit(EXIT_FAILURE);
	}
#endif // defined(FUZZ_INTERLEAVING)

	/* Bind the client side. */
	memset(&sconn, 0, sizeof(struct sockaddr_conn));
	sconn.sconn_family = AF_CONN;
#if defined(HAVE_SCONN_LEN)
	sconn.sconn_len = sizeof(struct sockaddr_conn);
#endif // defined(HAVE_SCONN_LEN)
	sconn.sconn_port = htons(port);
	sconn.sconn_addr = &fd_c;
	if (usrsctp_bind(s_c, (struct sockaddr*)&sconn, sizeof(struct sockaddr_conn)) < 0) {
		perror("usrsctp_bind 1");
		exit(EXIT_FAILURE);
	}

	/* Bind the server side. */
	memset(&sconn, 0, sizeof(struct sockaddr_conn));
	sconn.sconn_family = AF_CONN;
#if defined(HAVE_SCONN_LEN)
	sconn.sconn_len = sizeof(struct sockaddr_conn);
#endif // defined(HAVE_SCONN_LEN)
	sconn.sconn_port = htons(port);
	sconn.sconn_addr = &fd_s;
	if (usrsctp_bind(s_l, (struct sockaddr*)&sconn, sizeof(struct sockaddr_conn)) < 0) {
		perror("usrsctp_bind 2");
		exit(EXIT_FAILURE);
	}

	/* Make server side passive... */
	if (usrsctp_listen(s_l, 1) < 0) {
		perror("usrsctp_listen");
		exit(EXIT_FAILURE);
	}

	/* Initiate the handshake */
	memset(&sconn, 0, sizeof(struct sockaddr_conn));
	sconn.sconn_family = AF_CONN;
#if defined(HAVE_SCONN_LEN)
	sconn.sconn_len = sizeof(struct sockaddr_conn);
#endif // defined(HAVE_SCONN_LEN)
	sconn.sconn_port = htons(port);
	sconn.sconn_addr = &fd_c;

	if (usrsctp_connect(s_c, (struct sockaddr*)&sconn, sizeof(struct sockaddr_conn)) < 0) {
		perror("usrsctp_connect");
		exit(EXIT_FAILURE);
	}

	if ((s_s = usrsctp_accept(s_l, NULL, NULL)) == NULL) {
		perror("usrsctp_accept");
		exit(EXIT_FAILURE);
	}

#if 0
	if (usrsctp_setsockopt(s_s, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(struct linger)) < 0) {
		perror("usrsctp_setsockopt 3");
		exit(EXIT_FAILURE);
	}
#endif

	// close listening socket
	usrsctp_close(s_l);

#if defined(FUZZ_EXPLICIT_EOR)
	struct sctp_sndinfo sndinfo;
	memset(&sndinfo, 0, sizeof(struct sctp_sndinfo));
	//sndinfo.snd_sid = 1207;
	//sndinfo.snd_flags 	= SCTP_EOR;
	sndinfo.snd_ppid 	= htonl(1207);
	if (usrsctp_sendv(s_c, &sndinfo, sizeof(struct sctp_sndinfo), NULL, 0, &sndinfo, (socklen_t)sizeof(struct sctp_sndinfo), SCTP_SENDV_SNDINFO, 0) < 0) {
		perror("sctp_sendv");
		exit(EXIT_FAILURE);
	}
#endif //defined(FUZZ_EXPLICIT_EOR)

	// inject packet
	pkt = (char *) malloc(data_size + 12);
	memcpy(pkt, c_cheader, 12);
	memcpy(pkt + 12, data, data_size);
	usrsctp_conninput(&fd_s, pkt, data_size + 12, 0);
	free(pkt);


#if !defined(FUZZING_MODE)
	if (data != data_sample) {
		free(data);
	}
#endif // !defined(FUZZING_MODE)

	usrsctp_close(s_c);

#if !defined(FUZZ_FAST) || !defined(FUZZING_MODE)

	usrsctp_deregister_address((void*)&fd_c);
	usrsctp_deregister_address((void*)&fd_s);

	while(usrsctp_finish()) {
		//sleep(1);
		//printf("finishing....\n");
	}

	//pthread_join(tid_c, NULL);
	//pthread_join(tid_s, NULL);

	close(fd_c);
	close(fd_s);
#endif // !defined(FUZZ_FAST)

	return (0);
}
