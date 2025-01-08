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
#define REFRESH_TEST 0
#define NTP 1   // use network time protocol
//#define USE_TIMER 1
#define USE_SLEEP 1
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <malloc.h>
//#include <calloc.h>
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#if defined(NTP)
#include "esp_netif_sntp.h"	// TODO: probably don't need this in main
#endif
#define LOG_LOCAL_LEVEL 4   // DEBUG for just this file
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_event.h"

#define ONE_SHOT 1  // one shot or periodic timer
#ifdef USE_TIMER    // timer to update word
#include "esp_timer.h"
// ESP timer fnuctions use microseconds
const uint64_t DAY_SECONDS = 24 * 60 * 60;
// TODO: REFRESH params only used for periodic, not for ONE_SHOT
const uint64_t REFRESH_INTERVAL_HOURS = 6;
const uint64_t REFRESH_INTERVAL_SECONDS = 60 * 60 * REFRESH_INTERVAL_HOURS;
const uint64_t REFRESH_SECONDS = 300;   // 5 minutes for testing
//const uint64_t REFRESH_INTERVAL = (REFRESH_INTERVAL_SECONDS * 1000000);
const uint64_t REFRESH_INTERVAL = (REFRESH_SECONDS * 1000000);
#else   // deep sleep to update word
#endif
// epaper includes
//#define EPAPER EPD_2IN7
//#define EPAPER EPD_2IN9
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include "imagedata.h"
#include "stream_buf.h"
#include "esp_mac.h"
#include <math.h>

static const char *TAG = "daily_word";
//#define MAX_HTTP_RECV_BUFFER 2048
//#define WORD_URL "https://www.merriam-webster.com/word-of-the-day/"

extern "C" void wifi_init_sta(void);
extern "C" void stream_buf_init(STREAM_BUF *stream_buf, int max_buf_len);
extern "C" int stream_buf_match(STREAM_BUF *stream_buf, char *buf, int blen, bool bStart);
extern "C" int sntp_setup(void);
extern "C" struct tm get_local_datetime(char* dt_str, const char* tz_posix);
extern int http_perform_as_stream_reader(STREAM_BUF *stream_buf);

// TODO: tz_posix should only be defined in sntp.c or maybe in menuconfig
char datetime_str[64];
const char tz_posix[] = "PST8PDT,M3.2.0/2:00:00,M11.1.0/2:00:00";

#define WORD_LEN    30
#define TYPE_LEN    30
#define PRONOUNCE_LEN   40
#define DEFINE_LEN  500
typedef struct {
    char word[WORD_LEN];
    char type[TYPE_LEN];    // noun, verb, etc.
    char pronounce[PRONOUNCE_LEN];
    char define[DEFINE_LEN];
} WORD_OF_DAY;
WORD_OF_DAY word_of_day;

void word_init(WORD_OF_DAY *w)
{
	memset(w->word, '\0', sizeof(w->word));
	memset(w->type, '\0', sizeof(w->type));
	memset(w->pronounce, '\0', sizeof(w->pronounce));
	memset(w->define, '\0', sizeof(w->define));
}

// TODO: the following warnings exist to tell me about menuconfig problems
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

static void update_word(void);

char *test_num_str = "0123456789";
char *test_abc_str = "ABCDEFGHIJ";
static int test_cnt = 0;
static void test_refresh(void);

/**
 * Calculate next display refresh time, specified by day, hour and minute.
 * The day value is always relative to current, e.g., 5 days in the future,
 * but the hour and minute values can either designate relative to now,
 * or clock time. If absolute (clock) time, day=0 for next day.
 * bRelative - if zero, then clock time, else relative to now.
 * @return - time delay in seconds.
 * Example: Refresh the display once a day at 2:10 AM. (0, 2, 10, 0)
 * Example: Refresh the display 5 hours from now. (0, 5, 0, 1)
 */
static uint64_t calc_refresh_delay(int day, int hour, int minute, int bRelative)
{
    struct tm tm_local, tm_next;    // year, month, day, hour, minute, jday, etc.
    struct timeval tv_now;  // two members: tv_sec and tv_usec (microseconds)
    time_t now_sec, next_sec;
    // call get_local_datetime to be sure that timezone is set.
    tm_local = get_local_datetime(datetime_str, tz_posix);
    // tv_now and now_sec should be nearly the same
    gettimeofday(&tv_now, NULL);    // seconds and microsenconds since epoch
    now_sec = mktime(&tm_local);
    ESP_LOGD(TAG, "tv_now = %llu, now_sec = %llu", tv_now.tv_sec, now_sec);
    /* If bRelative is true, then the delay in seconds is simply ((day*24 + hour)*60 + minute)*60 */
    /* if bRelative is false, then add (day+1) to now_time to get future_time,
     * then find h,m values to subtract from future_time to get specified hour,minute.
     * Now we have the target future_time. This can be displayed using ctime().
     * And future_time - now_time = the timer delay.
     */
    if (bRelative > 0) {
        next_sec = ((day*24 + hour) * 60 + minute) * 60;
        ESP_LOGD(TAG, "relative: next_sec = %llu", next_sec);
    }
    else {
        // specify future clock time
        struct timeval tv_next;
        tv_next.tv_usec = 0; // don't care about microseconds
        tv_next.tv_sec = now_sec + (day+1) * 24 * 60 * 60;
        next_sec = tv_next.tv_sec;
        localtime_r(&next_sec, &tm_next);
        // time_next is greater than desired time, but less than 24 hours in the future.
        int dhour, dmin, dsec;
        dmin = tm_next.tm_min - minute;
        dhour = tm_next.tm_hour - hour;
        dsec = (dhour * 60 + dmin) * 60;
        next_sec -= dsec;
        next_sec -= now_sec;
        ESP_LOGD(TAG, "absolute: next_sec = %llu", next_sec);
    }
#if 1   // print the calculated delay for debugging
    int h, m, s, ts;
    h = next_sec / 3600;
    m = floor(next_sec % 3600 / 60);
    s = floor(next_sec % 3600 % 60);
    ESP_LOGI(TAG, "Set one_shot for %d h %d m %d sec", h, m, s);
#endif
    return next_sec;
}

#ifdef USE_TIMER
// Put this code somewhere else
static esp_timer_handle_t update_timer;
const int update_max = 3;
static int update_cnt = 0;

#ifdef ONE_SHOT
static void timer_callback(void *arg) {
    struct tm localtime;
    // publish
    ++update_cnt;
    ESP_LOGI(TAG, "callback i=%d", update_cnt);
#if REFRESH_TEST==0
    update_word();
    uint64_t refresh_sec = calc_refresh_delay(0, 1, 5, 0);
    uint64_t refresh_microsec = refresh_sec * 1000000;
    ESP_ERROR_CHECK(esp_timer_start_once(update_timer, refresh_microsec));
#else
    if (test_cnt < strlen(test_abc_str)) {
        test_refresh();
        test_cnt++;
        uint64_t refresh_sec = calc_refresh_delay(0, 0, 4, 1);
        uint64_t refresh_microsec = refresh_sec * 1000000;
        ESP_ERROR_CHECK(esp_timer_start_once(update_timer, refresh_microsec));
    }
    else {
        ESP_LOGD(TAG, "test_cnt=%d, no more refresh", test_cnt);
    }
#endif
}
#else
// Uses REFRESH_INTERVAL for refresh of word
static void timer_callback(void *) {
    // publish
    ++update_cnt;
    ESP_LOGI("timer", "callback i=%d", update_cnt);
    update_word();
    if (update_cnt > update_max) {
        esp_timer_stop(update_timer);
        ESP_LOGI("timer", "STOP PERIODIC update_timer");
    }
}
#endif
#endif	// USE_TIMER

/**
 * Strip extra spaces. Modify input str in place.
 */
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
 * @return char* null-terminated buffer for output, created by calloc. Calling routine must free when done.
 */
char* strip_tags(char* in_buf, int in_len)
{
    char* out_buf = (char*)calloc(in_len+1, sizeof(char));
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
    char *out_buf = (char*) calloc(in_buf_len, sizeof(char));
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
                ESP_LOGE(TAG, "strip_comments: impossible");
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
    //ESP_LOGD(TAG, "strip_comments: out_buf: %s", out_buf);
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
        word_t->word[find-w] = '\0';
        ESP_LOGI(TAG, "word_extract: found = %s", word_t->word);
    }
}

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
    char* find = strstr(word_buf, start);
    if (!find) {
        ESP_LOGE(TAG, "type: could not find %s\n", start);
    }
    else {
        char *w = find + strlen(start);
        find = strstr(w, "</span>");
        strncpy(word_t->type, w, find - w);
        word_t->type[find-w] = '\0';
        ESP_LOGI(TAG, "type_extract: found = %s", word_t->type);
    }
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
        word_t->pronounce[find-w] = '\0';
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
		int define_len = strlen(new_buf) < DEFINE_LEN ? strlen(new_buf) : DEFINE_LEN-1;
        strncpy(word_t->define, new_buf, define_len);
		word_t->define[define_len] = '\0';
        ESP_LOGI(TAG, "define_extract: found = %s", word_t->define);
        free(new_buf);
    }
}

void test_refresh(void)
{
    struct tm localtime;
    char str[26];
    // Wavesahre ePaper display code follows
    DEV_Module_Init();
    int display_width, display_height;
#ifdef CONFIG_EPD_29
    display_width = EPD_2IN9_HEIGHT;
    display_height = EPD_2IN9_WIDTH;
    EPD_2IN9_Init(EPD_2IN9_FULL);
    EPD_2IN9_Clear();
#else
    display_width = EPD_2IN7_V2_HEIGHT;
    display_height = EPD_2IN7_V2_WIDTH;
    EPD_2IN7_V2_Init();
    EPD_2IN7_V2_Clear();
#endif
    DEV_Delay_ms(500);

    //Create a new image cache
    UBYTE *BlackImage;
    UWORD Imagesize = ((display_height % 8 == 0)? (display_height / 8 ): (display_height / 8 + 1)) * display_width;
    if((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
        printf("Failed to allocate BlackImage...\r\n");
        // TODO: what to do now?
    }
    // setup a PAINT object (canvas) for the GUI_Paint functions - PAINT object is a Singleton
    // rotete 270
    Paint_NewImage(BlackImage, display_height, display_width, 270, WHITE);
#if 1   // redundant?
    // Assign the canvas to GUI_Paint functions
    Paint_SelectImage(BlackImage);
#endif
    Paint_Clear(WHITE);

    // Draw on the canvas
    uint16_t cWidth, cHeight, usedHeight;
    // Display the word
    sFONT* font = &Font24;
    cWidth = font->Width; cHeight = font->Height;
    char *test_refresh_str;
    // alternate writing numbers and letters
    test_refresh_str = (test_cnt % 2) ? test_abc_str : test_num_str;
    if (test_cnt < strlen(test_refresh_str)) {
        strncpy(str, test_refresh_str, strlen(test_refresh_str)-test_cnt);
        str[strlen(test_refresh_str)-test_cnt] = '\0';
        ESP_LOGD(TAG, "DrawString: %s", str);
        Paint_DrawString_EN(2, 0, str, font, WHITE, BLACK);
    }
    usedHeight = cHeight + 1;

    // TODO: if a line remains, maybe display the datetime
    font = &Font16;
    if ((usedHeight + cHeight) < display_height) {
        localtime = get_local_datetime(datetime_str, tz_posix);
        Paint_DrawString_EN(2, usedHeight, datetime_str, font, BLACK, WHITE);
    }

    // Send canvas to the display
#ifdef CONFIG_EPD_29
    EPD_2IN9_Display(BlackImage);
    DEV_Delay_ms(10000);    // TODO: why delay here?
    EPD_2IN9_Sleep();
#else
    EPD_2IN7_V2_Display(BlackImage);
    DEV_Delay_ms(10000);
    EPD_2IN7_V2_Sleep();
#endif
    free(BlackImage);
    BlackImage = NULL;
}

/**
 * Fetch word and update display once a day.
 */
void update_word(void)
{
    struct tm localtime;
    STREAM_BUF stream_buf;
    stream_buf_init(&stream_buf, STREAM_BUF_LEN);
    
    // stream_buf contains tags for start and end of data.
    // The stream reader will store only the data that we want, ignoring all
    // the cruft that is so prevalent in a modern web page.
    int status = http_perform_as_stream_reader(&stream_buf);
    if (status != 0) {
        // TODO: find a better way
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    char* new_buf = strip_comments(&stream_buf);    // removes HTML comments
    // Extract items from the buffer
	word_init(&word_of_day);
    word_extract(&word_of_day, new_buf);            // get the word
    type_extract(&word_of_day, new_buf);
    pronounce_extract(&word_of_day, new_buf);       // get the pronounciation
    define_extract(&word_of_day, new_buf);          // get the definition
    free(new_buf);  // created by calloc, must free

    // Wavesahre ePaper display code follows
    DEV_Module_Init();
    int display_width, display_height;
#ifdef CONFIG_EPD_29
    display_width = EPD_2IN9_HEIGHT;
    display_height = EPD_2IN9_WIDTH;
    EPD_2IN9_Init(EPD_2IN9_FULL);
    EPD_2IN9_Clear();
#else
    display_width = EPD_2IN7_V2_HEIGHT;
    display_height = EPD_2IN7_V2_WIDTH;
    EPD_2IN7_V2_Init();
    EPD_2IN7_V2_Clear();
#endif
    DEV_Delay_ms(500);

    //Create a new image cache
    UBYTE *BlackImage;
    UWORD Imagesize = ((display_height % 8 == 0)? (display_height / 8 ): (display_height / 8 + 1)) * display_width;
    if((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
        printf("Failed to allocate BlackImage...\r\n");
        // TODO: what to do now?
    }
    // setup a PAINT object (canvas) for the GUI_Paint functions - PAINT object is a Singleton
    // rotete 270
    Paint_NewImage(BlackImage, display_height, display_width, 270, WHITE);
#if 1   // redundant?
    // Assign the canvas to GUI_Paint functions
    Paint_SelectImage(BlackImage);
#endif
    Paint_Clear(WHITE);

    // Draw on the canvas
    uint16_t cWidth, cHeight, usedHeight;
    // Display the word
    sFONT* font = &Font24;
    cWidth = font->Width; cHeight = font->Height;
    Paint_DrawString_EN(2, 0, word_of_day.word, font, WHITE, BLACK);
    usedHeight = cHeight + 1;

    // Display the pronounciation
    font = &Font16;
    cWidth = font->Width; cHeight = font->Height;
#if 1   // for debugging datetime issues, set this to 0
    // Display type of word (e.g., noun, adjective) and pronounciation.
    // If both are longer than one line, then display on two lines.
    Paint_DrawString_EN(2, usedHeight, word_of_day.type, font, BLACK, WHITE);
    int column = cWidth * strlen(word_of_day.type) + 5;
    if ((column + cWidth + 5 + cWidth * strlen(word_of_day.pronounce)) < display_width) {
        Paint_DrawString_EN(column, usedHeight, "|", font, BLACK, WHITE);
        column += cWidth + 5;
        Paint_DrawString_EN(column, usedHeight, word_of_day.pronounce, font, BLACK, WHITE);
    }
    else {
        // must put pronounce on new line, because pronounce takes up too must space
        usedHeight += cHeight + 1;
        Paint_DrawString_EN(2, usedHeight, word_of_day.pronounce, font, BLACK, WHITE);
    }
#else
    // display datetime for debug purposes
    localtime = get_local_datetime(datetime_str, tz_posix);
    Paint_DrawString_EN(2, usedHeight, datetime_str, &Font16, BLACK, WHITE);
#endif
    usedHeight += cHeight + 1;

    // Display the definition. Limit it to the number of characters that remain on the screen.
    // Remaining characters depends on character width and number of rows as determined by font size.
    font = &Font12;
    cWidth = font->Width; cHeight = font->Height;
    // calculate number of chars per line and number of lines remaining on display
    // Note: display width and height are portrait orientation in Waveshare sample code,
    // I am using display in landscape.
    int nChar = display_width / (cWidth);
    int nRow  = (display_height - usedHeight) / (cHeight+1);    // used 40 for first line
    /* TODO: minor bug. 2.7 inch screen calculates to have w=33 for font12.
     * Definition is 107 characters, so it calculates to need 4 rows, but it only uses 3.
     * Oh, maybe the fixed font chars already include 1 pixel width.
     */
    ESP_LOGD(TAG, "definition max display: nRow=%d, nChar=%d\n", nRow, nChar);
    ESP_LOGD(TAG, "definition: len=%d, nRow=%d", strlen(word_of_day.define), (strlen(word_of_day.define)/nChar)+1);
    if (strlen(word_of_day.define) > nRow*nChar) word_of_day.define[nRow*nChar] = '\0';
    Paint_DrawString_EN(0, usedHeight, word_of_day.define, font, BLACK, WHITE);
    usedHeight += (cHeight + 1) * (strlen(word_of_day.define) / nChar + 1);

    // TODO: if a line remains, maybe display the datetime
    if ((usedHeight + cHeight) < display_height) {
        localtime = get_local_datetime(datetime_str, tz_posix);
        Paint_DrawString_EN(2, usedHeight, datetime_str, font, WHITE, BLACK);
    }

    // Send canvas to the display
#ifdef CONFIG_EPD_29
    EPD_2IN9_Display(BlackImage);
    DEV_Delay_ms(10000);    // TODO: why delay here?
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
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    wifi_init_sta();
    ESP_LOGI(TAG, "Connected to AP");

    // TODO: what if not using NTP or if NTP not accessible?
    // Supposedly can get time from DHCP.
    int sntp_status = sntp_setup();	// 0==OK

#if REFRESH_TEST==0
    // Fetch from website, extract word-of-the-day, write to display
    update_word();

    // Setup timer for next update, sometime after midnight: (day=0, hour=1, minute=5, absolute)
    uint64_t refresh_sec = calc_refresh_delay(0, 6, 0, 1);  // wake every 6 hours
#else
    test_refresh();
    uint64_t refresh_sec = calc_refresh_delay(0, 0, 4, 1);
#endif
    uint64_t refresh_microsec = refresh_sec * 1000000LL;

#ifdef USE_TIMER
    //esp_timer_handle_t publish_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = &timer_callback,
        .name = "update_timer"
    };

    // esp_timer_create has to be performed within a function - cannot be called in global area.
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &update_timer));
#ifdef ONE_SHOT
    // one-shot timer will restart itself in the timer callback
    ESP_LOGI("timer", "start_once, seconds=%llu", refresh_sec);
    ESP_ERROR_CHECK(esp_timer_start_once(update_timer, refresh_microsec));
#else
    ESP_LOGI("timer", "start_periodic");
    ESP_ERROR_CHECK(esp_timer_start_periodic(update_timer, REFRESH_INTERVAL));
#endif

#endif

#ifdef USE_SLEEP
	esp_wifi_stop();
	esp_netif_sntp_deinit();
	
    const int deep_sleep_sec = refresh_sec;
    ESP_LOGI(TAG, "Entering deep sleep for %d seconds", deep_sleep_sec);
    esp_deep_sleep(1000000LL * deep_sleep_sec);
	
#endif
}