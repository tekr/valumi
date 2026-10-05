/* The device half of captive_dns.h: one UDP socket on port 53, served by a
 * small task. The task is created once and never exits: stopping closes the
 * socket and parks it on a notification, which start() gives. A task that
 * deleted itself on stop would race a quick stop-start into leaving nothing
 * running. */
#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "captive_dns";

static volatile bool s_run;
static TaskHandle_t s_task;
static volatile uint32_t s_ip;

static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    /* A receive timeout so the task notices stop() within a second. */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

static void dns_task(void *arg)
{
    static uint8_t q[256], r[272];
    for (;;) {
        while (!s_run) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        int sock = open_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "cannot bind :53, retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "answering every name with the setup page");
        while (s_run) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) {
                continue;
            }
            int len = captive_dns_reply(q, (size_t)n, r, sizeof(r), s_ip);
            /* Which names a phone looks up on joining is exactly what decides
             * whether it offers the setup page, and it differs by maker; log
             * them. Setup mode only, and from this task, never the render
             * one. */
            char name[64];
            int k = 0;
            for (int p = 12; p < n && q[p] != 0 && k < (int)sizeof(name) - 1;) {
                int l = q[p++];
                for (int c = 0; c < l && p < n && k < (int)sizeof(name) - 2; c++) {
                    name[k++] = (char)q[p++];
                }
                name[k++] = '.';
            }
            name[k > 0 ? k - 1 : 0] = '\0';
            int qtype = -1;
            if (len > 0) {
                int p = 12;
                while (p < n && q[p] != 0) {
                    p += q[p] + 1;
                }
                qtype = (p + 2 < n) ? (q[p + 1] << 8 | q[p + 2]) : -1;
            }
            ESP_LOGI(TAG, "query %s type %d -> %s", name, qtype, len > 0 ? "answered" : "ignored");
            if (len > 0) {
                sendto(sock, r, (size_t)len, 0, (struct sockaddr *)&from, fl);
            }
        }
        close(sock);
        ESP_LOGI(TAG, "stopped");
    }
}

esp_err_t captive_dns_start(uint32_t ip)
{
    s_ip = ip;
    s_run = true;
    if (s_task == NULL &&
        xTaskCreate(dns_task, "captive_dns", 3072, NULL, 2, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void captive_dns_stop(void)
{
    s_run = false;
}
