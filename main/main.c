#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "driver/i2c_master.h"

#define TAG "MOSaico"
#define AP_PASSWORD "mosaico123"

static httpd_handle_t server;
static bool provisioning_mode;
static i2c_master_bus_handle_t oled_bus;
static i2c_master_dev_handle_t oled_device;
static uint8_t oled_frame[1024];

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDRESS 0x3C
#define OLED_SDA_GPIO 8
#define OLED_SCL_GPIO 9

static esp_err_t oled_write(const uint8_t *data, size_t length)
{
    if (!oled_device) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(oled_device, data, length, 100);
}

static esp_err_t oled_command(uint8_t command)
{
    uint8_t packet[2] = {0x00, command};
    return oled_write(packet, sizeof(packet));
}

static esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &oled_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDRESS,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(oled_bus, &device_config, &oled_device);
    if (err != ESP_OK) return err;

    err = i2c_master_probe(oled_bus, OLED_ADDRESS, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED not detected at 0x%02X: %s",
                 OLED_ADDRESS, esp_err_to_name(err));
        return err;
    }

    const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
        0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1,
        0xC8, 0xDA, 0x12, 0x81, 0x7F, 0xA4,
        0xA6, 0xAF
    };
    for (size_t i = 0; i < sizeof(init_commands); i++) {
        esp_err_t err = oled_command(init_commands[i]);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "SSD1306 OLED initialized at 0x%02X on SDA=%d SCL=%d",
             OLED_ADDRESS, OLED_SDA_GPIO, OLED_SCL_GPIO);
    return ESP_OK;
}

static esp_err_t oled_render(void)
{
    esp_err_t err = oled_command(0x21);
    if (err == ESP_OK) err = oled_command(0);
    if (err == ESP_OK) err = oled_command(127);
    if (err == ESP_OK) err = oled_command(0x22);
    if (err == ESP_OK) err = oled_command(0);
    if (err == ESP_OK) err = oled_command(7);
    if (err != ESP_OK) return err;

    uint8_t packet[1025];
    packet[0] = 0x40;
    memcpy(&packet[1], oled_frame, sizeof(oled_frame));
    return oled_write(packet, sizeof(packet));
}

static void oled_load_frame(void)
{
    nvs_handle_t handle;
    if (nvs_open("mosaico", NVS_READONLY, &handle) == ESP_OK) {
        size_t length = sizeof(oled_frame);
        esp_err_t err = nvs_get_blob(handle, "oled_frame", oled_frame, &length);
        nvs_close(handle);
        if (err != ESP_OK || length != sizeof(oled_frame)) {
            memset(oled_frame, 0, sizeof(oled_frame));
        }
    } else {
        memset(oled_frame, 0, sizeof(oled_frame));
    }
}

static esp_err_t index_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Mosaico</title><style>"
        ":root{color-scheme:dark}*{box-sizing:border-box}"
        "body{margin:0;font-family:system-ui;background:radial-gradient(circle at 20% 10%,#1b3150,#090d18 55%);color:#edf4ff}"
        "main{max-width:760px;margin:auto;padding:24px}.brand{display:flex;align-items:center;gap:14px;margin:28px 0}"
        ".mark{width:54px;height:54px;border-radius:16px;background:linear-gradient(135deg,#70e1c2,#6c8cff)}"
        "h1{margin:0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}"
        ".card{background:#141b2dcc;border:1px solid #ffffff12;border-radius:18px;padding:16px}"
        ".wide{grid-column:1/-1}.card h2{font-size:13px;color:#91a0b8;text-transform:uppercase;margin:0 0 12px}"
        ".value{font-size:18px;font-weight:700}.row{display:flex;gap:8px;margin-top:10px}"
        "input{width:100%;padding:12px;border-radius:10px;border:1px solid #ffffff20;background:#0c1220;color:white}"
        "button{border:0;border-radius:10px;padding:11px 16px;background:#6ef0c2;color:#07120f;font-weight:800}"
        ".progress{display:none;height:8px;border-radius:8px;background:#0a0f1b;margin-top:12px}"
        ".oled-preview{display:block;width:256px;max-width:100%;image-rendering:pixelated;border:1px solid #ffffff20;background:#d7dce7}"
        "</style></head><body><main>"
        "<header class=\"brand\"><div class=\"mark\"></div><div><h1>Mosaico</h1><small id=\"firmware\">Starting...</small></div></header>"
        "<section class=\"grid\"><div class=\"card\"><h2>Device</h2><div class=\"value\" id=\"device\">--</div></div>"
        "<div class=\"card\"><h2>Network</h2><div class=\"value\" id=\"network\">--</div></div>"
        "<div class=\"card wide\"><h2>Wi-Fi setup</h2><input id=\"ssid\" maxlength=\"32\" placeholder=\"Network name\">"
        "<div class=\"row\"><input id=\"password\" maxlength=\"64\" type=\"password\" placeholder=\"Password\"><button onclick=\"saveWifi()\">Connect</button></div></div>"
        "<div class=\"card wide\"><h2>Firmware update</h2><input id=\"firmwareFile\" type=\"file\" accept=\".bin\">"
        "<div class=\"row\"><button onclick=\"uploadFirmware()\">Upload firmware</button><button onclick=\"restart()\">Restart</button></div><div class=\"progress\" id=\"progress\"></div></div>"
        "<div class=\"card wide\"><h2>OLED image · 128 × 64</h2>"
        "<input id=\"imageFile\" type=\"file\" accept=\"image/*\" onchange=\"previewImage()\">"
        "<div class=\"row\"><canvas class=\"oled-preview\" id=\"oledPreview\" width=\"128\" height=\"64\"></canvas></div>"
        "<div class=\"row\"><button onclick=\"uploadImage()\">Upload to OLED</button><button onclick=\"clearImage()\">Clear OLED</button></div></div>"
        "</section></main><script>"
        "const oledCanvas=document.querySelector('#oledPreview');const oledContext=oledCanvas.getContext('2d',{willReadFrequently:true});"
        "async function previewImage(){let f=imageFile.files[0];if(!f)return;let bitmap=await createImageBitmap(f);"
        "oledContext.fillStyle='#d7dce7';oledContext.fillRect(0,0,128,64);let scale=Math.min(128/bitmap.width,64/bitmap.height);"
        "let w=bitmap.width*scale,h=bitmap.height*scale,x=(128-w)/2,y=(64-h)/2;oledContext.drawImage(bitmap,x,y,w,h);"
        "let pixels=oledContext.getImageData(0,0,128,64).data;"
        "for(let i=0;i<pixels.length;i+=4){let gray=.299*pixels[i]+.587*pixels[i+1]+.114*pixels[i+2];let value=gray<128?0:255;"
        "pixels[i]=pixels[i+1]=pixels[i+2]=value}oledContext.putImageData(new ImageData(pixels,128,64),0,0)}"
        "function oledBytes(){let pixels=oledContext.getImageData(0,0,128,64).data,bytes=new Uint8Array(1024);"
        "for(let page=0;page<8;page++){for(let x=0;x<128;x++){let value=0;for(let bit=0;bit<8;bit++){let y=page*8+bit;"
        "if(pixels[(y*128+x)*4]<128)value|=1<<bit}bytes[page*128+x]=value}}return bytes}"
        "async function uploadImage(){if(!imageFile.files[0])return alert('Choose an image first');"
        "let r=await fetch('/api/image',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:oledBytes()});"
        "alert(await r.text())}"
        "async function clearImage(){let r=await fetch('/api/image/clear',{method:'POST'});oledContext.fillStyle='#d7dce7';oledContext.fillRect(0,0,128,64);alert(await r.text())}"
        "async function refresh(){let s=await(await fetch('/api/status')).json();firmware.textContent=s.name+' '+s.version;device.textContent=s.chip+(s.psramMB?' · '+s.psramMB+' MB PSRAM':' · No PSRAM');network.textContent=s.wifi}"
        "async function saveWifi(){let r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid.value,password:password.value})});alert(await r.text())}"
        "async function restart(){await fetch('/api/restart',{method:'POST'})}"
        "async function uploadFirmware(){let f=firmwareFile.files[0];if(!f)return alert('Choose a firmware bin');let x=new XMLHttpRequest();x.open('POST','/api/update');x.onload=()=>alert(x.status==200?'Update complete':x.responseText);x.send(f)}"
        "refresh();setInterval(refresh,5000)</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ssid[33] = {0};
    char ip[16] = "Not connected";
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(ssid, ap.ssid, sizeof(ssid) - 1);
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr) {
            esp_ip4addr_ntoa((esp_ip4_addr_t *)&info.ip.addr, ip, sizeof(ip));
        }
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    char response[512];
    snprintf(response, sizeof(response),
             "{\"name\":\"Mosaico S3\",\"version\":\"%s\",\"chip\":\"ESP32-S3 rev %d\",\"psramMB\":%u,\"wifi\":\"%s%s%s\",\"freeHeap\":%u,\"uptime\":%llu}",
             app->version, chip.revision,
             (unsigned)(esp_get_free_heap_size() ? heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1048576 : 0),
             strlen(ssid) ? ssid : (provisioning_mode ? "Provisioning AP" : "Not connected"),
             strlen(ssid) ? " · " : "", strlen(ssid) ? ip : "",
             (unsigned)esp_get_free_heap_size(), (unsigned long long)(xTaskGetTickCount() / configTICK_RATE_HZ));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    char body[258] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    body[len] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};
    const char *ssid_key = "\"ssid\":\"";
    const char *pass_key = "\"password\":\"";
    char *p = strstr(body, ssid_key);
    if (p) {
        p += strlen(ssid_key);
        char *end = strchr(p, '"');
        if (end) {
            size_t count = (size_t)(end - p);
            if (count >= sizeof(ssid)) count = sizeof(ssid) - 1;
            memcpy(ssid, p, count);
        }
    }
    p = strstr(body, pass_key);
    if (p) {
        p += strlen(pass_key);
        char *end = strchr(p, '"');
        if (end) {
            size_t count = (size_t)(end - p);
            if (count >= sizeof(pass)) count = sizeof(pass) - 1;
            memcpy(pass, p, count);
        }
    }
    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Network name is required");
        return ESP_FAIL;
    }

    nvs_handle_t handle;
    if (nvs_open("mosaico", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ssid", ssid);
        nvs_set_str(handle, "password", pass);
        nvs_commit(handle);
        nvs_close(handle);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Credentials saved. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Restarting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t update_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    esp_err_t err = esp_ota_begin(update_partition, req->content_len > 0 ? req->content_len : OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buffer[1024];
    int received;
    while ((received = httpd_req_recv(req, buffer, sizeof(buffer))) > 0) {
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
    }
    if (received < 0) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
        return ESP_FAIL;
    }
    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Update complete");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t image_upload_handler(httpd_req_t *req)
{
    if (req->content_len != sizeof(oled_frame)) {
        char message[96];
        snprintf(message, sizeof(message),
                 "Expected %u bytes for a 128x64 monochrome image",
                 (unsigned)sizeof(oled_frame));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
        return ESP_FAIL;
    }

    uint8_t buffer[sizeof(oled_frame)];
    size_t received_total = 0;
    while (received_total < sizeof(buffer)) {
        int received = httpd_req_recv(req,
                                      (char *)buffer + received_total,
                                      sizeof(buffer) - received_total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image upload interrupted");
            return ESP_FAIL;
        }
        received_total += received;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("mosaico", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not open image storage");
        return ESP_FAIL;
    }
    err = nvs_set_blob(handle, "oled_frame", buffer, sizeof(buffer));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save image");
        return ESP_FAIL;
    }

    memcpy(oled_frame, buffer, sizeof(oled_frame));
    err = oled_render();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED render failed: %s", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Image uploaded to OLED");
    return ESP_OK;
}

static esp_err_t image_clear_handler(httpd_req_t *req)
{
    memset(oled_frame, 0, sizeof(oled_frame));
    nvs_handle_t handle;
    if (nvs_open("mosaico", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "oled_frame", oled_frame, sizeof(oled_frame));
        nvs_commit(handle);
        nvs_close(handle);
    }
    oled_render();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OLED cleared");
    return ESP_OK;
}

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) != ESP_OK) return;

    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler};
    httpd_uri_t wifi_uri = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler};
    httpd_uri_t restart_uri = {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler};
    httpd_uri_t update_uri = {.uri = "/api/update", .method = HTTP_POST, .handler = update_handler};
    httpd_uri_t image_upload_uri = {.uri = "/api/image", .method = HTTP_POST, .handler = image_upload_handler};
    httpd_uri_t image_clear_uri = {.uri = "/api/image/clear", .method = HTTP_POST, .handler = image_clear_handler};
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &restart_uri);
    httpd_register_uri_handler(server, &update_uri);
    httpd_register_uri_handler(server, &image_upload_uri);
    httpd_register_uri_handler(server, &image_clear_uri);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        provisioning_mode = true;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        provisioning_mode = false;
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void start_ap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_name[24];
    snprintf(ap_name, sizeof(ap_name), "Mosaico-%02X%02X%02X", mac[3], mac[4], mac[5]);

    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", ap_name);
    ap_config.ap.ssid_len = strlen(ap_name);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", AP_PASSWORD);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    ESP_LOGI(TAG, "Provisioning AP: %s password=%s", ap_name, AP_PASSWORD);
}

static esp_err_t load_wifi(nvs_handle_t handle, char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (err != ESP_OK || ssid[0] == 0) return ESP_ERR_NOT_FOUND;
    if (nvs_get_str(handle, "password", password, &password_size) != ESP_OK) password[0] = 0;
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    oled_load_frame();
    err = oled_init();
    if (err == ESP_OK) {
        err = oled_render();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not render saved OLED image: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "OLED not detected: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    char ssid[33] = {0};
    char password[65] = {0};
    nvs_handle_t handle;
    bool has_saved_wifi = false;
    if (nvs_open("mosaico", NVS_READWRITE, &handle) == ESP_OK) {
        size_t ssid_size = sizeof(ssid);
        size_t password_size = sizeof(password);
        has_saved_wifi = load_wifi(handle, ssid, ssid_size, password, password_size) == ESP_OK;
        nvs_close(handle);
    }

    if (has_saved_wifi) {
        esp_netif_create_default_wifi_sta();
        wifi_config_t sta_config = {0};
        memcpy(sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
        memcpy(sta_config.sta.password, password, sizeof(sta_config.sta.password));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        start_ap();
        provisioning_mode = false;
        ESP_LOGI(TAG, "Starting saved Wi-Fi connection: %s", ssid);
    } else {
        provisioning_mode = true;
        start_ap();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    start_server();

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }
    ESP_LOGI(TAG, "Boot: Mosaico S3 %s", app->version);
    ESP_LOGI(TAG, "ESP32-S3 rev %d, flash=%u MB, PSRAM=%u MB",
             chip.revision,
             (unsigned)(flash_size / 1048576),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1048576));
}
