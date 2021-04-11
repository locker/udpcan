#include <assert.h>
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <netdb.h>
#include <net/if.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void *xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p) errx(EXIT_FAILURE, "Out of memory");
	return p;
}

static char *xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p) errx(EXIT_FAILURE, "Out of memory");
	return p;
}

/*
 * Returns a human-readable string representation of a CAN frame in format
 * <can_id>#<data>. Uses a statically allocated buffer.
 */
static const char *str_can_frame(const struct can_frame *frame)
{
	static char buf[32];
	char *s = buf, *end = buf + sizeof(buf);
	s += snprintf(s, end - s, "%.3X#", (unsigned)frame->can_id);
	for (int i = 0; i < (int)frame->can_dlc; i++)
		s += snprintf(s, end - s, "%.2X", (unsigned)frame->data[i]);
	return buf;
}

#define PACKED_CAN_FRAME_MAX_DATA_SIZE 8
#define PACKED_CAN_FRAME_HDR_SIZE \
	(sizeof(struct packed_can_frame) - PACKED_CAN_FRAME_MAX_DATA_SIZE)


/*
 * CAN frame representation suitable for transmission via network. All values
 * are in the network byte order.
 */
struct packed_can_frame {
	uint32_t can_id;
	uint8_t data[PACKED_CAN_FRAME_MAX_DATA_SIZE];
};

static void pack_can_frame(const struct can_frame *frame,
		struct packed_can_frame *packed_frame,
		size_t *packed_frame_size)
{
	packed_frame->can_id = htonl(frame->can_id);
	assert(frame->can_dlc <= PACKED_CAN_FRAME_MAX_DATA_SIZE);
	memcpy(packed_frame->data, frame->data, frame->can_dlc);
	*packed_frame_size = frame->can_dlc + PACKED_CAN_FRAME_HDR_SIZE;
}

static void unpack_can_frame(const struct packed_can_frame *packed_frame,
		size_t packed_frame_size, struct can_frame *frame)
{
	assert(packed_frame_size <= sizeof(*packed_frame));
	size_t data_size = packed_frame_size - PACKED_CAN_FRAME_HDR_SIZE;
	assert(data_size <= PACKED_CAN_FRAME_MAX_DATA_SIZE);
	frame->can_id = ntohl(packed_frame->can_id);
	frame->can_dlc = data_size;
	memcpy(frame->data, packed_frame->data, data_size);
}

struct config {
	/* Name of the CAN interface to read/write. */
	char *can_ifname;
	/* UDP port to listen for incoming CAN frames. */
	char *in_port;
	/* UDP host and port to forward CAN frames to. */
	char *out_host, *out_port;
};

/*
 * Returns a human-readable representation of a config in format
 * CAN_IFNAME:IN_PORT:OUT_HOST:OUT_PORT. Uses a statically allocated buffer.
 */
static const char *str_config(const struct config *config)
{
	static char buf[256];
	snprintf(buf, sizeof(buf), "%s:%s:%s:%s",
			config->can_ifname, config->in_port,
			config->out_host, config->out_port);
	return buf;
}

/*
 * Initializes a config from a string. The string is given in format
 * CAN_IFNAME:IN_PORT:OUT_HOST:OUT_PORT.
 */
static void parse_config(const char *config_str, struct config *config)
{
	char *end;
	char *s = xstrdup(config_str);
	config->can_ifname = s;
	end = strchr(s, ':');
	if (!end) goto fail;
	*end = '\0';
	config->in_port = s = end + 1;
	end = strchr(s, ':');
	if (!end) goto fail;
	*end = '\0';
	config->out_host = s = end + 1;
	end = strchr(s, ':');
	if (!end) goto fail;
	*end = '\0';
	config->out_port = s = end + 1;
	return;
fail:
	errx(EXIT_FAILURE, "Invalid config: Expected "
			"CAN_IFNAME:IN_PORT:OUT_HOST:OUT_PORT, got '%s'",
			config_str);
}

struct connection {
	struct config config;
	/* CAN socket fd. */
	int can_sfd;
	/* Socket fd for incoming CAN frames. */
	int in_sfd;
	/* Socket fd to forward CAN frames to. */
	int out_sfd;
};

/* Binds a socket to a CAN interface and returns its fd. */
static int bind_can(const char *ifname)
{
	int sfd;
	if ((sfd = socket(PF_CAN, SOCK_RAW, CAN_RAW)) == -1)
		err(EXIT_FAILURE, "socket");
	struct ifreq ifr;
	strcpy(ifr.ifr_name, ifname);
	if (ioctl(sfd, SIOCGIFINDEX, &ifr) == -1) {
		err(EXIT_FAILURE, "Failed to resolve CAN interface name '%s'",
				ifname);
	}
	struct sockaddr_can addr;
	addr.can_family  = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr))) {
		err(EXIT_FAILURE, "Failed to bind to CAN interface '%s'",
				ifname);
	}
	return sfd;
}

/* Binds a socket to a UDP port and returns its fd. */
static int bind_udp(const char *port)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;
	struct addrinfo *ai;
	int errcode = getaddrinfo(NULL, port, &hints, &ai);
	if (errcode != 0) {
		errx(EXIT_FAILURE, "Failed to resolve UDP port '%s': %s",
				port, gai_strerror(errcode));
	}
	int sfd = -1;
	for (struct addrinfo *rp = ai; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1)
			err(EXIT_FAILURE, "socket");
		if (bind(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;
		errcode = errno;
		close(sfd);
		sfd = -1;
	}
	freeaddrinfo(ai);
	if (sfd == -1) {
		errno =  errcode;
		err(EXIT_FAILURE, "Failed to bind to UDP port '%s'", port);
	}
	return sfd;
}

/* Connects a socket to a UDP port and returns its fd. */
int connect_udp(const char *host, const char *port)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICSERV;
	struct addrinfo *ai;
	int errcode = getaddrinfo(host, port, &hints, &ai);
	if (errcode != 0) {
		errx(EXIT_FAILURE, "Failed to resolve UDP address '%s:%s': %s",
				host, port, gai_strerror(errcode));
	}
	int sfd = -1;
	for (struct addrinfo *rp = ai; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1)
			err(EXIT_FAILURE, "socket");
		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;
		errcode = errno;
		close(sfd);
	}
	freeaddrinfo(ai);
	if (sfd == -1) {
		errno =  errcode;
		err(EXIT_FAILURE, "Failed to connect to UDP address '%s:%s'",
				host, port);
	}
	return sfd;
}

static void setup_connection(struct connection *conn)
{
	conn->can_sfd = bind_can(conn->config.can_ifname);
	conn->in_sfd = bind_udp(conn->config.in_port);
	conn->out_sfd = connect_udp(conn->config.out_host,
			conn->config.out_port);
}

/* Forwards a CAN frame from in_sfd to can_sfd. */
static void udp_to_can(struct connection *conn)
{
	ssize_t size;
	struct packed_can_frame packed_frame;
	if ((size = recv(conn->in_sfd, &packed_frame, sizeof(packed_frame),
			MSG_DONTWAIT | MSG_TRUNC)) == -1) {
		printf("%s: UDP->CAN: recv failed: %s\n",
				str_config(&conn->config), strerror(errno));
		return;
	}
	if (size < PACKED_CAN_FRAME_HDR_SIZE) {
		printf("%s: UDP->CAN: message too short: %zd < %zu\n",
				str_config(&conn->config), size,
				PACKED_CAN_FRAME_HDR_SIZE);
		return;
	}
	if (size > sizeof(packed_frame)) {
		printf("%s: UDP->CAN: message truncated: %zd->%zu\n",
				str_config(&conn->config),
				size, sizeof(packed_frame));
		size = sizeof(packed_frame);
	}
	struct can_frame frame;
	unpack_can_frame(&packed_frame, size, &frame);
	printf("%s: UDP->CAN: %s\n",
			str_config(&conn->config), str_can_frame(&frame));
	if (send(conn->can_sfd, &frame, sizeof(frame), 0) == -1) {
		printf("%s: UDP->CAN: send failed: %s\n",
				str_config(&conn->config), strerror(errno));
	}
}

/* Forwards a CAN frame from can_sfd to out_sfd. */
static void can_to_udp(struct connection *conn)
{
	struct can_frame frame;
	if (recv(conn->can_sfd, &frame, sizeof(frame), MSG_DONTWAIT) == -1) {
		printf("%s: CAN->UDP: recv failed: %s\n",
				str_config(&conn->config), strerror(errno));
		return;
	}
	printf("%s: CAN->UDP: %s\n",
			str_config(&conn->config), str_can_frame(&frame));
	size_t size;
	struct packed_can_frame packed_frame;
	pack_can_frame(&frame, &packed_frame, &size);
	if (send(conn->out_sfd, &packed_frame, size, 0) == -1) {
		printf("%s: CAN->UDP: send failed: %s\n",
				str_config(&conn->config), strerror(errno));
	}
}

int main(int argc, char *argv[])
{
	if (argc == 1) {
		errx(EXIT_FAILURE, "Usage: %s "
				"CAN_IFNAME:IN_PORT:OUT_HOST:OUT_PORT ...",
				argv[0]);
	}
	int n_connections = argc - 1;
	struct connection *connections = xmalloc(
			sizeof(*connections) * n_connections);
	struct pollfd *pfds = xmalloc(sizeof(*pfds) * n_connections * 2);
	for (int i = 0; i < n_connections; i++) {
		struct connection *conn = &connections[i];
		parse_config(argv[i + 1], &conn->config);
		setup_connection(conn);
		pfds[i * 2].fd = conn->can_sfd;
		pfds[i * 2].events = POLLIN;
		pfds[i * 2 + 1].fd = conn->in_sfd;
		pfds[i * 2 + 1].events = POLLIN;
	}
	while (1) {
		if (poll(pfds, n_connections * 2, -1) == -1)
			err(EXIT_FAILURE, "poll");
		for (int i = 0; i < n_connections * 2; i++) {
			if (!(pfds[i].revents & POLLIN))
				continue;
			struct connection *conn = &connections[i / 2];
			if (i % 2 == 0) {
				assert(pfds[i].fd == conn->can_sfd);
				can_to_udp(conn);
			} else {
				assert(pfds[i].fd == conn->in_sfd);
				udp_to_can(conn);
			}
		}
	}
	return 0;
}
