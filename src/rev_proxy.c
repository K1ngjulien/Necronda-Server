/**
 * Necronda Web Server
 * Reverse proxy
 * src/rev_proxy.c
 * Lorenz Stechauner, 2021-01-07
 */

#include "rev_proxy.h"

sock rev_proxy;
char *rev_proxy_host = NULL;
struct timeval server_timeout = {.tv_sec = SERVER_TIMEOUT, .tv_usec = 0};


int rev_proxy_init(http_req *req, http_res *res, host_config *conf, sock *client, http_status *custom_status,
                   char * err_msg) {
    char buffer[CHUNK_SIZE];
    long ret;
    int tries = 0;
    int retry = 0;

    if (rev_proxy.socket != 0 && strcmp(rev_proxy_host, conf->name) == 0 && sock_check(&rev_proxy) == 0) {
        goto rev_proxy;
    }

    retry:
    if (rev_proxy.socket != 0) {
        print(BLUE_STR "Closing proxy connection" CLR_STR);
        sock_close(&rev_proxy);
    }
    retry = 0;
    tries++;

    rev_proxy.socket = socket(AF_INET6, SOCK_STREAM, 0);
    if (rev_proxy.socket  < 0) {
        print(ERR_STR "Unable to create socket: %s" CLR_STR, strerror(errno));
        res->status = http_get_status(500);
        return -1;
    }

    server_timeout.tv_sec = SERVER_TIMEOUT;
    server_timeout.tv_usec = 0;
    if (setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &server_timeout, sizeof(server_timeout)) < 0)
        goto rev_proxy_timeout_err;
    if (setsockopt(client->socket, SOL_SOCKET, SO_SNDTIMEO, &server_timeout, sizeof(server_timeout)) < 0) {
        rev_proxy_timeout_err:
        res->status = http_get_status(502);
        print(ERR_STR "Unable to set timeout for socket: %s" CLR_STR, strerror(errno));
        sprintf(err_msg, "Unable to set timeout for socket: %s", strerror(errno));
        goto proxy_err;
    }

    struct hostent *host_ent = gethostbyname(conf->rev_proxy.hostname);
    if (host_ent == NULL) {
        res->status = http_get_status(502);
        print(ERR_STR "Unable to connect to server: Name or service not known" CLR_STR);
        sprintf(err_msg, "Unable to connect to server: Name or service not known.");
        goto proxy_err;
    }

    struct sockaddr_in6 address = {.sin6_family = AF_INET6, .sin6_port = htons(conf->rev_proxy.port)};
    if (host_ent->h_addrtype == AF_INET6) {
        memcpy(&address.sin6_addr, host_ent->h_addr_list[0], host_ent->h_length);
    } else if (host_ent->h_addrtype == AF_INET) {
        unsigned char addr[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0};
        memcpy(addr + 12, host_ent->h_addr_list[0], host_ent->h_length);
        memcpy(&address.sin6_addr, addr, 16);
    }

    if (connect(rev_proxy.socket, (struct sockaddr *) &address, sizeof(address)) < 0) {
        res->status = http_get_status(502);
        print(ERR_STR "Unable to connect to server: %s" CLR_STR, strerror(errno));
        sprintf(err_msg, "Unable to connect to server: %s.", strerror(errno));
        goto proxy_err;
    }

    if (conf->rev_proxy.enc) {
        rev_proxy.ssl = SSL_new(rev_proxy.ctx);
        SSL_set_fd(rev_proxy.ssl, rev_proxy.socket);
        SSL_set_connect_state(rev_proxy.ssl);

        ret = SSL_do_handshake(rev_proxy.ssl);
        rev_proxy._last_ret = ret;
        rev_proxy._errno = errno;
        rev_proxy._ssl_error = ERR_get_error();
        rev_proxy.enc = 1;
        if (ret < 0) {
            res->status = http_get_status(502);
            print(ERR_STR "Unable to perform handshake: %s" CLR_STR, sock_strerror(&rev_proxy));
            sprintf(err_msg, "Unable to perform handshake: %s.", sock_strerror(&rev_proxy));
            goto proxy_err;
        }
    }

    rev_proxy_host = conf->name;
    inet_ntop(address.sin6_family, (void *) &address.sin6_addr, buffer, sizeof(buffer));
    print(BLUE_STR "Established new connection with " BLD_STR "[%s]:%i" CLR_STR, buffer, conf->rev_proxy.port);

    rev_proxy:
    http_remove_header_field(&req->hdr, "Connection", HTTP_REMOVE_ALL);
    http_add_header_field(&req->hdr, "Connection", "keep-alive");
    http_remove_header_field(&req->hdr, "X-Forwarded-For", HTTP_REMOVE_ALL);
    http_add_header_field(&req->hdr, "X-Forwarded-For", client_addr_str);

    ret = http_send_request(&rev_proxy, req);
    if (ret < 0) {
        res->status = http_get_status(502);
        print(ERR_STR "Unable to send request to server (1): %s" CLR_STR, sock_strerror(&rev_proxy));
        sprintf(err_msg, "Unable to send request to server: %s.", sock_strerror(&rev_proxy));
        retry = tries < 4;
        goto proxy_err;
    }

    char *content_length = http_get_header_field(&req->hdr, "Content-Length");
    if (content_length != NULL) {
        unsigned long content_len = strtoul(content_length, NULL, 10);
        if (client->buf_len - client->buf_off > 0) {
            unsigned long len = client->buf_len - client->buf_off;
            if (len > content_len) {
                len = content_len;
            }
            ret = sock_send(&rev_proxy, client->buf, len, 0);
            if (ret <= 0) {
                res->status = http_get_status(502);
                print(ERR_STR "Unable to send request to server (2): %s" CLR_STR, sock_strerror(&rev_proxy));
                sprintf(err_msg, "Unable to send request to server: %s.", sock_strerror(&rev_proxy));
                retry = tries < 4;
                goto proxy_err;
            }
            content_len -= len;
        }
        if (content_len > 0) {
            ret = sock_splice(&rev_proxy, client, buffer, sizeof(buffer), content_len);
            if (ret <= 0) {
                if (ret == -1) {
                    res->status = http_get_status(502);
                    print(ERR_STR "Unable to send request to server (3): %s" CLR_STR, sock_strerror(&rev_proxy));
                    sprintf(err_msg, "Unable to send request to server: %s.", sock_strerror(&rev_proxy));
                    goto proxy_err;
                } else if (ret == -2) {
                    res->status = http_get_status(400);
                    print(ERR_STR "Unable to receive request from client: %s" CLR_STR, sock_strerror(client));
                    sprintf(err_msg, "Unable to receive request from client: %s.", sock_strerror(client));
                    return -1;
                }
                res->status = http_get_status(500);
                print(ERR_STR "Unknown Error" CLR_STR);
                return -1;
            }
        }
    }

    ret = sock_recv(&rev_proxy, buffer, sizeof(buffer), MSG_PEEK);
    if (ret <= 0) {
        res->status = http_get_status(502);
        print(ERR_STR "Unable to receive response from server: %s" CLR_STR, sock_strerror(&rev_proxy));
        sprintf(err_msg, "Unable to receive response from server: %s.", sock_strerror(&rev_proxy));
        goto proxy_err;
    }

    char *buf = buffer;
    unsigned short header_len = (unsigned short) (strstr(buffer, "\r\n\r\n") - buffer + 4);

    if (header_len <= 0) {
        res->status = http_get_status(502);
        print(ERR_STR "Unable to parse header: End of header not found" CLR_STR);
        sprintf(err_msg, "Unable to parser header: End of header not found.");
        goto proxy_err;
    }

    for (int i = 0; i < header_len; i++) {
        if ((buf[i] >= 0x00 && buf[i] <= 0x1F && buf[i] != '\r' && buf[i] != '\n') || buf[i] == 0x7F) {
            res->status = http_get_status(502);
            print(ERR_STR "Unable to parse header: Header contains illegal characters" CLR_STR);
            sprintf(err_msg, "Unable to parse header: Header contains illegal characters.");
            goto proxy_err;
        }
    }

    char *ptr = buf;
    while (header_len != (ptr - buf)) {
        char *pos0 = strstr(ptr, "\r\n");
        if (pos0 == NULL) {
            res->status = http_get_status(502);
            print(ERR_STR "Unable to parse header: Invalid header format" CLR_STR);
            sprintf(err_msg, "Unable to parse header: Invalid header format.");
            goto proxy_err;
        }
        if (ptr == buf) {
            if (strncmp(ptr, "HTTP/", 5) != 0) {
                res->status = http_get_status(502);
                print(ERR_STR "Unable to parse header: Invalid header format" CLR_STR);
                sprintf(err_msg, "Unable to parse header: Invalid header format.");
                goto proxy_err;
            }
            int status_code = (int) strtol(ptr + 9, NULL, 10);
            res->status = http_get_status(status_code);
            if (res->status == NULL && status_code >= 100 && status_code <= 999) {
                custom_status->code = status_code;
                strcpy(custom_status->type, "");
                strncpy(custom_status->msg, ptr + 13, strchr(ptr, '\r') - ptr - 13);
                res->status = custom_status;
            } else if (res->status == NULL) {
                res->status = http_get_status(502);
                print(ERR_STR "Unable to parse header: Invalid or unknown status code" CLR_STR);
                sprintf(err_msg, "Unable to parse header: Invalid or unknown status code.");
                goto proxy_err;
            }
        } else {
            ret = http_parse_header_field(&res->hdr, ptr, pos0);
            if (ret != 0) {
                res->status = http_get_status(502);
                print(ERR_STR "Unable to parse header" CLR_STR);
                sprintf(err_msg, "Unable to parse header.");
                goto proxy_err;
            }
        }
        if (pos0[2] == '\r' && pos0[3] == '\n') {
            break;
        }
        ptr = pos0 + 2;
    }
    sock_recv(&rev_proxy, buffer, header_len, 0);

    return 0;

    proxy_err:
    if (retry) goto retry;
    return -1;
}

int rev_proxy_send(sock *client, int chunked, unsigned long len_to_send) {
    long ret;
    char buffer[CHUNK_SIZE];
    long len, snd_len;
    // TODO handle websockets
    do {
        if (chunked) {
            ret = sock_recv(&rev_proxy, buffer, 16, MSG_PEEK);
            if (ret <= 0) {
                print("Unable to receive: %s", sock_strerror(&rev_proxy));
                break;
            }

            len_to_send = strtol(buffer, NULL, 16);
            char *pos = strstr(buffer, "\r\n");
            len = pos - buffer + 2;
            ret = sock_send(client, buffer, len, 0);

            sock_recv(&rev_proxy, buffer, len, 0);
            if (ret <= 0) break;
        }
        snd_len = 0;
        while (snd_len < len_to_send) {
            len = sock_recv(&rev_proxy, buffer, CHUNK_SIZE < (len_to_send - snd_len) ? CHUNK_SIZE : len_to_send - snd_len, 0);
            ret = sock_send(client, buffer, len, 0);
            if (ret <= 0) {
                print(ERR_STR "Unable to send: %s" CLR_STR, sock_strerror(client));
                break;
            }
            snd_len += ret;
        }
        if (ret <= 0) break;
        if (chunked) {
            sock_recv(&rev_proxy, buffer, 2, 0);
            ret = sock_send(client, "\r\n", 2, 0);
            if (ret <= 0) {
                print(ERR_STR "Unable to send: %s" CLR_STR, sock_strerror(client));
                break;
            }
        }
    } while (chunked && len_to_send > 0);
    return 0;
}
