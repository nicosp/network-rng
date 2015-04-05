/*
 Copyright (c) 2013, Nicos Panayides <nicosp@gmail.com>
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 TCP server to send random data from all connected Quantis USB devices to clients.

 Protocol:
 - All integers are in network byte order (big endian).
 - Clients should send requests at least once every 20s.

 Request:
 uint32_t entropy_requested

 entropy_requested can be 0.

 Response 1..n:
 uint32_t length.
 ... Entropy (of size length).

 Please note that the server may send multiple responses for one request.
 The server will never exceed the sum of all requested entropy but there
 are no guarantees on the number of responses.

 The server does not enforce any kind of request timeout and requests never
 fail.

 The server will always send the data in the order it was received from the hardware.
 This is used for testing and to ensure that there is absolutely no difference between
 receiving data from the RNG directly or through the server.

 TESTING:
   1. Server: Use the output option to save all data as soon as its received without copies or
   any other manipulation.
   2. Client: Save the data received from the client.
   3. Both files should be identical (check with diff etc).
*/

#define __STDC_FORMAT_MACROS

#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif /* __STDC_VERSION__ */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <syslog.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <time.h>

#include <sys/signalfd.h>

#include "databuf.h"
#include "quantisusb.h"
#include "version.h"

#define MAX_CLIENTS (512)

/**
* Maximum time to allow for idle clients in seconds.
*/
#define MAX_IDLE_TIME (30)

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof(a[0]))
#define HEADER_SIZE (sizeof(uint32_t))
#define MAX_FRAME_SIZE (65536)

/*
 Space available to read data from at least one device
*/
#define BUFFER_SPACE (512UL*16UL)

#define DEFAULT_ENTROPY_BUF_SIZE ((2*1024*1024))

#define DEFAULT_PORT (4545)
#define DEFAULT_VERBOSITY (2)

#define MIN_BUF_SIZE (BUFFER_SPACE)

/**
* Connected client information
*/
struct Client {
	/* Entropy requested but not sent */
	uint32_t entropy_requested;
	/* Entropy that is waiting to be send. Always send first without header */
	uint32_t entropy_pending;

	/* Whether the client requested a keep-alive */
	int keepalive_pending;

	/* Handle headers split between different writes */
	uint32_t header_bytes_pending;

	/* Client socket */
	int socket;

	/* Time of last request. Used to enforce timeouts */
	struct timespec last_request;
};

typedef struct Client Client;

static QuantisUSBContext *ctx;


static Client *clients;
static size_t client_sockets_length;
static size_t num_client_sockets;

/** Current receiver index.
 Used to ensure fairness.

 Data is send in a round-robin fashion. The server sends all data requested
 that is immediately available and moves to the next client.
*/
static int receiver_index = 0;

/**
 Bounded queue for reading random bytes from connected devices.
*/
static DataBuffer *data_buf;

static fd_set writefds;

static int test_fd = -1;


static int client_add(int sock)
{
	if (num_client_sockets == client_sockets_length) {
		return -1;
	}

	memset(&clients[num_client_sockets], 0, sizeof(Client));
	clients[num_client_sockets].socket = sock;

	num_client_sockets++;

	return 0;
}

static int client_remove_by_index(size_t i)
{
	if (i >= num_client_sockets) {
		return -1;
	}

	num_client_sockets--;
	memmove(clients + i, clients + i + 1, sizeof(Client) * (num_client_sockets - i));

	/* Update the receiver index if necessary  */
	if (receiver_index >= i) {
		receiver_index--;
		if (receiver_index < 0) {
			receiver_index = 0;
		}
	}

	return 0;
}

static void receiver_advance(void)
{
	receiver_index++;
	if (receiver_index == num_client_sockets) {
		receiver_index = 0;
	}
}

static void send_entropy(void)
{
	static unsigned char send_buf[MAX_FRAME_SIZE];
	int sock;
	uint32_t write_size;
	uint32_t header_size;
	ssize_t send_status;
	uint32_t send_len;
	int clients_checked = 0;
	uint32_t entropy_send;

	while(clients_checked < num_client_sockets) {
		clients_checked++;

		if (!clients[receiver_index].keepalive_pending && !data_buf_available(data_buf)) {
			goto next_receiver;
		}

		/* Incomplete header */
		if (clients[receiver_index].header_bytes_pending) {
			header_size = clients[receiver_index].header_bytes_pending;
			write_size = clients[receiver_index].entropy_pending;

		/* Incomplete entropy frame */
		} else if (clients[receiver_index].entropy_pending) {
			header_size = 0;
			write_size = clients[receiver_index].entropy_pending;
		} else {
			header_size = HEADER_SIZE;
			write_size = clients[receiver_index].entropy_requested;

			/* No entropy or keep-alive requested. Nothing to send. */
			if (!write_size && !clients[receiver_index].keepalive_pending) {
				goto next_receiver;
			}
		}

		sock = clients[receiver_index].socket;

		/* We have data to send but the socket is not available for write yet */
		if (!FD_ISSET(sock, &writefds)) {
			goto next_receiver;
		}

		/* Never exceed max frame size  */
		if (write_size + header_size > MAX_FRAME_SIZE) {
			write_size = MAX_FRAME_SIZE - header_size;
		}

		if (write_size > data_buf_available(data_buf)) {
			write_size = (uint32_t)data_buf_available(data_buf);
		}

		/* Get random data from buffer */
		write_size = (uint32_t)data_buf_read(data_buf, send_buf+header_size, write_size);

		/* Write the header */
		if (header_size) {
			if (!clients[receiver_index].header_bytes_pending) {
				send_len = htonl(write_size);
				memcpy(send_buf, &send_len, HEADER_SIZE);
			} else {
				/* Incomplete header */
				unsigned char buf[HEADER_SIZE];
				size_t buf_offset;

				/* Entropy pending cannot change without sending something
                                 * so it is safe to assume that the bytes sent are still valid.
                                 */
				send_len = htonl(clients[receiver_index].entropy_pending);
				memcpy(buf, &send_len, HEADER_SIZE);

				buf_offset = HEADER_SIZE - header_size;

				memcpy(send_buf, buf + buf_offset, header_size);
			}
		}

		/* Write entropy */
		send_status = send(sock, send_buf, write_size+header_size, MSG_NOSIGNAL);
		if (send_status >= 0) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Sent %d bytes of entropy to client", write_size);

			clients[receiver_index].keepalive_pending = 0;
			clients[receiver_index].header_bytes_pending = 0;

			/* Incomplete header */
			if (send_status < header_size) {
				entropy_send = 0;
				clients[receiver_index].header_bytes_pending = (uint32_t)(header_size-send_status);

				if (clients[receiver_index].entropy_pending) {
					/* Already sending previous packet. Nothing to do here */
				} else if (!write_size) {
					/* No entropy requested. Mark keep-alive for later. */
					clients[receiver_index].keepalive_pending = 1;
				} else {
					/* Mark the payload as pending */
					clients[receiver_index].entropy_requested -= write_size;
					clients[receiver_index].entropy_pending = write_size;
				}

			} else if (header_size == HEADER_SIZE) {
				entropy_send = (uint32_t)send_status - header_size;
				clients[receiver_index].entropy_requested -= write_size;
				clients[receiver_index].entropy_pending = write_size - entropy_send;
			} else {
				entropy_send = (uint32_t)send_status - header_size;
				clients[receiver_index].entropy_pending -= entropy_send;
			}

			/* Return unsent entropy to the data buffer */
			if (entropy_send < write_size) {
				data_buf_unread(data_buf, send_buf+send_status, write_size - entropy_send);
			}

		} else {
			/* Return unsent entropy to the buffer */
			data_buf_unread(data_buf, send_buf+HEADER_SIZE, write_size);

			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_WARNING), "Send error: %s", strerror(errno));
			}
		}

next_receiver:
		receiver_advance();
	}
}

static void on_read(QuantisUSBDevice *device, const unsigned char *data, int data_len)
{
	size_t data_saved;

	if (test_fd >= 0) {
		write(test_fd, data, (size_t)data_len);
	}

	data_saved = data_buf_write(data_buf, data, (size_t)data_len);

	if (data_saved < data_len) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_WARNING), "%zu bytes of entropy wasted", (size_t)data_len - data_saved);
	}
}

/**
* Called when an error occurs when reading from the device. Also called when read is cancelled and
* errno will be set to ECANCELLED in that case.
*/
static void on_error(QuantisUSBDevice *device)
{
	syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "USB device error %s", strerror(errno));
}

static int should_read(void)
{
	return data_buf_space(data_buf) >= BUFFER_SPACE;
}

static void on_device(QuantisUSBDevice *device, int present)
{
	char sn[128];
	static const char* status[] = {"Closed", "Opened"};

	if (quantis_usb_get_serial_number(device, sn, 128)) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Unable to get Device serial number: %s", strerror(errno));
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "%s USB RNG device", status[!!present]);
	} else {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "%s USB RNG device. (Serial Number: %s)", status[!!present], sn);
	}

	if (present && should_read()) {
		quantis_usb_read(device);
	}
}

static void error_log(QuantisUSBContext *ctx, const char *msg)
{
	syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "%s: %s", msg, strerror(errno));
}

/*
 Use all available devices
*/
static int should_open_device(QuantisUSBDevice *device)
{
	return 1;
}

#if 0
/*
 This is an example on how to cause the daemon to use a specific device only
*/
static int should_open_device(QuantisUSBDevice *device)
{
	char sn[128];

	if (!quantis_usb_get_serial_number(device, sn, 128)) {
		return !strcmp("100887A410", sn);
	}

	return 0;
}
#endif

static int setnonblocking(int sock)
{
	int opts;

	opts = fcntl(sock, F_GETFL);
	if (opts < 0) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Error while setting socket to non-blocking. fcntl(F_GETFL) failed: %s", strerror(errno));
		return -1;
	}

	opts = (opts | O_NONBLOCK);
	if (fcntl(sock, F_SETFL, opts) < 0) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Error while setting socket to non-blocking. fcntl(F_SETFL) failed: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static void show_usage(const char *app)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n\n"
		"Options:\n"
		"-4       Listens to IPv4 address only. (Default: both)\n"
		"-6       Listens to IPv6 address only. (Default: both)\n"
		"-b SIZE  Buffer size. (Default: %d)\n"
		"-h       Help. Show this message and exit\n"
		"-l LEVEL Log Verbosity. (0 Errors, 1 Warnings, 2 Info, 3 Debug) (Default: %d)\n"
		"-p PORT  Port to listen to (Default: %d)\n"
                "-o FILE  Write all random numbers to this file. Used for testing.\n"
		"-v       Show version number.\n"
		, app, DEFAULT_ENTROPY_BUF_SIZE, DEFAULT_VERBOSITY, DEFAULT_PORT);
}

static void show_version(const char *app)
{
	fprintf(stderr, "quantis-rngd %s (%s)\n", 
		VERSION, 
		__DATE__);
}

static size_t get_max_alloc_size(void)
{
	int res;
	struct rlimit rlim;

	res = getrlimit(RLIMIT_AS, &rlim);
	if (res) {
		return LONG_MAX;
	}

	if (rlim.rlim_cur == RLIM_INFINITY) {
		return LONG_MAX;
	}

	return (size_t)rlim.rlim_cur;
}

int main(int argc, char **argv)
{
	int exit_status = 0;
	struct sigaction sa;

    	int sock = -1;
	int sock6 = -1;
	int client_sock;
	/* server address */
	struct sockaddr_in6 local6;
	struct sockaddr_in6 remote6;

	struct sockaddr_in local;
	struct sockaddr_in remote;
	socklen_t remote_len = sizeof(remote);
	int nfds;
	fd_set readfds;
	fd_set errorfds;
	int select_status;
	int quantis_status;
	int status;
	struct timeval timeout;
	struct timespec now;
	int64_t idle_time; /* idle time in milliseconds */
	ssize_t i;
	int so_reuseaddr = 1;
	int sfd;
	sigset_t mask;
	int port = DEFAULT_PORT;
	int opt;
	int ipv4_enabled = 1;
	int ipv6_enabled = 1;
	int verbosity = DEFAULT_VERBOSITY;
	size_t buf_size = DEFAULT_ENTROPY_BUF_SIZE;
	const char *outfile = NULL;

	/* Option handling */
	while ((opt = getopt(argc, argv, "46b:hl:o:p:v")) != -1) {
        	switch (opt) {
			case '4':
				ipv4_enabled = 1;
				ipv6_enabled = 0;
				break;
			case '6':
				ipv4_enabled = 0;
				ipv6_enabled = 1;
				break;
			case 'b':
				if (sscanf(optarg, "%zu", &buf_size) != 1) {
					fprintf(stderr, "Invalid buffer size\n");
					exit(1);
				}

				if (buf_size < MIN_BUF_SIZE || buf_size > get_max_alloc_size()) {
					fprintf(stderr, "Buffer size out of bounds. Allowed (%zu - %zu)\n", MIN_BUF_SIZE, get_max_alloc_size());
					exit(1);
				}

				break;
			case 'h':
				show_usage(argv[0]);
				exit(0);
				break;
			case 'l':
				if (sscanf(optarg, "%d", &verbosity) != 1) {
					fprintf(stderr, "Invalid port number\n");
					exit(1);
				}
				break;
			case 'o':
				outfile = optarg;
				break;
        		case 'p':
				if (sscanf(optarg, "%d", &port) != 1) {
					fprintf(stderr, "Invalid port number\n");
					exit(1);
				}
				break;
			case 'v':
				show_version(argv[0]);
				show_usage(argv[0]);
				exit(1);
				break;
        		default: /* '?' */
				show_usage(argv[0]);
				exit(1);
				break;
        	}
    	}

	switch (verbosity) {
		case -1:
			setlogmask(LOG_UPTO(LOG_CRIT));
			break;
		case 0:
			setlogmask(LOG_UPTO(LOG_ERR));
			break;
		case 1:
			setlogmask(LOG_UPTO(LOG_WARNING));
			break;
		case 2:
			setlogmask(LOG_UPTO(LOG_INFO));
			break;
		case 3:
			setlogmask(LOG_UPTO(LOG_DEBUG));
			break;
		default:
			fprintf(stderr, "Invalid verbosity\n");
			exit(1);
			break;
	}

	if (optind < argc) {
	        fprintf(stderr, "Extra characters after options\n");
	        exit(1);
	}

	if (!ipv4_enabled && !ipv6_enabled) {
		fprintf(stderr, "No listen addresses are enabled. Aborting.\n");
		exit(1);
	}

	/* Ignore SIGPIPE */
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0;

	if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, 0) == -1) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to ignore SIGPIPE: %s", strerror(errno));
		return -1;
        }

	/* Handle SIGTERM and SIGINT. */
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

	/* Block the signals that will be handled using signalfd(), so they don't
	 * cause signal handlers or default signal actions to execute. */
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "sigprocmask error: %s", strerror(errno));
		return 1;
	}

	/* Create a file descriptor from which we will read the signals. */
	sfd = signalfd (-1, &mask, 0);
	if (sfd < 0) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "signalfd error: %s", strerror(errno));
		return 1;
	}

	data_buf = data_buf_create(buf_size);
	if (!data_buf) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Out of memory");
		return -3;
	}


	client_sockets_length = MAX_CLIENTS;
	clients = malloc(client_sockets_length * sizeof(Client));
	if (!clients) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Out of memory");
		return -3;
	}

	if (outfile) {
		mode_t mode = S_IRUSR | S_IWUSR;

		test_fd = open(outfile, O_WRONLY|O_CREAT|O_EXCL|O_SYNC, mode);
		if (test_fd < 0) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to create file to write test data");
			return -3;
		}
	}

	ctx = quantis_usb_init(on_read, on_error, on_device, should_open_device, error_log, NULL);

	if (!ctx) {
		syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to initialize libusb: %s", strerror(errno));
		exit_status = -3;
		goto cleanup;
	}

	if (ipv4_enabled) {
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Unable to create IPv4 socket: %s", strerror(errno));
			exit_status = -1;
			goto cleanup;
		}
	}

	if (ipv6_enabled) {
		sock6 = socket(AF_INET6, SOCK_STREAM, 0);
		if (sock6 < 0) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Unable to create IPv6 socket: %s", strerror(errno));
			exit_status = -1;
			goto cleanup;
		}
	}

	if (sock >= 0) {
		memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = INADDR_ANY;
		local.sin_port = htons(port);

		setsockopt(sock,SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));

		if (bind(sock, (struct sockaddr *)&local, sizeof(local)) == -1) {
		        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to bind IPv4 socket: %s", strerror(errno));
			exit_status = 1;
			goto cleanup;
		}
	}

	if (sock6 >= 0) {
		memset(&local6, 0, sizeof(local6));
		local6.sin6_family = AF_INET6;
		local6.sin6_addr = in6addr_any;
		local6.sin6_port = htons(port);

		setsockopt(sock6, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));
		setsockopt(sock6, IPPROTO_IPV6, IPV6_V6ONLY, &so_reuseaddr, sizeof(so_reuseaddr));

		if (bind(sock6, (struct sockaddr *)&local6, sizeof(local6)) == -1) {
		        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to bind IPv6 socket: %s", strerror(errno));
			exit_status = 1;
			goto cleanup;
		}
	}

	quantis_usb_enable_hotplug(ctx, 1);

	if (sock >= 0) {
		if (listen(sock, 5) < 0) {
		        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to listen to IPv4 socket: %s", strerror(errno));
			exit_status = 1;
			goto cleanup;
		}

		if (setnonblocking(sock)) {
			exit_status = 1;
			goto cleanup;
		}
	}

	if (sock6 >= 0) {
		if (listen(sock6, 5) < 0) {
		        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Unable to listen to IPv6 socket: %s", strerror(errno));
			exit_status = 1;
			goto cleanup;
		}

		if (setnonblocking(sock6)) {
			exit_status = 1;
			goto cleanup;
		}
	}

	quantis_usb_read_all(ctx);

	syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Listening for connections on port %d", port);

	for(;;) {
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_ZERO(&errorfds);
		nfds = 0;


		FD_SET(sfd, &readfds);
		nfds = sfd + 1;

		if (sock >= 0) {
			FD_SET(sock, &readfds);
			FD_SET(sock, &writefds);
			FD_SET(sock, &errorfds);

			if (nfds <= sock) {
				nfds = sock + 1;
			}
		}

		if (sock6 >= 0) {
			FD_SET(sock6, &readfds);
			FD_SET(sock6, &writefds);
			FD_SET(sock6, &errorfds);

			if (nfds <= sock6) {
				nfds = sock6 + 1;
			}
		}

		for(i=0; i < num_client_sockets; i++) {
			FD_SET(clients[i].socket, &readfds);
			FD_SET(clients[i].socket, &writefds);
			FD_SET(clients[i].socket, &errorfds);

			if (nfds <= clients[i].socket) {
				nfds = clients[i].socket + 1;
			}
		}

		timeout.tv_sec = MAX_IDLE_TIME / 2;
		timeout.tv_usec = 0;

		select_status = quantis_usb_before_poll(ctx, &nfds, &readfds, &writefds, &errorfds, &timeout);
		if (select_status) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Quantis error: %s", strerror(errno));
			break;
		}

		select_status = select(nfds, &readfds, &writefds, &errorfds, &timeout);
		if (select_status < 0) {
			if (errno == EINTR) {
				continue;
			}

			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Select error: %s", strerror(errno));
			break;
		}

		quantis_status = quantis_usb_after_poll(ctx, !select_status, &readfds, &writefds, &errorfds);
		if (quantis_status < 0) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "Quantis error: %s", strerror(errno));
			break;
		}


		if (FD_ISSET(sfd, &readfds)) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Process signalled. Exiting");
			break;
		}


		if (FD_ISSET(sock, &errorfds)) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "IPv4 socket error: %s", strerror(errno));
			break;
		}

		if (FD_ISSET(sock6, &errorfds)) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "IPv6 socket error: %s", strerror(errno));
			break;
		}

		if (clock_gettime(CLOCK_MONOTONIC, &now)) {
			syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_CRIT), "clock_gettime error: %s", strerror(errno));
			break;
		}

		/* Handle new connections IPv4 */
	 	if (FD_ISSET(sock, &readfds)) {
			remote_len = sizeof(remote);
			client_sock = accept(sock, (struct sockaddr *)&remote, &remote_len);

			if (client_sock >= 0) {
				if (setnonblocking(client_sock)) {
					close(client_sock);
				} else {
					char str[INET_ADDRSTRLEN];
					inet_ntop(AF_INET, &(remote.sin_addr), str, INET_ADDRSTRLEN);

					status = client_add(client_sock);

					if (status < 0) {
						close(client_sock);
						syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Rejected connection from %s. Too many clients", str);
					} else {
						memcpy(&clients[num_client_sockets-1].last_request, &now, sizeof(struct timespec));
						syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Accepted connection from %s:%d. Open connections: %zu", str, (int)remote.sin_port, num_client_sockets);
					}
				}
			} else {
				if (errno != EAGAIN) {
					syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Could not accept client connection: %s", strerror(errno));
				}
			}
		}

		/* Handle new connections IPv6 */
	 	if (FD_ISSET(sock6, &readfds)) {
			remote_len = sizeof(remote6);
			client_sock = accept(sock6, (struct sockaddr *)&remote6, &remote_len);

			if (client_sock >= 0) {
				if (setnonblocking(client_sock)) {
					close(client_sock);
				} else {
					char str[INET6_ADDRSTRLEN];
					inet_ntop(AF_INET6, &(remote6.sin6_addr), str, INET6_ADDRSTRLEN);

					status = client_add(client_sock);

					if (status < 0) {
						close(client_sock);
						syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Rejected connection from %s. Too many clients", str);
					} else {
						memcpy(&clients[num_client_sockets-1].last_request, &now, sizeof(struct timespec));
						syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Accepted connection from %s:%d. Open connections: %zu", str, (int)remote6.sin6_port, num_client_sockets);
					}
				}
			} else {
				if (errno != EAGAIN) {
					syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Could not accept client connection: %s", strerror(errno));
				}
			}
		}

		/* Handle requests */
		for(i=0; i < num_client_sockets; i++) {
			if (FD_ISSET(clients[i].socket, &errorfds)) {
				client_remove_by_index((size_t)i);
				i--;

				syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "Client disconnected: %s. Open connections: %zu", strerror(errno), num_client_sockets);
				continue;
			}

			/* Handle idle clients */
			idle_time = (now.tv_sec - clients[i].last_request.tv_sec);
			idle_time += (now.tv_nsec - clients[i].last_request.tv_nsec) / 1000000000L;

			if (idle_time >= MAX_IDLE_TIME) {
				client_remove_by_index((size_t)i);
				i--;
				syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Client connection time-out: %s. Open connections: %zu", strerror(errno), num_client_sockets);
				continue;
			}

			if (FD_ISSET(clients[i].socket, &readfds)) {
				ssize_t recv_status;
				uint32_t entropy_requested;
				uint32_t new_entropy;

				recv_status = recv(clients[i].socket, &entropy_requested, sizeof(uint32_t), 0);
				/* Client disconnected */
				if (recv_status == 0) {
					client_remove_by_index((size_t)i);
					i--;
					syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Client disconnected. Open connections: %zu", num_client_sockets);
					continue;
				}

				if (recv_status < 0) {
					if (errno != EWOULDBLOCK && errno != EINPROGRESS) {
						client_remove_by_index((size_t)i);
						i--;

						syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Client connection error: %s. Open connections: %zu", strerror(errno), num_client_sockets);
					}

					continue;
				}

				/* We received less than 4 bytes. It is highly unlikely to get partial messages of this size so assume the
				 * client is broken and disconnect.
				*/
				if (recv_status < sizeof(uint32_t)) {
					client_remove_by_index((size_t)i);
					i--;
					continue;
				}


				entropy_requested = ntohl(entropy_requested);

				syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Client requested %d bytes of entropy", entropy_requested);

				new_entropy = entropy_requested + clients[i].entropy_requested;

				/* Overflow. No way to handle this properly. Disconnect client */
				if (new_entropy < clients[i].entropy_requested) {
					client_remove_by_index((size_t)i);
					i--;
					continue;
				}

				clients[i].entropy_requested = new_entropy;
				memcpy(&clients[i].last_request, &now, sizeof(struct timespec));
			
				if (!entropy_requested) {
					clients[i].keepalive_pending = 1;
				}
			}
		}

		send_entropy();

		/* If we are low on entropy make sure we replenish it before it runs out */
		if (should_read()) {
			quantis_usb_read_all(ctx);
		}

		// syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Finished processing events iteration. Entropy available: %zu. Connections: %zu\n", data_buf_available(data_buf), num_client_sockets);
	}

cleanup:

	if (test_fd >= 0) {
		close(test_fd);
	}

	if (sfd >= 0) {
		close(sfd);
	}

	/* Close client sockets */
	for(i=0; i < num_client_sockets; i++) {
		close(clients[i].socket);
	}

	if (clients) {
		free(clients);
	}

	if (sock >= 0) {
		close(sock);
	}

	if (sock6 >= 0) {
		close(sock6);
	}

	quantis_usb_destroy(ctx);
	data_buf_destroy(data_buf);

	syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_INFO), "Daemon shutdown. Status: %d", exit_status);

	return exit_status;
}
