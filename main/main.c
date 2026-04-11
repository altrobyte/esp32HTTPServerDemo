#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define WIFI_SSID "sidd"
#define WIFI_PASS "12345678"
#define MAX_RETRY 10
#define LED_PIN GPIO_NUM_2

static const char *TAG = "ESP32";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static int wifi_retry = 0;
static bool led_state = false;

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[200];
    snprintf(json, sizeof(json),
             "{\"device\":\"ESP32\",\"uptime\":%lu,\"led\":\"%s\"}",
             xTaskGetTickCount() / configTICK_RATE_HZ,
             led_state ? "ON" : "OFF");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    ESP_LOGI(TAG, "GET /status");
    return ESP_OK;
}

static esp_err_t led_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    httpd_req_recv(req, body, len);
    char resp[64];
    if (strstr(body, "LED_ON"))
    {
        led_state = true;
        gpio_set_level(LED_PIN, 1);
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"led\":\"ON\"}");
        ESP_LOGI(TAG, "LED ON");
    }
    else
    {
        led_state = false;
        gpio_set_level(LED_PIN, 0);
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"led\":\"OFF\"}");
        ESP_LOGI(TAG, "LED OFF");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t s = {"/status", HTTP_GET, status_handler, NULL};
        httpd_uri_t lp = {"/led", HTTP_POST, led_handler, NULL};
        httpd_register_uri_handler(server, &s);
        httpd_register_uri_handler(server, &lp);
        ESP_LOGI(TAG, "Server started!");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (wifi_retry < MAX_RETRY)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
            wifi_retry++;
            ESP_LOGW(TAG, "Retry %d/%d...", wifi_retry, MAX_RETRY);
        }
        else
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "=============================");
        ESP_LOGI(TAG, " CONNECTED!");
        ESP_LOGI(TAG, " IP: " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, " GET  http://" IPSTR "/status",
                 IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, " POST http://" IPSTR "/led",
                 IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "=============================");
        wifi_retry = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGE(TAG, "Failed! Check SSID/Password");
        esp_restart();
    }
}

void app_main(void)
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    printf("\n==================================\n");
    printf("  ESP32 HTTP Server — Altrobyte\n");
    printf("==================================\n\n");

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    start_server();
}