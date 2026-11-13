#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

static void (*s_common_recv)(const uint8_t *mac, const uint8_t *data, int len) = nullptr;

#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
static void onRecvV3(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info ? info->src_addr : nullptr;
  if (s_common_recv) {
    s_common_recv(mac, data, len);
  }
}
#else
static void onRecvLegacy(const uint8_t *mac, const uint8_t *data, int len) {
  if (s_common_recv) {
    s_common_recv(mac, data, len);
  }
}
#endif

void espnow_register_recv_bridge(void (*common)(const uint8_t *mac, const uint8_t *data, int len)) {
  s_common_recv = common;
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  esp_now_register_recv_cb(onRecvV3);
#else
  esp_now_register_recv_cb(onRecvLegacy);
#endif
}

