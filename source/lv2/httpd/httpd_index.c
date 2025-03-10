/*
 * httpd_index.c
 *
 *  Created on: Aug 3, 2008
 *      Author: Redline99
 *  Updated by: HexaMods Team
 */

#include <stdio.h>
#include <string.h>
#include <lwip/tcp.h>
#include <xenon_soc/xenon_io.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_soc/xenon_secotp.h>
#include <network/network.h>
#include <xb360/xb360.h>

#include "httpd.h"
#include "httpd_index.h"
#include "httpd_html.h"

// Store CPU & DVD Keys
unsigned char cpukey[0x10];
unsigned char dvdkey[0x10];

// Store Console Type
char console_type[20];
char fuse_info[512];  // Increased buffer size for safety

struct response_mem_priv_s {
    void *base;
    int len;
    int ptr, hdr_state;
    int togo;
    char *filename;
};

/*
 * Retrieve Console Type
 */
const char* get_console_type() {
    switch (xenon_get_console_type()) {
        case 0: return "Xenon";
        case 1: return "Zephyr";
        case 2: return "Falcon";
        case 3: return "Jasper";
        case 4: return "Trinity";
        case 5: return "Corona";
        case 6: return "Corona MMC";
        case 7: return "Winchester (Impossible)";
        default: return "Unknown";
    }
}

/*
 * Process HTTP Request
 */
int response_index_process_request(struct http_state *http, const char *method, const char *url) {
    if (strcmp(method, "GET") != 0)
        return 0;

    if (strcmp(url, "/index.html") != 0 && strcmp(url, "/default.html") != 0 &&
        strcmp(url, "/index.htm") != 0 && strcmp(url, "/default.htm") != 0 &&
        strcmp(url, "/") != 0)
        return 0;

    http->response_priv = mem_malloc(sizeof(struct response_mem_priv_s));
    if (!http->response_priv)
        return 0;

    struct response_mem_priv_s *priv = http->response_priv;

    // Fetch CPU & DVD Keys
    memset(cpukey, 0x00, sizeof(cpukey));
    cpu_get_key(cpukey);
    
    memset(dvdkey, 0x00, sizeof(dvdkey));
    kv_get_dvd_key(dvdkey);

    // Fetch Console Type
    strncpy(console_type, get_console_type(), sizeof(console_type) - 1);
    console_type[sizeof(console_type) - 1] = '\0'; // Ensure null termination

    // Fetch Fuse Data
    extern void get_fuse_data(char *buffer);
    get_fuse_data(fuse_info);

    priv->hdr_state = 0;
    priv->ptr = 0;
    http->code = 200;

    return 1;
}

/*
 * Generate HTTP Headers
 */
int response_index_do_header(struct http_state *http) {
    struct response_mem_priv_s *priv = http->response_priv;

    const char *t = NULL, *o = NULL;
    switch (priv->hdr_state) {
        case 0:
            t = "Content-Type";
            o = "text/html; charset=US-ASCII";
            break;
        case 1:
            return httpd_do_std_header(http);
    }

    int av = httpd_available_sendbuffer(http);
    if (av < (strlen(t) + strlen(o) + 4))
        return 1;

    httpd_put_sendbuffer_string(http, t);
    httpd_put_sendbuffer_string(http, ": ");
    httpd_put_sendbuffer_string(http, o);
    httpd_put_sendbuffer_string(http, "\r\n");
    ++priv->hdr_state;
    return 2;
}

/*
 * Generate HTML Response with Dynamic Data
 */
int response_index_do_data(struct http_state *http) {
    struct response_mem_priv_s *priv = http->response_priv;

    int i = 0;
    int c = sizeof(INDEX_HTML) / sizeof(*INDEX_HTML);
    int av = httpd_available_sendbuffer(http);

    if (!av) {
        printf("No HTTPD send buffer space\n");
        return 1;
    }

    char buffer[1024];

    for (i = priv->ptr; i < c; ++i) {
        memset(buffer, '\0', sizeof(buffer));

        // Replace placeholders with actual values
        if (strcmp(INDEX_HTML[i], "CPU_KEY") == 0) {
            snprintf(buffer, sizeof(buffer), "%016llX%016llX", ld(&cpukey[0x0]), ld(&cpukey[0x8]));
        }
        else if (strcmp(INDEX_HTML[i], "DVD_KEY") == 0) {
            snprintf(buffer, sizeof(buffer), "%016llX%016llX", ld(&dvdkey[0x0]), ld(&dvdkey[0x8]));
        }
        else if (strcmp(INDEX_HTML[i], "CONSOLE_TYPE") == 0) {
            snprintf(buffer, sizeof(buffer), "%s", console_type);
        }
        else if (strcmp(INDEX_HTML[i], "FUSE_INFO") == 0) {
            snprintf(buffer, sizeof(buffer), "%s", fuse_info);
        }
        else {
            snprintf(buffer, sizeof(buffer), "%s", INDEX_HTML[i]);
        }

        av -= (int) strlen(buffer);
        if (av <= 0) return 1;

        httpd_put_sendbuffer(http, (void *)buffer, (int)strlen(buffer));

        priv->ptr++;  // Increment pointer
    }
    return 0;
}

/*
 * Free Memory When Done
 */
void response_index_finish(struct http_state *http) {
    struct response_mem_priv_s *priv = http->response_priv;
    if (priv) {
        mem_free(priv);
    }
}
