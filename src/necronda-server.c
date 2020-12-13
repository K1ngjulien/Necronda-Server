/**
 * Necronda Web Server
 * Main executable
 * src/necronda-server.c
 * Lorenz Stechauner, 2020-12-03
 */

#define _POSIX_C_SOURCE 199309L

#include "necronda-server.h"

#include "utils.c"
#include "uri.c"
#include "net/http.c"
#include "client.c"


int active = 1;


void openssl_init() {
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();
}

char *ssl_get_error(SSL *ssl, int ret) {
    if (ret > 0) {
        return NULL;
    }

    unsigned long ret2 = ERR_get_error();
    char *err2 = strerror(errno);
    char *err1 = (char *) ERR_reason_error_string(ret2);

    switch (SSL_get_error(ssl, ret)) {
        case SSL_ERROR_NONE:
            return "none";
        case SSL_ERROR_ZERO_RETURN:
            return "closed";
        case SSL_ERROR_WANT_READ:
            return "want read";
        case SSL_ERROR_WANT_WRITE:
            return "want write";
        case SSL_ERROR_WANT_CONNECT:
            return "want connect";
        case SSL_ERROR_WANT_ACCEPT:
            return "want accept";
        case SSL_ERROR_WANT_X509_LOOKUP:
            return "want x509 lookup";
        case SSL_ERROR_SYSCALL:
            return ((ret2 == 0) ? ((ret == 0) ? "protocol violation" : err2) : err1);
        case SSL_ERROR_SSL:
            return err1;
        default:
            return "unknown error";
    }
}

void destroy() {
    fprintf(stderr, "\n" ERR_STR "Terminating forcefully!" CLR_STR "\n");
    int status = 0;
    int ret;
    int kills = 0;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (CHILDREN[i] != 0) {
            ret = waitpid(CHILDREN[i], &status, WNOHANG);
            if (ret < 0) {
                fprintf(stderr, ERR_STR "Unable to wait for child process (PID %i): %s" CLR_STR "\n", CHILDREN[i], strerror(errno));
            } else if (ret == CHILDREN[i]) {
                CHILDREN[i] = 0;
            } else {
                kill(CHILDREN[i], SIGKILL);
                kills++;
            }
        }
    }
    if (kills > 0) {
        fprintf(stderr, ERR_STR "Killed %i child process(es)" CLR_STR "\n", kills);
    }
    exit(2);
}

void terminate() {
    fprintf(stderr, "\nTerminating gracefully...\n");
    active = 0;

    signal(SIGINT, destroy);
    signal(SIGTERM, destroy);

    for (int i = 0; i < NUM_SOCKETS; i++) {
        shutdown(SOCKETS[i], SHUT_RDWR);
        close(SOCKETS[i]);
    }

    int status = 0;
    int wait_num = 0;
    int ret;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (CHILDREN[i] != 0) {
            ret = waitpid(CHILDREN[i], &status, WNOHANG);
            if (ret < 0) {
                fprintf(stderr, ERR_STR "Unable to wait for child process (PID %i): %s" CLR_STR "\n", CHILDREN[i], strerror(errno));
            } else if (ret == CHILDREN[i]) {
                CHILDREN[i] = 0;
            } else {
                kill(CHILDREN[i], SIGTERM);
                wait_num++;
            }
        }
    }

    if (wait_num > 0) {
        fprintf(stderr, "Waiting for %i child process(es)...\n", wait_num);
    }

    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (CHILDREN[i] != 0) {
            ret = waitpid(CHILDREN[i], &status, 0);
            if (ret < 0) {
                fprintf(stderr, ERR_STR "Unable to wait for child process (PID %i): %s" CLR_STR "\n", CHILDREN[i], strerror(errno));
            } else if (ret == CHILDREN[i]) {
                CHILDREN[i] = 0;
            }
        }
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    if (wait_num > 0) {
        sleep(1);
        fprintf(stderr, "\nGoodbye\n");
    } else {
        fprintf(stderr, "Goodbye\n");
    }
    exit(0);
}

int main(int argc, const char *argv[]) {
    const int YES = 1;
    fd_set socket_fds, read_socket_fds;
    int max_socket_fd = 0;
    int ready_sockets_num = 0;
    long client_num = 0;

    int client_fd;
    sock client;
    struct sockaddr_in6 client_addr;
    unsigned int client_addr_len = sizeof(client_addr);

    struct timeval timeout;

    parent_stdout = stdout;
    parent_stderr = stderr;

    const struct sockaddr_in6 addresses[2] = {
            {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_ANY_INIT, .sin6_port = htons(8080)},
            {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_ANY_INIT, .sin6_port = htons(4443)}
    };

    printf("Necronda Web Server\n");

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        unsigned long len = strlen(arg);
        if ((len == 2 && strncmp(arg, "-h", 2) == 0) || (len == 6 && strncmp(arg, "--help", 6) == 0)) {
            printf("Usage: necronda-server [-h] -w <PATH> -c <CERT-FILE> -p <KEY-FILE>\n"
                   "\n"
                   "Options:\n"
                   "  -c, --cert <CERT-FILE>    path to the full chain certificate file\n"
                   "  -h, --help                print this dialogue\n"
                   "  -p, --privkey <KEY-FILE>  path to the private key file\n"
                   "  -w, --webroot <PATH>      path to the web root directory\n");
            return 0;
        } else if ((len == 2 && strncmp(arg, "-w", 2) == 0) || (len == 9 && strncmp(arg, "--webroot", 9) == 0)) {
            if (i == argc - 1) {
                fprintf(stderr, ERR_STR "Unable to parse argument %s, usage: --webroot <WEBROOT>" CLR_STR "\n", arg);
                return 1;
            }
            webroot_base = argv[++i];
        } else if ((len == 2 && strncmp(arg, "-c", 2) == 0) || (len == 6 && strncmp(arg, "--cert", 6) == 0)) {
            if (i == argc - 1) {
                fprintf(stderr, ERR_STR "Unable to parse argument %s, usage: --cert <CERT-FILE>" CLR_STR "\n", arg);
                return 1;
            }
            cert_file = argv[++i];
        } else if ((len == 2 && strncmp(arg, "-p", 2) == 0) || (len == 9 && strncmp(arg, "--privkey", 9) == 0)) {
            if (i == argc - 1) {
                fprintf(stderr, ERR_STR "Unable to parse argument %s, usage: --privkey <KEY-FILE>" CLR_STR "\n", arg);
                return 1;
            }
            key_file = argv[++i];
        } else {
            fprintf(stderr, ERR_STR "Unable to parse argument '%s'" CLR_STR "\n", arg);
            return 1;
        }
    }

    if (webroot_base == NULL) {
        fprintf(stderr, ERR_STR "Error: --webroot is missing" CLR_STR "\n");
        return 1;
    }
    if (cert_file == NULL) {
        fprintf(stderr, ERR_STR "Error: --cert is missing" CLR_STR "\n");
        return 1;
    }
    if (key_file == NULL) {
        fprintf(stderr, ERR_STR "Error: --privkey is missing" CLR_STR "\n");
        return 1;
    }

    SOCKETS[0] = socket(AF_INET6, SOCK_STREAM, 0);
    if (SOCKETS[0] == -1) goto socket_err;
    SOCKETS[1] = socket(AF_INET6, SOCK_STREAM, 0);
    if (SOCKETS[1] == -1) {
        socket_err:
        fprintf(stderr, ERR_STR "Unable to create socket: %s" CLR_STR "\n", strerror(errno));
        return 1;
    }

    for (int i = 0; i < NUM_SOCKETS; i++) {
        if (setsockopt(SOCKETS[i], SOL_SOCKET, SO_REUSEADDR, &YES, sizeof(YES)) == -1) {
            fprintf(stderr, ERR_STR "Unable to set options for socket %i: %s" CLR_STR "\n", i, strerror(errno));
            return 1;
        }
    }

    if (bind(SOCKETS[0], (struct sockaddr *) &addresses[0], sizeof(addresses[0])) == -1) goto bind_err;
    if (bind(SOCKETS[1], (struct sockaddr *) &addresses[1], sizeof(addresses[1])) == -1) {
        bind_err:
        fprintf(stderr, ERR_STR "Unable to bind socket to address: %s" CLR_STR "\n", strerror(errno));
        return 1;
    }

    signal(SIGINT, terminate);
    signal(SIGTERM, terminate);

    openssl_init();

    client.ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_options(client.ctx, SSL_OP_SINGLE_DH_USE);
    SSL_CTX_set_verify(client.ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_min_proto_version(client.ctx, TLS1_VERSION);
    SSL_CTX_set_mode(client.ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_cipher_list(client.ctx, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4");
    SSL_CTX_set_ecdh_auto(client.ctx, 1);

    if (SSL_CTX_use_certificate_chain_file(client.ctx, cert_file) != 1) {
        fprintf(stderr, ERR_STR "Unable to load certificate chain file: %s: %s" CLR_STR "\n", ERR_reason_error_string(ERR_get_error()), cert_file);
        return 1;
    }
    if (SSL_CTX_use_PrivateKey_file(client.ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, ERR_STR "Unable to load private key file: %s: %s" CLR_STR "\n", ERR_reason_error_string(ERR_get_error()), key_file);
        return 1;
    }

    for (int i = 0; i < NUM_SOCKETS; i++) {
        if (listen(SOCKETS[i], LISTEN_BACKLOG) == -1) {
            fprintf(stderr, ERR_STR "Unable to listen on socket %i: %s" CLR_STR "\n", i, strerror(errno));
            return 1;
        }
    }

    FD_ZERO(&socket_fds);
    for (int i = 0; i < NUM_SOCKETS; i++) {
        FD_SET(SOCKETS[i], &socket_fds);
        if (SOCKETS[i] > max_socket_fd) {
            max_socket_fd = SOCKETS[i];
        }
    }

    fprintf(stderr, "Ready to accept connections\n");

    while (active) {
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        read_socket_fds = socket_fds;
        ready_sockets_num = select(max_socket_fd + 1, &read_socket_fds, NULL, NULL, &timeout);
        if (ready_sockets_num == -1) {
            fprintf(stderr, ERR_STR "Unable to select sockets: %s" CLR_STR "\n", strerror(errno));
            return 1;
        }

        for (int i = 0; i < NUM_SOCKETS; i++) {
            if (FD_ISSET(SOCKETS[i], &read_socket_fds)) {
                client_fd = accept(SOCKETS[i], (struct sockaddr *) &client_addr, &client_addr_len);
                if (client_fd == -1) {
                    fprintf(parent_stderr, ERR_STR "Unable to accept connection: %s" CLR_STR "\n", strerror(errno));
                    continue;
                }

                pid_t pid = fork();
                if (pid == 0) {
                    // child
                    signal(SIGINT, SIG_IGN);
                    signal(SIGTERM, SIG_IGN);

                    client.socket = client_fd;
                    client.enc = i == 1;

                    return client_handler(&client, client_num, &client_addr);
                } else if (pid > 0) {
                    // parent
                    client_num++;
                    close(client_fd);
                    for (int j = 0; j < MAX_CHILDREN; j++) {
                        if (CHILDREN[j] == 0) {
                            CHILDREN[j] = pid;
                            break;
                        }
                    }
                } else {
                    fprintf(stderr, ERR_STR "Unable to create child process: %s" CLR_STR "\n", strerror(errno));
                }
            }
        }

        int status = 0;
        int ret;
        for (int i = 0; i < MAX_CHILDREN; i++) {
            if (CHILDREN[i] != 0) {
                ret = waitpid(CHILDREN[i], &status, WNOHANG);
                if (ret < 0) {
                    fprintf(stderr, ERR_STR "Unable to wait for child process (PID %i): %s" CLR_STR "\n", CHILDREN[i], strerror(errno));
                } else if (ret == CHILDREN[i]) {
                    CHILDREN[i] = 0;
                }
            }
        }
    }

    return 0;
}
