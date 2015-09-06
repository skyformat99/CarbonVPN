#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <getopt.h>
#include <errno.h>

#define BUFSIZE		2048
#define DEF_PORT	5059
#define DEF_IFNAME	"tun0"

/* Common lengths */
#define IP_HDR_LEN 20
#define ETH_HDR_LEN 14
#define ARP_PKT_LEN 28

static int debug = 0;

void do_error(char *msg, ...) {
	va_list argp;
  
	va_start(argp, msg);
	vfprintf(stderr, msg, argp);
	va_end(argp);
}

int create_socket() {
	int sock_fd = 0;
 
	if((sock_fd = socket(AF_INET, SOCK_DGRAM, 0))<0){
		do_error("Cannot create socket\n");
		return -1;
	}

 	return sock_fd;
}

int set_tun(char *dev, int flags) {
	struct ifreq ifr;
	int fd, err;

	if((fd = open("/dev/net/tun", O_RDWR)) < 0 ) {
		do_error("Cannot open interface\n");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = flags;

	if (*dev)
		strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
		do_error("Error setting interface\n");
		close(fd);
		return err;
	}

	strcpy(dev, ifr.ifr_name);
	return fd;
}

int set_ip(char *ifname, char *ip_addr) {
	struct ifreq ifr;
	struct sockaddr_in sin;
	int sock_fd = create_socket();

	sin.sin_family = AF_INET;

	inet_aton(ip_addr, (struct in_addr *)&sin.sin_addr.s_addr);

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
	memcpy(&ifr.ifr_addr, &sin, sizeof(struct sockaddr)); 

	/* Set interface address */
	if (ioctl(sock_fd, SIOCSIFADDR, &ifr)<0) {
		do_error("Error setting ip address\n");
		return -1;
	}            

	return sock_fd;
}

int set_netmask(int sock_fd, char *ifname, char *ip_addr_mask ) {
	struct ifreq ifr;

	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	memset(&ifr, 0, sizeof(ifr));
	sin->sin_family = AF_INET;
	
	inet_pton(AF_INET, ip_addr_mask, &sin->sin_addr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

	if (ioctl(sock_fd, SIOCSIFNETMASK, &ifr)<0) {
		do_error("Error setting netmask\n");
		return -1;
	}

	return sock_fd;
}

int fd_read(int fd, char *buf, int n){
	int nread;

	if((nread = read(fd, buf, n))<0){
		do_error("Cannot read descriptor\n");
		return -1;
	}
	return nread;
}

int fd_write(int fd, char *buf, int n){
	int nwrite;

	if((nwrite = write(fd, buf, n))<0){
		do_error("Cannot write to descriptor\n");
		return -1;
	}
	return nwrite;
}

int fd_count(int fd, char *buf, int n) {
	int nread, left = n;

	/* Read until buffer is 0 */
	while (left > 0) {
		if (!(nread = fd_read(fd, buf, left))){
			return 0;
		}else {
			left -= nread;
			buf += nread;
		}
	}
	return n;
}

void usage(char *name) {
	fprintf(stderr, "Usage: %s -i <interface> [-c <address>] [-p <port>] [-a] [-v]\n\n", name);
	fprintf(stderr, "Options\n");
	fprintf(stderr, "  -i <interface>  Use specific interface (Default: " DEF_IFNAME ")\n");
	fprintf(stderr, "  -c <address>    Connect to remote VPN server (Enables client mode)\n");
	fprintf(stderr, "  -p <port>       Bind to port or connect to port (Default: %u)\n", DEF_PORT);
	fprintf(stderr, "  -a              Use TAP interface (Default: TUN)\n");
	fprintf(stderr, "  -v              Verbose output\n");
	fprintf(stderr, "  -h              This help text\n");
}

int main(int argc, char *argv[]) {
	int flags = IFF_TUN;
	char if_name[IFNAMSIZ] = DEF_IFNAME;
	char remote_ip[16];
	int tap_fd, sock_fd, net_fd, option, server = 1;
	unsigned short int port = DEF_PORT;
	int header_len = IP_HDR_LEN;
	struct sockaddr_in local, remote;

	/* Check command line options */
	while((option = getopt(argc, argv, "i:c:p:ahv"))>0){
		switch(option) {
			case 'v':
				debug = 1;
				break;
			case 'h':
				usage(argv[0]);
				return 1;
			case 'i':
				strncpy(if_name, optarg, IFNAMSIZ-1);
				break;
			case 'c':
				server = 0;
				strncpy(remote_ip, optarg, 15);
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'a':
				flags = IFF_TAP;
				header_len = ETH_HDR_LEN;
				break;
			default:
				do_error("Unknown option %c\n", option);
				usage(argv[0]);
				return 1;
			}
	}

	argv += optind;
	argc -= optind;

	/* Initialize tun/tap interface */
	if ((tap_fd = set_tun(if_name, flags | IFF_NO_PI)) < 0 ) {
		do_error("Error connecting to tun/tap interface %s\n", if_name);
		return 1;
	}

	if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0))<0) {
		do_error("Error opening new socket\n");
		exit(1);
	}

	/* Client or server mode */
	if (!server) {
		/* Assign the destination address */
		memset(&remote, 0, sizeof(remote));
		remote.sin_family = AF_INET;
		remote.sin_addr.s_addr = inet_addr(remote_ip);
		remote.sin_port = htons(port);

		/* Connection request */
		if (connect(sock_fd, (struct sockaddr*)&remote, sizeof(remote))<0){
			do_error("Error connecting to server\n");
			return 1;
		}

		net_fd = sock_fd;
		printf("Connected to server %s\n", inet_ntoa(remote.sin_addr));
	} else {
		/* Server, set local addr */
		int sock = set_ip(if_name, "10.7.0.1");
		set_netmask(sock, if_name, "255.255.255.0" );

		/* Avoid EADDRINUSE */
		int _opval = 1;
		if(setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&_opval, sizeof(_opval)) < 0){
			do_error("Error setting socket\n");
			return 1;
		}

		memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = htons(port);
		if (bind(sock_fd, (struct sockaddr*)&local, sizeof(local)) < 0){
			do_error("Error cannot bind to port\n");
			return 1;
		}

		if (listen(sock_fd, 5) < 0){
			do_error("Error listen on socket\n");
			return 1;
		}

		/* Wait for request */
		socklen_t remotelen = sizeof(remote);
		memset(&remote, 0, remotelen);
		if ((net_fd = accept(sock_fd, (struct sockaddr*)&remote, &remotelen)) < 0){
			do_error("Error cannot accpet connection\n");
			return 1;
		}

		printf("Client connected from %s\n", inet_ntoa(remote.sin_addr));
	}

	int maxfd = (tap_fd > net_fd) ? tap_fd : net_fd;
	while(1) {
		char buffer[BUFSIZE];
		unsigned short nread, nwrite, plen;
		fd_set rd_set;

		FD_ZERO(&rd_set);
		FD_SET(tap_fd, &rd_set);
		FD_SET(net_fd, &rd_set);

		/* Listen on fds */
		int ret = select(maxfd + 1, &rd_set, NULL, NULL, NULL);
		if (ret < 0 && errno == EINTR){
			continue;
		}

		if (ret < 0) {
			do_error("No descriptors in queue\n");
			return 1;
		}

		/* Action on TUN */
		if(FD_ISSET(tap_fd, &rd_set)){
			nread = fd_read(tap_fd, buffer, BUFSIZE);

			printf("Read %d bytes from tun\n", nread);

			/* Write packet */
			plen = htons(nread);
			nwrite = fd_write(net_fd, (char *)&plen, sizeof(plen));
			nwrite = fd_write(net_fd, buffer, nread);

			printf("Wrote %d bytes to socket\n", nwrite);
		}

		/* Action on socket */
		if(FD_ISSET(net_fd, &rd_set)){
			nread = fd_count(net_fd, (char *)&plen, sizeof(plen));
			if(nread == 0) {
				close(net_fd);
				continue;
			}

			printf("Read %d bytes from socket\n", nread);

			/* Read packet */
			nread = fd_count(net_fd, buffer, ntohs(plen));
			nwrite = fd_write(tap_fd, buffer, nread);

			printf("Wrote %d bytes to tun\n", nwrite);
		}
	}

	return 0;
}
