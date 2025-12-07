#define LOG_LOCAL_LEVEL 4   // DEBUG for just this file
//#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "stream_buf.h"
#include "word_site.h"
extern "C" int stream_buf_match(STREAM_BUF *stream_buf, char *buf, int blen, bool bStart);

#define MAX_HTTP_RECV_BUFFER 2048
//#define WORD_URL "https://www.merriam-webster.com/word-of-the-day/"
static const char *TAG = "http";

// TODO: create a function for cleaning up STREAM_BUF when main caller is done.
// TODO: maybe make this into a C++ class.

/**
 * HTTPS conenction to download web page.
 * The website for word-of-the-day contains lots of config that we must skip until we get
 * to the word and its definition. An ESP32 has limited memory, so this function throws away
 * most of the stream until we see a unique start string. Then we store until a unique end tag is found.
 * The storage buffer is malloc'ed here and must be freed by the caller.
 * @param stream_buf - contains parameters that define when to start storing page and when to stop.
 */
int http_perform_as_stream_reader(STREAM_BUF *stream_buf)
{
    bool bStart = true;
    int match_stat;
    char *buffer = (char *)malloc(MAX_HTTP_RECV_BUFFER);    // temp buffer, fetch web page in chunks
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return -1;
    }
    // use CRT bundle containing Mozilla root CA store
    esp_http_client_config_t config = {};
    config.url = WORD_URL;  // e.g., Merriam-Webster website
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        free(buffer);
        return -1;
    }
    // TODO: what if header does not have content_length?
    int content_length =  esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %" PRId64,
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));
    int total_read_len = 0, read_len;
    int read_cnt = 0;
    while (total_read_len < content_length) {
        read_len = esp_http_client_read(client, buffer, MAX_HTTP_RECV_BUFFER);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "Error read data");
        }
        total_read_len += read_len;
        read_cnt++;
        ESP_LOGI(TAG, "read_cnt=%d, total_read_len=%d", read_cnt, total_read_len);
#if 1
        // extract the content between start and end srings. stream_buf will contain it.
        match_stat = stream_buf_match(stream_buf, buffer, read_len, bStart);
		if (match_stat == 1 && bStart) {
			// found match_start
			ESP_LOGI(TAG, "found match_start at %d, while file_len = %d\n", stream_buf->idx, total_read_len);
			bStart = false;		// now look for match_end
		}
		if (read_len < MAX_HTTP_RECV_BUFFER || match_stat == 2) {
			// Reached end of file or found match_end
            if (match_stat == 2)
			    ESP_LOGI(TAG, "found match_end at %d, while file_len = %d\n", stream_buf->idx, total_read_len);
            else {
                ESP_LOGW(TAG, "end of stream, but match_end not found");
                stream_buf->buffer[stream_buf->idx] = '\0';
            }
			break;
		}
#endif
    }
    ESP_LOGI(TAG, "finished read, total_read_len = %d\n", total_read_len);
	ESP_LOGI(TAG, "stream_buf len = %d\n", stream_buf->idx);
	ESP_LOGD(TAG, "stream_buf.buffer len = %d\n", strlen(stream_buf->buffer));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    // TODO: caller should later free stream_buf
    free(buffer);
    return 0;
}
