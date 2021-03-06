#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>

#ifndef __USE_BSD
#define __USE_BSD
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/limits.h>

#include <signal.h>
#include <stdarg.h>
#include <getopt.h>
#include <errno.h>
#include <sodium.h>
#include <ev.h>

#include "be.h"
#include "logger.h"
#include "conf.h"
#include "vector.h"
#include "lz4/lz4.h"
#include "util.h"

#define BUFSIZE			2048
#define CERTSIZE		32
#define ADDRSIZE		16
#define DEF_PORT		5059
#define DEF_IFNAME		"tun0"
#define DEF_ROUTER_ADDR		"10.7.0.1"
#define DEF_NETMASK		"255.255.255.0"
#define DEF_MAX_CLIENTS		20
#define PACKET_MAGIC		0xe460
#define PACKET_CNT		2048
#define DEF_HEARTBEAT_INTERVAL	1800
#define DEF_CONN_UDP 	1
#define DEF_HEARTBEAT_TIMEOUT 	2
#define DEF_LOGFILE 	"/var/log/carbonvpn.log"

EV_P;
const static unsigned char version[] = "CarbonVPN 0.8.8 - See https://github.com/yorickdewid/CarbonVPN";
static int total_clients = 0;
vector_t vector_clients;
int tap_fd;
struct ev_io w_accept, w_tun, w_dgram;

static struct sock_ev_client *conn_client = NULL;

typedef struct {
	unsigned short port;
	unsigned short server;
	char *if_name;
	char *ip;
	char *ip_netmask;
	char *logfile;
	unsigned short mtu;
	unsigned char debug;
	unsigned char max_conn;
	unsigned char daemon;
	unsigned char dgram;
	unsigned int heartbeat_interval;
	unsigned char cacert[crypto_sign_BYTES + CERTSIZE];
	unsigned char capk[crypto_sign_PUBLICKEYBYTES];
	unsigned char cask[crypto_sign_SECRETKEYBYTES];
	unsigned char pk[crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES];
	unsigned char sk[crypto_box_SECRETKEYBYTES];
} config_t;

config_t cfg;

enum mode {
	CLIENT_HELLO = 1,
	SERVER_HELLO,
	INIT_EPHEX,
	RESP_EPHEX,
	STREAM,
	PING,
	PING_BACK,
};

struct sock_ev_client {
	ev_io io;
	int net_fd;
	int index;
	struct sockaddr_in netaddr;
	unsigned char hb_cnt;
	unsigned long hladdr;
	unsigned int packet_cnt;
	unsigned char st_pk[crypto_box_PUBLICKEYBYTES];
	unsigned char st_sk[crypto_box_SECRETKEYBYTES];
	unsigned char cl_lt_pk[crypto_box_PUBLICKEYBYTES];
	unsigned char sshk[crypto_box_BEFORENMBYTES];
};

struct handshake {
	char pubkey[crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES];
	char ip[ADDRSIZE];
	char netmask[ADDRSIZE];
} __attribute__ ((packed));

struct wrapper {
	short packet_chk;
	unsigned int packet_cnt;
	unsigned short data_len;
	unsigned char mode;
	unsigned char nonce[crypto_box_NONCEBYTES];
} __attribute__ ((packed));

void usage(char *name) {
	fprintf(stderr, "Usage: %s [OPTIONS] [COMMANDS]\n", name);
	fprintf(stderr, "Options\n");
	fprintf(stderr, "  -f <file>       Read options from config file\n");
	fprintf(stderr, "  -i <interface>  Use specific interface (Default: " DEF_IFNAME ")\n");
	fprintf(stderr, "  -c <address>    Connect to remote VPN server (Enables client mode)\n");
	fprintf(stderr, "  -p <port>       Bind to port or connect to port (Default: %u)\n", DEF_PORT);
	fprintf(stderr, "  -a              Use TAP interface (Default: TUN)\n");
	fprintf(stderr, "  -d              Run daemon in background\n");
	fprintf(stderr, "  -v              Verbose output\n");
	fprintf(stderr, "  -V              Print version\n");
	fprintf(stderr, "  -h              This help text\n\n");
	fprintf(stderr, "Commands\n");
	fprintf(stderr, "  genca           Generate CA certificate\n");
	fprintf(stderr, "  gencert         Create and sign certificate\n");
	fprintf(stderr, "\n%s\n", version);
}

int parse_config(void *_pcfg, const char *section, const char *name, const char *value) {
	config_t *pcfg = (config_t*)_pcfg;

	if (!strcmp(name, "port")) {
		pcfg->port = atoi(value);
	} else if (!strcmp(name, "interface")) {
		free(pcfg->if_name);
		pcfg->if_name = c_strdup(value);
	} else if (!strcmp(name, "router")) {
		free(pcfg->ip);
		pcfg->ip = c_strdup(value);
	} else if (!strcmp(name, "netmask")) {
		free(pcfg->ip_netmask);
		pcfg->ip_netmask = c_strdup(value);
	} else if (!strcmp(name, "log")) {
		if (!strcmp(value, "false")) {
			pcfg->logfile = NULL;
		} else {
			free(pcfg->logfile);
			pcfg->logfile = c_strdup(value);
		}
	} else if (!strcmp(name, "mtu")) {
		pcfg->mtu = atoi(value);
	} else if (!strcmp(name, "heartbeat")) {
		pcfg->heartbeat_interval = atoi(value);
	} else if (!strcmp(name, "debug")) {
		pcfg->debug = value[0] == 't' ? 1 : 0;
	} else if (!strcmp(name, "daemonize")) {
		pcfg->daemon = value[0] == 't' ? 1 : 0;
	} else if (!strcmp(name, "max_clients")) {
		pcfg->max_conn = atoi(value);
	} else if (!strcmp(name, "protocol") && !strcmp(value, "tcp")) {
		pcfg->dgram = 0;
	} else if (!strcmp(name, "cacert")) {
		if (strlen(value) == (2*(crypto_sign_BYTES + CERTSIZE))) {
			hextobin(pcfg->cacert, (unsigned char *)value, crypto_sign_BYTES + CERTSIZE);
		}
	} else if (!strcmp(name, "capublickey")) {
		if (strlen(value) == (2*crypto_sign_PUBLICKEYBYTES)) {
			hextobin(pcfg->capk, (unsigned char *)value, crypto_sign_PUBLICKEYBYTES);
		}
	} else if (!strcmp(name, "caprivatekey")) {
		if (strlen(value) == (2*crypto_sign_SECRETKEYBYTES)) {
			hextobin(pcfg->cask, (unsigned char *)value, crypto_sign_SECRETKEYBYTES);
		}
	} else if (!strcmp(name, "publickey")) {
		if (strlen(value) == (2*(crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES))) {
			hextobin(pcfg->pk, (unsigned char *)value, crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES);
		}
	} else if (!strcmp(name, "privatekey")) {
		if (strlen(value) == (2*crypto_box_SECRETKEYBYTES)) {
			hextobin(pcfg->sk, (unsigned char *)value, crypto_box_SECRETKEYBYTES);
		}
	} else {
		return 0;
	}
	return 1;
}

int setnonblock(int fd) {
	int flags;

	flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags);
}

int setreuse(int sock_fd) {
	int optval = 1;

	return setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

int create_socket() {
	int sock_fd = 0;

	if((sock_fd = socket(AF_INET, SOCK_STREAM, 0))<0){
		lprint("[erro] Cannot create socket\n");
		return -1;
	}

 	return sock_fd;
}

int set_ip(char *ifname, char *ip_addr) {
	struct ifreq ifr;
	struct sockaddr_in sin;
	int sock_fd = create_socket();

	memset(&ifr, 0, sizeof(ifr));
	sin.sin_family = AF_INET;

	inet_pton(AF_INET, ip_addr, &sin.sin_addr);

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);
	memcpy(&ifr.ifr_addr, &sin, sizeof(struct sockaddr)); 

	// Set it non-blocking
	if (setnonblock(sock_fd)<0) {
		perror("echo server socket nonblock");
		return -1;
	}

	/* Set interface address */
	if (ioctl(sock_fd, SIOCSIFADDR, &ifr)<0) {
		lprint("[erro] Cannot set ip address\n");
		return -1;
	}

	if (ioctl(sock_fd, SIOCGIFFLAGS, &ifr)<0) {
		lprint("[erro] Cannot get interface\n");
		return -1;
	}

	/* Ensure the interface is up */
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(sock_fd, SIOCSIFFLAGS, &ifr)<0) {
		lprint("[erro] Cannot set interface\n");
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
		lprint("[erro] Cannot set netmask\n");
		return -1;
	}

	return sock_fd;
}

int set_mtu(int sock_fd, char *ifname, unsigned short mtu) {
	struct ifreq ifr;

	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	memset(&ifr, 0, sizeof(ifr));
	sin->sin_family = AF_INET;
	
	ifr.ifr_mtu = mtu; 
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

	if (ioctl(sock_fd, SIOCSIFMTU, &ifr)<0) {
		lprint("[erro] Cannot set MTU\n");
		return -1;
	}

	return sock_fd;
}

char *incr_ip(char *ip_addr, unsigned char increment) {
	struct sockaddr_in sin;
	static char ip[INET_ADDRSTRLEN];

	inet_pton(AF_INET, ip_addr, &sin.sin_addr);

	unsigned long nlenh = ntohl(sin.sin_addr.s_addr);
	nlenh += increment;
	sin.sin_addr.s_addr = htonl(nlenh);

	inet_ntop(AF_INET, &sin.sin_addr, ip, INET6_ADDRSTRLEN);
	return ip;
}

char *resolve_host(char *hostname) {
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_in *sin;
	static char ip[INET_ADDRSTRLEN];
	int rv;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC; // use AF_INET6 to force IPv6
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(hostname, NULL, &hints, &servinfo))<0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		lprint("[erro] Cannot resolve host\n");
		return NULL;
	}

	// Loop through all the results and connect to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		sin = (struct sockaddr_in *)p->ai_addr;
		if (sin->sin_family == AF_INET) {
			strcpy(ip , inet_ntoa(sin->sin_addr));
			break;
		}
	}

	freeaddrinfo(servinfo);
	return ip;
}

unsigned long inet_ntohl(char *ip_addr) {
	struct sockaddr_in sin;

	inet_pton(AF_INET, ip_addr, &sin.sin_addr);

	return ntohl(sin.sin_addr.s_addr);
}

int fd_read(struct sock_ev_client *client, unsigned char *buf, int n){
	int read;
	int c = 0;
	socklen_t addr_len = sizeof(client->netaddr);
	struct timespec tsp = {0, 200};

r_again:
	if (cfg.dgram) {
		read = recvfrom(client->net_fd, buf, n, 0, (struct sockaddr *)&client->netaddr, (socklen_t *)&addr_len);
	} else {
		read = recv(client->net_fd, buf, n, 0);
	}

	if (read < 0) {
		if (EAGAIN == errno || EWOULDBLOCK == errno) {
			if (c++ < 20) {
				nanosleep(&tsp, NULL);
				goto r_again;
			}
		} else {
			perror("read error");
		}
		return read;
	}

	if (read == 0) {
		close(client->net_fd);
		total_clients--;

		if (!cfg.server) {
			lprintf("[info] [client %d] Server disconnected\n", client->index);
			
			ev_break(EV_A_ EVBREAK_ALL);
		} else {
			lprintf("[info] [client %d] Disconnected\n", client->index);
			lprintf("[info] %d client(s) connected\n", total_clients);
		}
		ev_io_stop(EV_A_ &client->io);

		sodium_memzero(client->sshk, sizeof(client->sshk));
		sodium_memzero(client->st_sk, crypto_box_SECRETKEYBYTES);

		int i;
		struct sock_ev_client *vclient = NULL;
		for (i=0; i<vector_clients.size; ++i) {
			vclient = (struct sock_ev_client *)vector_get(&vector_clients, i);
			if (!vclient)
				continue;

			if (vclient->index == client->index) {
				vector_clients.data[i] = NULL;
			}
		}

		free(client);
	}
	return read;
}

int fd_write(struct sock_ev_client *client, unsigned char *buf, int n) {
	int read;
	int c = 0;
	struct timespec tsp = {0, 200};

s_again:
	if (cfg.dgram) {
		read = sendto(client->net_fd, buf, n, 0, (struct sockaddr *)&client->netaddr, sizeof(client->netaddr));
	} else {
		read = send(client->net_fd, buf, n, 0);
	}

	if (read < 0) {
		if (EAGAIN == errno || EWOULDBLOCK == errno) {
			if (c++ < 10) {
				nanosleep(&tsp, NULL);
				goto s_again;
			}
		} else {
			perror("write error");
		}
		return read;
	}

	return read;
}

void sigint_cb(EV_P_ struct ev_signal *watcher, int revents){
	syslog(LOG_NOTICE, "Starting daemon");
	lprint("[info] Shutdown daemon\n");

	int i;
	struct sock_ev_client *client = NULL;
	for (i=0; i<vector_clients.size; ++i) {
		client = (struct sock_ev_client *)vector_get(&vector_clients, i);
		if (!client)
			continue;

		ev_io_stop(EV_A_ &client->io);

		close(client->net_fd);

		sodium_memzero(client->sshk, sizeof(client->sshk));
		sodium_memzero(client->st_sk, crypto_box_SECRETKEYBYTES);

		free(client);
	}

	ev_io_stop(EV_A_ &w_accept);
	ev_io_stop(EV_A_ &w_tun);
	ev_unloop(EV_A_ EVUNLOOP_ALL);
}

/* Read client message */
void read_cb(EV_P_ struct ev_io *watcher, int revents){
	unsigned char buffer[BUFSIZE];
	unsigned char cbuffer[crypto_box_MACBYTES + BUFSIZE];
	struct wrapper encap;
	ssize_t read;
	struct sock_ev_client *client = NULL;

	if (EV_ERROR & revents) {
		perror("got invalid event");
		return;
	}

	if (cfg.dgram) {
		struct sockaddr_in addr;
		socklen_t addr_len = sizeof(addr);

		read = recvfrom(watcher->fd, (unsigned char *)&encap, sizeof(encap), 0, (struct sockaddr*)&addr, (socklen_t *)&addr_len);

		int i;
		struct sock_ev_client *vclient = NULL;
		for (i=0; i<vector_clients.size; ++i) {
			vclient = (struct sock_ev_client *)vector_get(&vector_clients, i);
			if (!vclient)
				continue;

			if ((unsigned long)vclient->netaddr.sin_addr.s_addr == (unsigned long)addr.sin_addr.s_addr && vclient->netaddr.sin_port == addr.sin_port) {
				client = vclient;
			}
		}

		if (!client) {
			if (total_clients == cfg.max_conn) {
				lprintf("[warn] Client rejected\n");
				if (cfg.debug) lprint("[dbug] Maximum number of clients reached\n");
				return;
			}

			client = (struct sock_ev_client *)calloc(1, sizeof(struct sock_ev_client));
			client->netaddr = addr;
			client->packet_cnt = PACKET_CNT;
			client->net_fd = watcher->fd;
			client->index = ++total_clients;
			lprint("[info] Successfully connected with client\n");
			lprintf("[info] %d client(s) connected\n", total_clients);

			vector_append(&vector_clients, (void *)client);
		}
	} else {
		client = (struct sock_ev_client *)watcher;

		read = fd_read(client, (unsigned char *)&encap, sizeof(encap));
	}

	if (read <= 0)
		return;

	client->hb_cnt = DEF_HEARTBEAT_TIMEOUT;

	int sesscnt = ntohl(encap.packet_cnt);
	if (cfg.debug) lprintf("[dbug] [client %d] Packet count %u\n", client->index, sesscnt);

	if (ntohs(encap.packet_chk) != PACKET_MAGIC) {
		if (cfg.debug) lprintf("[dbug] [client %d] Invalid packet, packet dropped\n", client->index);
		return;
	}

	if (cfg.debug) lprintf("[dbug] [client %d] Read %d bytes from socket\n", client->index, read);

	switch (encap.mode) {
		case CLIENT_HELLO: {
			struct handshake client_key;
			read = fd_read(client, (unsigned char *)&client_key, sizeof(client_key));

			unsigned char pk_unsigned[crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES];
			unsigned long long pk_unsigned_len;
			if (crypto_sign_open(pk_unsigned, &pk_unsigned_len, (const unsigned char *)client_key.pubkey, crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES, cfg.capk) != 0) {
				lprintf("[erro] [client %d] Authentication mismatch\n", client->index);
			} else {
				lprintf("[info] [client %d] Authentication verified\n", client->index);
			
				unsigned char ca_fp[crypto_generichash_BYTES];
				unsigned char cl_fp[crypto_generichash_BYTES];
				memcpy(client->cl_lt_pk, pk_unsigned, crypto_box_PUBLICKEYBYTES);
				memcpy(cl_fp, pk_unsigned+crypto_box_PUBLICKEYBYTES, crypto_generichash_BYTES);

				crypto_generichash(ca_fp, crypto_generichash_BYTES, cfg.cacert, (crypto_sign_BYTES + CERTSIZE), cfg.capk, crypto_sign_PUBLICKEYBYTES);

				if (!memcmp(ca_fp, cl_fp, crypto_generichash_BYTES)) {
					lprintf("[info] [client %d] Signature verified\n", client->index);

					encap.packet_chk = htons(PACKET_MAGIC);
					encap.packet_cnt = htonl(client->packet_cnt--);
					encap.data_len = 0;
					encap.mode = SERVER_HELLO;

					memcpy(client_key.pubkey, cfg.pk, crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES);
					strncpy(client_key.ip, incr_ip(cfg.ip, client->index), 15);
					strncpy(client_key.netmask, cfg.ip_netmask, 15);
					client->hladdr = inet_ntohl(client_key.ip);

					fd_write(client, (unsigned char *)&encap, sizeof(encap));
					fd_write(client, (unsigned char *)&client_key, sizeof(client_key));

					lprintf("[info] [client %d] Assigned %s\n", client->index, client_key.ip);
				} else {
					lprintf("[erro] [client %d] Signature mismatch\n", client->index);
				}
			}
			break;
		}
		case SERVER_HELLO: {
			struct handshake client_key;
			read = fd_read(client, (unsigned char *)&client_key, sizeof(client_key));

			unsigned char pk_unsigned[crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES];
			unsigned long long pk_unsigned_len;
			if (crypto_sign_open(pk_unsigned, &pk_unsigned_len, (const unsigned char *)client_key.pubkey, crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES, cfg.capk) != 0) {
				lprintf("[erro] [client %d] Server authentication mismatch\n", client->index);
			} else {
				lprintf("[info] [client %d] Server authentication verified\n", client->index);
			
				unsigned char ca_fp[crypto_generichash_BYTES];
				unsigned char cl_fp[crypto_generichash_BYTES];
				memcpy(client->cl_lt_pk, pk_unsigned, crypto_box_PUBLICKEYBYTES);
				memcpy(cl_fp, pk_unsigned+crypto_box_PUBLICKEYBYTES, crypto_generichash_BYTES);

				crypto_generichash(ca_fp, crypto_generichash_BYTES, cfg.cacert, (crypto_sign_BYTES + CERTSIZE), cfg.capk, crypto_sign_PUBLICKEYBYTES);

				if (!memcmp(ca_fp, cl_fp, crypto_generichash_BYTES)) {
					lprintf("[info] [client %d] Server signature verified\n", client->index);

					unsigned char nonce[crypto_box_NONCEBYTES];
					unsigned char ciphertext[crypto_box_MACBYTES + crypto_box_PUBLICKEYBYTES];
					randombytes_buf(nonce, crypto_box_NONCEBYTES);
					crypto_box_keypair(client->st_pk, client->st_sk);

					client->packet_cnt = PACKET_CNT;
					crypto_box_easy(ciphertext, client->st_pk, crypto_box_PUBLICKEYBYTES, nonce, client->cl_lt_pk, cfg.sk);

					encap.packet_chk = htons(PACKET_MAGIC);
					encap.packet_cnt = htonl(client->packet_cnt--);
					encap.data_len = 0;
					encap.mode = INIT_EPHEX;
					memcpy(encap.nonce, nonce, crypto_box_NONCEBYTES);

					fd_write(client, (unsigned char *)&encap, sizeof(encap));
					fd_write(client, ciphertext, sizeof(ciphertext));

					int sock = set_ip(cfg.if_name, client_key.ip);
					set_netmask(sock, cfg.if_name, client_key.netmask);
					if (cfg.mtu)
						set_mtu(sock, cfg.if_name, cfg.mtu);
					client->hladdr = inet_ntohl(client_key.ip);

					lprintf("[info] [client %d] Assgined %s/%s\n", client->index, client_key.ip, client_key.netmask);
				} else {
					lprintf("[erro] [client %d] Server signature mismatch\n", client->index);
				}
			}
			break;
		}
		case INIT_EPHEX: {
			unsigned char cl_st_pk[crypto_box_PUBLICKEYBYTES];
			unsigned char ciphertext[crypto_box_MACBYTES + crypto_box_PUBLICKEYBYTES];
			read = fd_read(client, (unsigned char *)&ciphertext, sizeof(ciphertext));

			if (crypto_box_open_easy(cl_st_pk, ciphertext, crypto_box_MACBYTES + crypto_box_PUBLICKEYBYTES, encap.nonce, client->cl_lt_pk, cfg.sk) != 0) {
				if (cfg.debug) lprintf("[dbug] [client %d] Ephemeral key exchange failed\n", client->index);
			} else {
				lprintf("[info] [client %d] Ephemeral key exchanged\n", client->index);

				unsigned char nonce[crypto_box_NONCEBYTES];
				randombytes_buf(nonce, crypto_box_NONCEBYTES);
				crypto_box_keypair(client->st_pk, client->st_sk);

				client->packet_cnt = PACKET_CNT;
				crypto_box_beforenm(client->sshk, cl_st_pk, client->st_sk);
				crypto_box_easy(ciphertext, client->st_pk, crypto_box_PUBLICKEYBYTES, nonce, client->cl_lt_pk, cfg.sk);

				encap.packet_chk = htons(PACKET_MAGIC);
				encap.packet_cnt = htonl(client->packet_cnt--);
				encap.data_len = 0;
				encap.mode = RESP_EPHEX;
				memcpy(encap.nonce, nonce, crypto_box_NONCEBYTES);

				fd_write(client, (unsigned char *)&encap, sizeof(encap));
				fd_write(client, ciphertext, sizeof(ciphertext));
			}
			break;
		}
		case RESP_EPHEX: {
			unsigned char cl_st_pk[crypto_box_PUBLICKEYBYTES];
			unsigned char ciphertext[crypto_box_MACBYTES + crypto_box_PUBLICKEYBYTES];
			read = fd_read(client, (unsigned char *)&ciphertext, sizeof(ciphertext));

			if (crypto_box_open_easy(cl_st_pk, ciphertext, crypto_box_MACBYTES + crypto_box_PUBLICKEYBYTES, encap.nonce, client->cl_lt_pk, cfg.sk) != 0) {
				if (cfg.debug) lprintf("[dbug] [client %d] Ephemeral key exchange failed\n", client->index);
			} else {
				lprintf("[info] [client %d] Ephemeral key exchanged\n", client->index);

				crypto_box_beforenm(client->sshk, cl_st_pk, client->st_sk);
			}
			break;
		}
		case STREAM: {
			read = fd_read(client, (unsigned char *)&cbuffer, ntohs(encap.data_len));
			if (read <= 0)
				return;

			if (crypto_box_open_easy_afternm(buffer, cbuffer, ntohs(encap.data_len), encap.nonce, client->sshk) != 0) {
				if (cfg.debug) lprintf("[dbug] [client %d] Unable to decrypt packet\n", client->index);
			} else {
				int nwrite;

				if((nwrite = write(tap_fd, buffer, read))<0){
					lprintf("[warn] [client %d] Cannot write device\n", client->index);
					return;
				}

				if (cfg.debug) lprintf("[dbug] [client %d] Wrote %d bytes to tun\n", client->index, nwrite);
			}
			break;
		}
		case PING: {
			encap.packet_chk = htons(PACKET_MAGIC);
			encap.packet_cnt = htonl(client->packet_cnt--);
			encap.data_len = 0;
			encap.mode = PING_BACK;

			fd_write(client, (unsigned char *)&encap, sizeof(encap));
			break;
		}
		case PING_BACK: {
			lprintf("[info] [client %d] Pingback heartbeat alive\n", client->index);
			break;
		}
		default:
			if (cfg.debug) lprintf("[dbug] [client %d] Request unknown, packet dropped\n", client->index);

	}

	if (sesscnt == 1) {
		lprintf("[info] [client %d] Ephemeral keypair expired\n", client->index);

		unsigned char nonce[crypto_box_NONCEBYTES];
		unsigned char ciphertext[crypto_box_MACBYTES + crypto_box_PUBLICKEYBYTES];
		randombytes_buf(nonce, crypto_box_NONCEBYTES);
		crypto_box_keypair(client->st_pk, client->st_sk);

		client->packet_cnt = PACKET_CNT;
		crypto_box_easy(ciphertext, client->st_pk, crypto_box_PUBLICKEYBYTES, nonce, client->cl_lt_pk, cfg.sk);

		encap.packet_chk = htons(PACKET_MAGIC);
		encap.packet_cnt = htonl(client->packet_cnt--);
		encap.data_len = 0;
		encap.mode = INIT_EPHEX;
		memcpy(encap.nonce, nonce, crypto_box_NONCEBYTES);

		fd_write(client, (unsigned char *)&encap, sizeof(encap));
		fd_write(client, ciphertext, sizeof(ciphertext));
	}
}

/* Accept client requests */
void tun_cb(EV_P_ struct ev_io *watcher, int revents) {
	unsigned char buffer[BUFSIZE];
	unsigned char cbuffer[crypto_box_MACBYTES + BUFSIZE];
	short nread;
	int tap = 0;

	if((nread = read(watcher->fd, buffer, BUFSIZE))<0){
		lprint("[warn] Cannot read device\n");
		return;
	}

	if (cfg.debug) lprintf("[dbug] Read %d bytes from tun\n", nread);

	struct tun_pi *pi = (struct tun_pi *)buffer;
	struct ip *iphdr = (struct ip *)(buffer+sizeof(struct tun_pi));

	if ((pi->flags & IFF_TAP) == IFF_TAP)
		tap = 1;

	int i;
	struct sock_ev_client *client = NULL;
	for (i=0; i<vector_clients.size; ++i) {
		client = (struct sock_ev_client *)vector_get(&vector_clients, i);
		if (!client)
			continue;

		unsigned long client_addr = (unsigned long)htonl(client->hladdr);
		if (client_addr != (unsigned long)iphdr->ip_src.s_addr && cfg.server && !tap)
			continue;

		unsigned char nonce[crypto_box_NONCEBYTES];
		randombytes_buf(nonce, crypto_box_NONCEBYTES);
		crypto_box_easy_afternm(cbuffer, buffer, nread, nonce, client->sshk);

		struct wrapper encap;
		encap.packet_chk = htons(PACKET_MAGIC);
		encap.packet_cnt = htonl(client->packet_cnt--);
		encap.data_len = htons(crypto_box_MACBYTES + nread);
		encap.mode = STREAM;
		memcpy(encap.nonce, nonce, crypto_box_NONCEBYTES);

		fd_write(client, (unsigned char *)&encap, sizeof(encap));
		fd_write(client, cbuffer, crypto_box_MACBYTES + nread);

		if (cfg.debug) lprintf("[dbug] [client %d] Wrote %d bytes to socket\n", client->index, crypto_box_MACBYTES + nread);
		
	}
}

int tun_init(char *dev, int flags) {
	struct ifreq ifr;
	int fd, err;

	if((fd = open("/dev/net/tun", O_RDWR)) < 0 ) {
		lprint("[erro] Cannot create interface\n");
		return -1;
	}

	if (setnonblock(fd)<0) {
		lprint("[erro] Cannot set nonblock\n");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = flags;

	if (*dev)
		strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
		lprint("[erro] Cannot set interface\n");
		close(fd);
		return err;
	}

	// Initialize and start watcher to read tun interface
	ev_io_init(&w_tun, tun_cb, fd, EV_READ);
	ev_io_start(EV_A_ &w_tun);

	strcpy(dev, ifr.ifr_name);
	return fd;
}

/* Accept client requests */
void accept_cb(EV_P_ struct ev_io *watcher, int revents) {
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int sd;
	struct sock_ev_client *client = (struct sock_ev_client *)calloc(1, sizeof(struct sock_ev_client));
	
	if (EV_ERROR & revents) {
		perror("got invalid event");
		return;
	}

	if (total_clients == cfg.max_conn) {
		lprintf("[warn] Client rejected\n");
		if (cfg.debug) lprint("[dbug] Maximum number of clients reached\n");
		free(client);
		return;
	}

	// Accept client request
	sd = accept(watcher->fd, (struct sockaddr *)&addr, &addr_len);
	if (sd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			perror("accept error");
			free(client);
			return;
		}
	}

	client->netaddr = addr;
	client->packet_cnt = PACKET_CNT;
	client->net_fd = sd;
	client->index = ++total_clients;
	lprint("[info] Successfully connected with client\n");
	lprintf("[info] %d client(s) connected\n", total_clients);

	vector_append(&vector_clients, (void *)client);

	// Initialize and start watcher to read client requests
	ev_io_init(&client->io, read_cb, sd, EV_READ);
	ev_io_start(EV_A_ &client->io);
}

int client_connect(EV_P_ char *remote_addr) {
	int sd;
	static unsigned int retry_ping = 3;
 	struct sockaddr_in remote;
 	struct wrapper encap;
 	conn_client = (struct sock_ev_client *)calloc(1, sizeof(struct sock_ev_client));

 	// Create client socket
	if ((sd = socket(AF_INET, cfg.dgram ? SOCK_DGRAM : SOCK_STREAM, 0))<0){
		perror("socket error");
		return -1;
	}

	conn_client->packet_cnt = PACKET_CNT;
	conn_client->net_fd = sd;
	conn_client->index = 0;

	if (setnonblock(conn_client->net_fd)<0) {
		lprint("[erro] Cannot set nonblock\n");
		return -1;
	}

	// Initialize the send callback, but wait to start until there is data to write
	ev_io_init(&conn_client->io, read_cb, sd, EV_READ);
	ev_io_start(EV_A_ &conn_client->io);

	memset(&remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_port = htons(DEF_PORT);
	remote.sin_addr.s_addr = inet_addr(remote_addr);
	conn_client->netaddr = remote;

	if (!cfg.dgram) {
		int res = connect(conn_client->net_fd, (struct sockaddr *)&remote, sizeof(remote));
		if (res < 0) {
			if (errno != EINPROGRESS) {
				perror("connect error");
				return -1;
			}
		}
		lprintf("[info] Connected to server %s\n", inet_ntoa(remote.sin_addr));
	} else {
		lprintf("[info] Using stateless connection\n");
	}

	vector_append(&vector_clients, (void *)conn_client);

retry:
	encap.packet_chk = htons(PACKET_MAGIC);
	encap.packet_cnt = htonl(conn_client->packet_cnt--);
	encap.data_len = 0;
	encap.mode = PING;

	if (fd_write(conn_client, (unsigned char *)&encap, sizeof(encap))<0) {
		retry_ping--;
		if (retry_ping>0) {
			lprint("[warn] Retry pingback\n");
			goto retry;
		}
		return -1;
	}

	encap.packet_chk = htons(PACKET_MAGIC);
	encap.packet_cnt = htonl(conn_client->packet_cnt--);
	encap.data_len = 0;
	encap.mode = CLIENT_HELLO;

	struct handshake client_key;
	memcpy(client_key.pubkey, cfg.pk, crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES);

	fd_write(conn_client, (unsigned char *)&encap, sizeof(encap));
	fd_write(conn_client, (unsigned char *)&client_key, sizeof(client_key));

  	return sd;
}

int stream_server_init() {
	int sd;
	struct sockaddr_in addr;

	// Create server socket
	if ((sd = socket(AF_INET, SOCK_STREAM, 0))<0){
		perror("socket error");
		return -1;
	}

	// Set sock non-blocking
	if (setnonblock(sd)<0) {
		perror("nonblock error");
		return -1;
	}

	// Reuse the socket options
	if (setreuse(sd)<0) {
		perror("reuse error");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(DEF_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	// Bind socket to address
	if (bind(sd, (struct sockaddr*)&addr, sizeof(addr))<0) {
		perror("bind error");
		return -1;
	}

	// Start listing on the socket
	if (listen(sd, cfg.max_conn)<0) {
		perror("listen error");
		return -1;
	}

	return sd;
}

int connless_server_init() {
	int sd;
	struct sockaddr_in addr;

	// Create server socket
	if ((sd = socket(AF_INET, SOCK_DGRAM, 0))<0){
		perror("socket error");
		return -1;
	}

	// Reuse the socket options
	if (setreuse(sd)<0) {
		perror("reuse error");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(DEF_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	// Bind socket to address
	if (bind(sd, (struct sockaddr*)&addr, sizeof(addr))<0) {
		perror("bind error");
		return -1;
	}

	return sd;
}

/* Heartbeat to keep connection alive */
void ping_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
	int i;
	struct wrapper encap;
	struct sock_ev_client *client = NULL;

	if (vector_clients.size > cfg.max_conn && (vector_clients.size/2) > total_clients) {
		vector_rebuild(&vector_clients, cfg.max_conn);
		if (cfg.debug) lprintf("[dbug] Rebuilt client pool\n");
	}

	for (i=0; i<vector_clients.size; ++i) {
		client = (struct sock_ev_client *)vector_get(&vector_clients, i);
		if (!client)
			continue;

		if (!client->hb_cnt) {
			if (!cfg.dgram) {
				close(client->net_fd);
			}
			total_clients--;

			lprintf("[info] [client %d] Dequeued due to timeout\n", client->index);
			lprintf("[info] %d client(s) connected\n", total_clients);
			ev_io_stop(EV_A_ &client->io);

			sodium_memzero(client->sshk, sizeof(client->sshk));
			sodium_memzero(client->st_sk, crypto_box_SECRETKEYBYTES);

			vector_clients.data[i] = NULL;

			free(client);
			continue;
		}

		if (cfg.debug) lprintf("[dbug] [client %d] Heartbeat timeout %d\n", client->index, client->hb_cnt);
		encap.packet_chk = htons(PACKET_MAGIC);
		encap.packet_cnt = htonl(client->packet_cnt--);
		encap.data_len = 0;
		encap.mode = PING;
		client->hb_cnt--;

		lprintf("[info] [client %d] Sending ping heartbeat\n", client->index);
		if (fd_write(client, (unsigned char *)&encap, sizeof(encap))<0) {
			lprintf("[warn] [client %d] Pingback failed\n", client->index);
		}
	}
}

/* Generate new CA */
void cert_genca() {
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	unsigned char sk[crypto_sign_SECRETKEYBYTES];
	unsigned char cert[CERTSIZE];
	unsigned char cert_signed[crypto_sign_BYTES + CERTSIZE];
	unsigned char fp[crypto_generichash_BYTES];
	unsigned long long cert_signed_len;

	randombytes_buf(cert, CERTSIZE);
	crypto_sign_keypair(pk, sk);
	crypto_sign(cert_signed, &cert_signed_len, cert, CERTSIZE, sk);
	crypto_generichash(fp, crypto_generichash_BYTES, cert_signed, cert_signed_len, pk, crypto_sign_PUBLICKEYBYTES);

	if (cfg.debug) {
		printf("Generating CA with %s-%s-SHA256\n", randombytes_implementation_name(), crypto_sign_primitive());
		printf("Private certificate: \t");
		print_hex(cert, CERTSIZE);
		printf("Public key: \t\t");
		print_hex(pk, crypto_sign_PUBLICKEYBYTES);
		printf("Private key: \t\t");
		print_hex(sk, crypto_sign_SECRETKEYBYTES);
		printf("Public certificate: \t");
		print_hex(cert_signed, cert_signed_len);
		printf("Fingerprint: \t\t");
		print_hex(fp, crypto_generichash_BYTES);
		putchar('\n');
	}

	puts("Add the following lines the config file:");
	printf("cacert = ");
	print_hex(cert_signed, cert_signed_len);
	printf("capublickey = ");
	print_hex(pk, crypto_sign_PUBLICKEYBYTES);
	printf("caprivatekey = ");
	print_hex(sk, crypto_sign_SECRETKEYBYTES);

	sodium_memzero(cert, sizeof(cert));
	sodium_memzero(sk, sizeof(sk));
}

/* Generate new client keypair */
void cert_gencert() {
	unsigned char pk[crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES];
	unsigned char sk[crypto_box_SECRETKEYBYTES];
	unsigned char fp[crypto_generichash_BYTES];
	unsigned char pk_signed[crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES];
	unsigned long long pk_signed_len;
	char q;

	if (isnull(cfg.cacert, crypto_sign_BYTES + CERTSIZE)) {
		lprintf("[erro] No CA certificate in config, see genca\n");
		return;
	}

	if (isnull(cfg.capk, crypto_sign_PUBLICKEYBYTES)) {
		lprintf("[erro] No CA public key in config, see genca\n");
		return;
	}

	if (isnull(cfg.cask, crypto_sign_SECRETKEYBYTES)) {
		lprintf("[erro] No CA private key in config, see genca\n");
		return;
	}

	crypto_generichash(fp, crypto_generichash_BYTES, cfg.cacert, (crypto_sign_BYTES + CERTSIZE), cfg.capk, crypto_sign_PUBLICKEYBYTES);
	crypto_box_keypair(pk, sk);
	memcpy(pk+crypto_box_PUBLICKEYBYTES, fp, crypto_generichash_BYTES);

	printf("Public key: ");
	print_hex(pk, crypto_box_PUBLICKEYBYTES);
	printf("Sign key with CA [y/N]? ");
	scanf("%c", &q);
	if (q != 'Y' && q != 'y') {
		return;
	}

	crypto_sign(pk_signed, &pk_signed_len, pk, crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES, cfg.cask);

	if (cfg.debug) {
		printf("Generating keypair with %s\n", crypto_box_primitive());
		printf("Appended public key: \t");
		print_hex(pk, crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES);
		printf("Private key: \t\t");
		print_hex(sk, crypto_box_SECRETKEYBYTES);
		printf("Fingerprint: \t\t");
		print_hex(fp, crypto_generichash_BYTES);
		printf("Signed public key: \t");
		print_hex(pk_signed, pk_signed_len);
		putchar('\n');
	}

	puts("Add the following lines the config file:");
	printf("publickey = ");
	print_hex(pk_signed, pk_signed_len);
	printf("privatekey = ");
	print_hex(sk, crypto_box_SECRETKEYBYTES);

	sodium_memzero(sk, sizeof(sk));
}

int ev_start_loop(char *remote_addr, int flags) {
	int sock_fd;
	struct ev_signal w_signal;
	struct ev_periodic i_ping;

	// Follow exit routine
	ev_signal_init(&w_signal, sigint_cb, SIGINT);
	ev_signal_start(EV_A_ &w_signal);

	ev_signal_init(&w_signal, sigint_cb, SIGTERM);
	ev_signal_start(EV_A_ &w_signal);

	ev_signal_init(&w_signal, sigint_cb, SIGUSR1);
	ev_signal_start(EV_A_ &w_signal);

	ev_signal_init(&w_signal, sigint_cb, SIGHUP);
	ev_signal_start(EV_A_ &w_signal);

	// Initialize client pool
	vector_init(&vector_clients, cfg.max_conn);

	/* Initialize tun/tap interface */
	tap_fd = tun_init(cfg.if_name, flags | IFF_NO_PI);

	/* Client or server mode */
	if (!cfg.server) {
		/* Resolve hostname */
		remote_addr = resolve_host(remote_addr);

		/* Assign the destination address */
		int res = client_connect(EV_A_ remote_addr);
		if (res < 0)
			return -1;
	} else {
		/* Server, set local addr */
		int sock = set_ip(cfg.if_name, cfg.ip);
		set_netmask(sock, cfg.if_name, cfg.ip_netmask);

		if (cfg.mtu)
			set_mtu(sock, cfg.if_name, cfg.mtu);

		if (cfg.dgram) {
			// datagram connection
			lprint("[info] Using stateless connections\n");
			sock_fd = connless_server_init();
			if (sock_fd < 0)
				return -1;

			// Stateless connections need smaller heartbeat
			if (cfg.heartbeat_interval) {
				cfg.heartbeat_interval = cfg.heartbeat_interval/2;
			}

			ev_io_init(&w_dgram, read_cb, sock_fd, EV_READ);
			ev_io_start(EV_A_ &w_dgram);
		} else {
			// stateful connection
			lprint("[info] Using stateful connections\n");
			sock_fd = stream_server_init();
			if (sock_fd < 0)
				return -1;

			// Initialize and start a watcher to accepts client requests
			ev_io_init(&w_accept, accept_cb, sock_fd, EV_READ);
			ev_io_start(EV_A_ &w_accept);
		}

		// Periodic check on clients
		if (cfg.heartbeat_interval) {
			ev_periodic_init(&i_ping, ping_cb, 0., cfg.heartbeat_interval, 0);
			ev_periodic_start(EV_A_ &i_ping);
		}
	}

	// Start infinite loop
	lprint("[info] Starting events\n");
	ev_loop(EV_A_ 0);

	return 0;
}

int daemonize(char *remote_addr, int flags) {
	pid_t pid, sid;

	syslog(LOG_NOTICE, "Starting daemon");
	lprintf("[info] Starting daemon in background\n");
	pid = fork();
	if (pid < 0) {
		lprintf("[erro] Failed to fork into background\n");
		return 1;
	}

	if (pid > 0) {
		return 0;
	}
	umask(0);

	sid = setsid();
	if (sid < 0) {
		lprintf("[erro] Failed to promote to session leader\n");
		return 1;
	}

#if SECURE_CHROOT
	if ((chdir("/")) < 0) {
			lprintf("[erro] Failed to change directory\n");
			return 1;
	}
#endif

	// Only log to file
	log_tty(0);

	// Daemon initialization
	ev_start_loop(remote_addr, flags);
	return 0;
}

int main(int argc, char *argv[]) {
	int flags = IFF_TUN;
	char remote_ip[ADDRSIZE];
	char config_file[NAME_MAX];
	int option, config = 0;
	loop = EV_DEFAULT;

	memset(&cfg, 0, sizeof(config_t));
	
	cfg.server = 1;
	cfg.port = DEF_PORT;
	cfg.if_name = c_strdup(DEF_IFNAME);
	cfg.ip = c_strdup(DEF_ROUTER_ADDR);
	cfg.ip_netmask = c_strdup(DEF_NETMASK);
	cfg.debug = 0;
	cfg.max_conn = DEF_MAX_CLIENTS;
	cfg.daemon = 0;
	cfg.dgram = DEF_CONN_UDP;
	cfg.logfile = c_strdup(DEF_LOGFILE);
	cfg.heartbeat_interval = DEF_HEARTBEAT_INTERVAL;

	// Start log
	setlogmask(LOG_UPTO(LOG_NOTICE));
	openlog(argv[0], LOG_CONS | LOG_PID, LOG_USER);

	// Initialize NaCl
	if (sodium_init()<0)
		goto cleanup;

	/* Check command line options */
	while ((option = getopt(argc, argv, "f:i:c:p:ahvVd"))>0){
		switch (option) {
			case 'd':
				cfg.daemon = 1;
				break;
			case 'v':
				cfg.debug = 1;
				break;
			case 'V':
				puts((char *)version);
				return 1;
			case 'h':
				usage(argv[0]);
				return 1;
			case 'i':
				free(cfg.if_name);
				cfg.if_name = c_strdup(optarg);
				break;
			case 'c':
				cfg.server = 0;
				strncpy(remote_ip, optarg, ADDRSIZE-1);
				break;
			case 'p':
				cfg.port = atoi(optarg);
				break;
			case 'a':
				flags = IFF_TAP;
				break;
			case 'f':
				config = 1;
				strncpy(config_file, optarg, NAME_MAX-1);
				break;
			default:
				fprintf(stderr, "Unknown option %c\n", option);
				usage(argv[0]);
				goto cleanup;
			}
	}

	argv += optind;
	argc -= optind;

	/* Parse config */
	if (config) {
		lprintf("[info] Loading config from %s\n", config_file);
		if (conf_parse(config_file, parse_config, &cfg) < 0) {
			lprintf("[erro] Cannot open %s\n", config_file);
			goto cleanup;
		}
	}

	if (argc > 0) {
		if (!strcmp(argv[0], "genca")) {
			cert_genca();
			goto cleanup;
		} else if (!strcmp(argv[0], "gencert")) {
			cert_gencert();
			goto cleanup;
		} else {
			fprintf(stderr, "Unknown command %s\n", argv[0]);
			goto cleanup;
		}
	}

	if (isnull(cfg.cacert, crypto_sign_BYTES + CERTSIZE)) {
		lprintf("[erro] No CA certificate in config, see genca\n");
		goto cleanup;
	}

	if (isnull(cfg.capk, crypto_sign_PUBLICKEYBYTES)) {
		lprintf("[erro] No CA public key in config, see genca\n");
		goto cleanup;
	}

	if (isnull(cfg.pk, crypto_sign_BYTES + crypto_box_PUBLICKEYBYTES + crypto_generichash_BYTES)) {
		lprintf("[erro] No client public key in config, see gencert\n");
		goto cleanup;
	}

	if (isnull(cfg.sk, crypto_box_SECRETKEYBYTES)) {
		lprintf("[erro] No client private key in config, see gencert\n");
		goto cleanup;
	}

	if (cfg.logfile)
		start_log(cfg.logfile);

	if (cfg.daemon)
		daemonize(remote_ip, flags);
	else
		ev_start_loop(remote_ip, flags);

cleanup:
	ev_loop_destroy(loop);

	free(cfg.if_name);
	free(cfg.ip);
	free(cfg.ip_netmask);
	free(cfg.logfile);

	vector_free(&vector_clients);

	stop_log();
	closelog();

	return 0;
}
