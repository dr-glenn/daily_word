/* Get a new word every day and display with definition.
 * I am using Merriam-Webster web site, but will add other possibilities.
 * The web page is full of cruft. From a page that exceeds 100 kBytes, the word
 * and definition itself rarely exceed about 1 kB.
 * The program searches for a unique text string
 * that precedes the word itself and then searches for another text string that
 * follows after the end of the definition. Only characters between the start
 * and end are saved for subsequent extraction.
 * The program extracts the word, the pronunciation and the defintion for display.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <malloc.h>
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#define LOG_LOCAL_LEVEL 4   // DEBUG for just this file
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_event.h"
// epaper includes
//#define EPAPER EPD_2IN7
//#define EPAPER EPD_2IN9
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include "imagedata.h"
#include "stream_buf.h"
#include "esp_mac.h"

static const char *TAG = "word-a-day";
//#define MAX_HTTP_RECV_BUFFER 2048
//#define WORD_URL "https://www.merriam-webster.com/word-of-the-day/"

extern "C" void wifi_init_sta(void);
extern "C" void stream_buf_init(STREAM_BUF *stream_buf, int max_buf_len);
extern "C" int stream_buf_match(STREAM_BUF *stream_buf, char *buf, int blen, bool bStart);
extern int http_perform_as_stream_reader(STREAM_BUF *stream_buf);

#define WORD_LEN    30
#define PRONOUNCE_LEN   40
#define DEFINE_LEN  300
typedef struct {
    char word[WORD_LEN];
    char pronounce[PRONOUNCE_LEN];
    char define[DEFINE_LEN];
} WORD_OF_DAY;
WORD_OF_DAY word_of_day;

#if !defined(CONFIG_WIFI_SSID)
#pragma GCC error "Do not have CONFIG_WIFI_SSID"
#else
#pragma GCC warning "YES have CONFIG_WIFI_SSID"
#endif

#if !defined(CONFIG_EPD_27) && !defined(CONFIG_EPD_29)
#pragma GCC warning "Do not have CONFIG_EPD_27 or EPD_29"
#else
#pragma GCC warning "YES have CONFIG_EPD_something"
#endif

void strip_extra_spaces(char* str) {
    ESP_LOGD(TAG, "strip_extra_spaces, len = %d\n  str = %s", strlen(str), str);
  int i, x;
  for(i=x=0; str[i]; ++i)
    if(!isspace(str[i]) || (i > 0 && !isspace(str[i-1])))
      str[x++] = str[i];
  str[x] = '\0';
}

/**
 * Remove all HTML tags from buffer and leading spaces. This function is only needed with the word definition,
 * because the definition may include <em> tags and <a> tags and the program cannot
 * handle these tags.
 * @param char* in_buf part of HTML stream, might not have a string terminator
 * @param int in_len length of in_buf
 * @return char* null-terminated buffer for output, created by malloc. Calling routine must free when done.
 */
char* strip_tags(char* in_buf, int in_len)
{
    char* out_buf = (char*)malloc(in_len+1);
    char* out_ptr = out_buf;
    char c;
    ESP_LOGD(TAG, "strip_tags: in_buf len = %d", in_len);
    bool is_tag = false;
    bool is_lead_space = true;
    for (int i = 0; i < in_len; i++) {
        c = in_buf[i];
        if (is_lead_space) {
            if (isspace(c)) continue;   //don't copy leading spaces
            else is_lead_space = false;
        }
        if (c == '<' || c == '(') { is_tag = true; continue; }
        else if (c == '>' || c == ')') { is_tag=false; continue; }
        if (is_tag) continue;   // skip all chars between "< ... > or (...)"
        // Not processing a tag
        *out_ptr = c;
        out_ptr++;
    }
    *out_ptr = '\0';

    // If there are parenthetical phrases, then we get extra spaces, so remove them.
    strip_extra_spaces(out_buf);
    ESP_LOGD(TAG, "return from strip_extra_spaces");
    return out_buf;
}

/**
  * Remove all HTML comments from the STREAM_BUF.
  * The buffer within STREAM_BUF is no longer needed.
  * The returned buffer must be free'd by calling routine when done with it.
  * @param STREAM_BUF* stream_buf
  * @return char* buffer new buffer, null-terminated
  */
char* strip_comments(STREAM_BUF *stream_buf)
{
    // When searching the contents for matches, there may be comments that look like what we want,
    // for example the pronounciation seems to have replicas, but they're not the same.
    int in_buf_len = stream_buf->idx;
    //int ifind = 0;
    char *in_buf = stream_buf->buffer;
    char *out_buf = (char*) malloc(in_buf_len);
    int out_idx = 0;
    out_buf[0] = '\0';
    char *find;

    while (1) {
        find = strstr(in_buf, "<!--");
        if (find) {
            // Copy preceding chars
            strncat(out_buf, in_buf, find - in_buf);
            out_idx += (find - in_buf);
            out_buf[out_idx] = '\0';
            // Now skip to comment end
            // TODO: can I simply use in_buf = find?
            in_buf += (find - in_buf);  // point to comment start position
            find = strstr(in_buf, "-->");
            if (!find) {
                ESP_LOGE(TAG, "impossible");
                // TODO: how to safely exit?
                break;
            }
            else {
                // TODO: can I simply use in_buf = find?
                in_buf += (find - in_buf + strlen("-->"));
            }
        }
        else {
            // no more comments found
            strcat(out_buf, in_buf);
            break;
        }
    }
    ESP_LOGD(TAG, "strip_comments: out_buf: %s", out_buf);
    return out_buf; // must free when done
}

/**
 * Extract word of the day from the HTML stream. Store it in word_t.
 * @param WORD_OF_DAY *word_t a struct for storing extracts from the HTML.
 * @param char* word_buf HTML extract that includes the word of the day.
 * @return void
 */
void word_extract(WORD_OF_DAY *word_t, char* word_buf)
{
    const char* start = "<h2 class=\"word-header-txt\">";
    char* find = strstr(word_buf, start);
    if (!find) {
        ESP_LOGE(TAG, "word: could not find %s\n", start);
    }
    else {
        char *w = find + strlen(start);
        find = strstr(w, "</h2>");
        strncpy(word_t->word, w, find - w);
        word_t->word[find-w+1] = '\0';
        ESP_LOGI(TAG, "word_extract: found = %s", word_t->word);
    }
}

/* TODO: implment and use this function */
/**
 * Extract word type from the HTML stream. Store it in word_t.
 * noun, adjective, verb are word types.
 * @param WORD_OF_DAY *word_t a struct for storing extracts from the HTML.
 * @param char* word_buf HTML extract that includes the word pronounciation.
 * @return void
 */
void type_extract(WORD_OF_DAY *word_t, char* word_buf)
{
    const char* start = "<span class=\"main-attr\">";
}

/**
 * Extract word pronounciation from the HTML stream. Store it in word_t.
 * @param WORD_OF_DAY *word_t a struct for storing extracts from the HTML.
 * @param char* word_buf HTML extract that includes the word pronounciation.
 * @return void
 */
void pronounce_extract(WORD_OF_DAY *word_t, char* word_buf)
{
    const char* start = "<span class=\"word-syllables\">";
    char* find = strstr(word_buf, start);
    if (!find) {
        ESP_LOGE(TAG, "pronounce: could not find %s\n", start);
    }
    else {
        char *w = find + strlen(start);
        find = strstr(w, "</span>");
        strncpy(word_t->pronounce, w, find - w);
        word_t->pronounce[find-w+1] = '\0';
        ESP_LOGI(TAG, "pronounce_extract: found = %s", word_t->pronounce);
    }
}

/**
 * Extract word definition from the HTML stream. Store it in word_t.
 * @param WORD_OF_DAY *word_t a struct for storing extracts from the HTML.
 * @param char* word_buf HTML extract that includes the word defintion.
 * @return void
 */
void define_extract(WORD_OF_DAY *word_t, char* word_buf)
{
    const char* start = "<h2>What It Means</h2>";
    char* find = strstr(word_buf, start);
    if (!find) {
        ESP_LOGE(TAG, "define: could not find %s\n", start);
    }
    else {
        char *w = find + strlen(start);
        find = strstr(w, "</p>");
        ESP_LOGD(TAG, "define_extract, len = %d", find-w);
        char* new_buf = strip_tags(w, find-w);
        ESP_LOGD(TAG, "define_extract: after strip_tags, len = %d", strlen(new_buf));
        strncpy(word_t->define, new_buf, strlen(new_buf) < DEFINE_LEN ? strlen(new_buf) : DEFINE_LEN-1);
        //word_t->pronounce[find-w+1] = '\0';
        ESP_LOGI(TAG, "define_extract: found = %s", word_t->define);
        free(new_buf);
    }
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);  // doesn't work!
    esp_log_level_set("wifi", ESP_LOG_ERROR);
#if 0
    esp_log_level_set("cpu_start", ESP_LOG_INFO);
    esp_log_level_set("pmu_param", ESP_LOG_INFO);
    esp_log_level_set("memory_layout", ESP_LOG_INFO);
    esp_log_level_set("heap_init", ESP_LOG_INFO);
    esp_log_level_set("intr_alloc", ESP_LOG_INFO);
    esp_log_level_set("spi_flash", ESP_LOG_INFO);
#endif

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    ESP_ERROR_CHECK(nvs_flash_init());
    //ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    wifi_init_sta();

    ESP_LOGI(TAG, "Connected to AP");
    STREAM_BUF stream_buf;
    stream_buf_init(&stream_buf, STREAM_BUF_LEN);
    
    int status = http_perform_as_stream_reader(&stream_buf);
    if (status != 0) {
        // TODO: find a better way
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    char* new_buf = strip_comments(&stream_buf);    // removes HTML comments
    // Extract items from the buffer
    word_extract(&word_of_day, new_buf);            // get the word
    pronounce_extract(&word_of_day, new_buf);       // get the pronounciation
    define_extract(&word_of_day, new_buf);          // get the definition
    free(new_buf);

    DEV_Module_Init();
#ifdef CONFIG_EPD_29
    EPD_2IN9_Init(EPD_2IN9_FULL);
    EPD_2IN9_Clear();
#else
    EPD_2IN7_V2_Init();
    EPD_2IN7_V2_Clear();
#endif
    DEV_Delay_ms(500);

    //Create a new image cache
    UBYTE *BlackImage;
    /* you have to edit the startup_stm32fxxx.s file and set a big enough heap size */
#ifdef CONFIG_EPD_29
    UWORD Imagesize = ((EPD_2IN9_WIDTH % 8 == 0)? (EPD_2IN9_WIDTH / 8 ): (EPD_2IN9_WIDTH / 8 + 1)) * EPD_2IN9_HEIGHT;
#else
    UWORD Imagesize = ((EPD_2IN7_V2_WIDTH % 8 == 0)? (EPD_2IN7_V2_WIDTH / 8 ): (EPD_2IN7_V2_WIDTH / 8 + 1)) * EPD_2IN7_V2_HEIGHT;
#endif
    if((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
        printf("Failed to apply for black memory...\r\n");
        while(1);
    }
    //printf("Paint_NewImage\r\n");
    // setup a PAINT object (canvas) for the GUI_Paint functions - PAINT object is a Singleton
#ifdef CONFIG_EPD_29
    // rotete 270
    Paint_NewImage(BlackImage, EPD_2IN9_WIDTH, EPD_2IN9_HEIGHT, 270, WHITE);
#else
    Paint_NewImage(BlackImage, EPD_2IN7_V2_WIDTH, EPD_2IN7_V2_HEIGHT, 270, WHITE);
#endif
#if 1   // redundant
    // Assign the canvas to GUI_Paint functions
    Paint_SelectImage(BlackImage);
#endif
    Paint_Clear(WHITE);

    // Draw on the canvas
    sFONT* font = &Font24;
    uint16_t cWidth, cHeight;
    Paint_DrawString_EN(10, 0, word_of_day.word, &Font24, WHITE, BLACK);
    font = &Font16;
    cWidth = font->Width; cHeight = font->Height;
    Paint_DrawString_EN(10, 25, word_of_day.pronounce, &Font16, BLACK, WHITE);
    font = &Font12;
    cWidth = font->Width; cHeight = font->Height;
    // calculate number of chars per line and number of lines
    // Note: display width and height are reversed in Waveshare sample code.
#ifdef CONFIG_EPD_29
    int nChar = EPD_2IN9_HEIGHT / (cWidth+1);
    int nRow  = (EPD_2IN9_WIDTH - 40) / (cHeight+1);
#else
    int nChar = EPD_2IN7_V2_HEIGHT / (cWidth+1);
    int nRow  = (EPD_2IN7_V2_WIDTH - 40) / (cHeight+1);
#endif
    //ESP_LOGD(TAG, "nRow=%d, nChar=%d\n", nRow, nChar);
    Paint_DrawString_EN(0, 40, word_of_day.define, &Font12, BLACK, WHITE);
    // Send canvas to the display
#ifdef CONFIG_EPD_29
    EPD_2IN9_Display(BlackImage);
    DEV_Delay_ms(10000);
    EPD_2IN9_Sleep();
#else
    EPD_2IN7_V2_Display(BlackImage);
    DEV_Delay_ms(10000);
    EPD_2IN7_V2_Sleep();
#endif
    free(BlackImage);
    BlackImage = NULL;
    free(stream_buf.buffer);
}