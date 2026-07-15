// appOTA —— OTA 空中升级(IDF 原生 HTTP OTA)
//
// 从写死的 URL 下载固件并烧录到 ota_1,完成后重启。
// 电脑上需要起 HTTP 服务器提供 .bin:
//   cd build && python -m http.server 8070
//
// 回合制语义:setup() 连接 WiFi → 下载 → 重启(或失败提示)。

#include "apps.h"
#include <starboard_app.h>
#include <starboard_hal.h>
#include <starboard_display.h>
#include <starboard_gui.h>
#include <starboard_config.h>
#include <Arduino.h>

#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_partition.h>

namespace
{

static const char *TAG = "appOTA";

// ⚠️ OTA 固件下载地址(按你实际电脑 IP 修改)
static const char *OTA_URL = "http://10.10.10.100:8070/ota_test.bin";

void drawOtaScreen(const char *line1, const char *line2 = nullptr,
                   const char *line3 = nullptr, const char *line4 = nullptr,
                   const char *line5 = nullptr)
{
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(COL_BG);
        u8g2.setFont(CN_FONT_MAIN);
        int y = 70;
        auto line = [&](const char *s, uint16_t col) {
            if (s) { u8g2.setForegroundColor(col); u8g2.drawUTF8(40, y, s); y += 32; }
        };
        line(line1, COL_NORMAL);
        line(line2, COL_NORMAL);
        line(line3, COL_ALERT);
        line(line4, COL_NORMAL);
        line(line5, COL_NORMAL);
    } while (display.nextPage());
}

class AppOTA : public AppBase
{
public:
    AppOTA()
    {
        name = "ota";
        title = "OTA 升级";
        resumable = false;
        showInList = false; // 从设置菜单进,不在 App 列表显示
    }

    void setup() override
    {
        // 连接 WiFi
        drawOtaScreen("正在连接 WiFi...");
        hal.wifiInit(8);
        if (hal.wifiState != HAL::WifiState::Connected)
        {
            GUI::msgbox("OTA 升级", "WiFi 连接失败\n请先配网再使用");
            appManager.goBack();
            return;
        }

        // 确认是否开始
        if (!GUI::msgbox_yn("OTA 升级", "将从服务器下载固件\n请确认电脑已启动 HTTP 服务器\n\n继续?"))
        {
            appManager.goBack();
            return;
        }

        // 开始下载
        drawOtaScreen("正在下载固件...", OTA_URL, "请勿断电");

        esp_err_t ret = doOtaUpdate();

        if (ret == ESP_OK)
        {
            drawOtaScreen("升级成功", "3 秒后重启...");
            delay(3000);
            esp_restart();
        }
        else
        {
            drawOtaScreen("升级失败", esp_err_to_name(ret), "按中键退出");
            while (digitalRead(PIN_BUTTONC) == LOW) { delay(50); }
            while (digitalRead(PIN_BUTTONC) == HIGH) { delay(50); }
            appManager.goBack();
        }
    }

private:
    esp_err_t doOtaUpdate()
    {
        esp_http_client_config_t httpConfig = {};
        httpConfig.url = OTA_URL;
        httpConfig.timeout_ms = 30000;
        httpConfig.buffer_size = 4096;
        httpConfig.skip_cert_common_name_check = true;

        esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
        if (!client)
        {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            return ESP_FAIL;
        }

        esp_err_t err = esp_http_client_open(client, 0); // GET
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return err;
        }

        int64_t contentLen = esp_http_client_fetch_headers(client);
        ESP_LOGI(TAG, "OTA file size: %lld bytes", contentLen);
        if (contentLen <= 0)
        {
            ESP_LOGE(TAG, "Invalid content length");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }

        const esp_partition_t *updatePartition = esp_ota_get_next_update_partition(NULL);
        if (!updatePartition)
        {
            ESP_LOGE(TAG, "No OTA partition found");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NOT_FOUND;
        }

        esp_ota_handle_t otaHandle;
        err = esp_ota_begin(updatePartition, contentLen, &otaHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return err;
        }

        char *buf = (char *)malloc(httpConfig.buffer_size);
        if (!buf)
        {
            ESP_LOGE(TAG, "malloc failed");
            esp_ota_abort(otaHandle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }

        int totalRead = 0;
        int lastProgress = -1;

        while (true)
        {
            int readLen = esp_http_client_read(client, buf, httpConfig.buffer_size);
            if (readLen < 0)
            {
                ESP_LOGE(TAG, "HTTP read error: %d", readLen);
                free(buf);
                esp_ota_abort(otaHandle);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            if (readLen == 0)
                break;

            err = esp_ota_write(otaHandle, buf, readLen);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                free(buf);
                esp_ota_abort(otaHandle);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return err;
            }

            totalRead += readLen;

            if (contentLen > 0)
            {
                int pct = (int)(totalRead * 100LL / contentLen);
                if (pct / 10 != lastProgress / 10)
                {
                    char pctBuf[16];
                    snprintf(pctBuf, sizeof(pctBuf), "%d%% (%dKB)", pct, totalRead / 1024);
                    drawOtaScreen("正在下载固件...", OTA_URL, pctBuf, "请勿断电");
                    lastProgress = pct;
                }
            }
        }

        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        ESP_LOGI(TAG, "OTA download complete, %d bytes written", totalRead);

        err = esp_ota_end(otaHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_ota_set_boot_partition(updatePartition);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "OTA done, next boot from %s", updatePartition->label);
        return ESP_OK;
    }
};

AppOTA appOTAInst;

} // namespace

extern AppBase *const appOTA = &appOTAInst;