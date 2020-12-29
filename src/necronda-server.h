/**
 * Necronda Web Server
 * Main executable (header file)
 * src/necronda-server.c
 * Lorenz Stechauner, 2020-12-03
 */

#ifndef NECRONDA_SERVER_NECRONDA_SERVER_H
#define NECRONDA_SERVER_NECRONDA_SERVER_H

#include <stdio.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <wait.h>
#include <sys/types.h>
#include <stdio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/engine.h>
#include <openssl/dh.h>
#include <maxminddb.h>
#include <dirent.h>


#define NUM_SOCKETS 2
#define MAX_CHILDREN 1024
#define MAX_MMDB 3
#define LISTEN_BACKLOG 16
#define REQ_PER_CONNECTION 100
#define CLIENT_TIMEOUT 3600

#define CHUNK_SIZE 4096
#define CLIENT_MAX_HEADER_SIZE 8192
#define FILE_CACHE_SIZE 1024
#define SHM_KEY 255641
#define GEOIP_MAX_SIZE 8192

#define ERR_STR "\x1B[1;31m"
#define CLR_STR "\x1B[0m"
#define BLD_STR "\x1B[1m"
#define WRN_STR "\x1B[1;33m"
#define HTTP_STR "\x1B[1;31m"
#define HTTPS_STR "\x1B[1;32m"

#define HTTP_1XX_STR "\x1B[1;32m"
#define HTTP_2XX_STR "\x1B[1;32m"
#define HTTP_3XX_STR "\x1B[1;33m"
#define HTTP_4XX_STR "\x1B[1;31m"
#define HTTP_5XX_STR "\x1B[1;31m"

#define NECRONDA_VERSION "4.0"
#define SERVER_STR "Necronda/" NECRONDA_VERSION
#define NECRONDA_ZLIB_LEVEL 9

#ifndef DEFAULT_HOST
#define DEFAULT_HOST "www.necronda.net"
#endif
#ifndef MAGIC_FILE
#define MAGIC_FILE "/usr/share/file/misc/magic.mgc"
#endif
#ifndef PHP_FPM_SOCKET
#define PHP_FPM_SOCKET "/var/run/php-fpm/php-fpm.sock"
#endif

int sockets[NUM_SOCKETS];
pid_t children[MAX_CHILDREN];
MMDB_s mmdbs[MAX_MMDB];

const char *cert_file, *key_file, *webroot_base, *geoip_dir, *dns_server;

typedef struct {
    unsigned int enc:1;
    int socket;
    SSL_CTX *ctx;
    SSL *ssl;
    char *buf;
    unsigned long buf_len;
    unsigned long buf_off;
} sock;

char *ssl_get_error(SSL *ssl, int ret);

#endif //NECRONDA_SERVER_NECRONDA_SERVER_H
