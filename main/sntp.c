#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <time.h>
static const char *TAG = "sntp";

int sntp_setup()
{
    int retval = 0;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    int retry = 0;
    const int retry_count = 10;
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    }
    if (retry >= retry_count) {
        ESP_LOGW(TAG, "Failed to connect to NTP");
        retval = 1;
    }
    return retval;
}

const char tz_posix[] = "PST8PDT,M3.2.0/2:00:00,M11.1.0/2:00:00";

struct tm get_local_datetime(char* dt_str, const char* tz_posix)
{
	// POSIX time functions
	time_t now;
	char strftime_buf[64];
	struct tm timeinfo;
	time(&now);
	// TODO: next 2 line should be done once in app_main()
	setenv("TZ", tz_posix, 1);  // POSIX timezone
	tzset();
	localtime_r(&now, &timeinfo);
	strftime(dt_str, sizeof(strftime_buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    return timeinfo;
}

