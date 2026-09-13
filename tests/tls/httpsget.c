#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "psa/crypto.h"

static void say(const char *what, int ret) {
    char msg[128];
    mbedtls_strerror(ret, msg, sizeof(msg));
    printf("httpsget: %s failed: -0x%04x %s\n", what, (unsigned)-ret, msg);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: httpsget <host> <port> <ca.pem> <path>\n");
        return 2;
    }
    const char *host = argv[1], *port = argv[2], *ca_path = argv[3], *path = argv[4];

    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&ca);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    int ret;
    int status = 1;
    if ((ret = psa_crypto_init()) != PSA_SUCCESS) {
        printf("httpsget: psa_crypto_init failed: %d\n", ret);
        goto out;
    }
    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *)"httpsget", 8)) != 0) {
        say("seeding the DRBG from the entropy source", ret);
        goto out;
    }
    if ((ret = mbedtls_x509_crt_parse_file(&ca, ca_path)) != 0) {
        say("parsing the CA certificate", ret);
        goto out;
    }
    if ((ret = mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0) {
        say("connect", ret);
        goto out;
    }
    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        say("ssl_config_defaults", ret);
        goto out;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        say("ssl_setup", ret);
        goto out;
    }
    if ((ret = mbedtls_ssl_set_hostname(&ssl, host)) != 0) {
        say("set_hostname", ret);
        goto out;
    }
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_receive, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            say("handshake", ret);
            uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
            if (flags != 0) {
                char why[512];
                mbedtls_x509_crt_verify_info(why, sizeof(why), "httpsget:   ", flags);
                printf("%s", why);
            }
            goto out;
        }
    }
    printf("httpsget: %s, %s\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
    {
        uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
        if (flags != 0) {
            printf("httpsget: the certificate did not verify (0x%08x)\n", (unsigned)flags);
            goto out;
        }
        printf("httpsget: certificate verified against the CA\n");
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
        char subject[256];
        if (peer && mbedtls_x509_dn_gets(subject, sizeof(subject), &peer->subject) > 0) {
            printf("httpsget: peer subject %s\n", subject);
        }
    }

    char req[512];
    int len = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    for (int off = 0; off < len;) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + off, (size_t)(len - off));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret <= 0) {
            say("write", ret);
            goto out;
        }
        off += ret;
    }
    printf("httpsget: response:\n");
    for (;;) {
        unsigned char buf[1024];
        ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf) - 1);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            break;
        }
        if (ret < 0) {
            say("read", ret);
            goto out;
        }
        buf[ret] = '\0';
        fputs((const char *)buf, stdout);
    }
    mbedtls_ssl_close_notify(&ssl);
    printf("\nhttpsget: done\n");
    status = 0;
out:
    mbedtls_net_free(&net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return status;
}
