/* Fill STREAM_BUF with web page content, looking for start and end tags that delimit the content */
#include <stdio.h>
#include <malloc.h>
#include <stdbool.h>
#include <string.h>
#define LOG_LOCAL_LEVEL 4   // DEBUG for just this file
#include "esp_log.h"
#include "esp_err.h"
#include "stream_buf.h"
#include "word_site.h"

static bool part_match = false;	// partial match of either start or end
static bool found_start = false;	// start has been found
static bool found_end = false;		// end has been found
static const char *TAG = "stream_buf";

void stream_buf_init(STREAM_BUF *stream_buf, int max_buf_len)
{
	part_match = false;	// partial match of either start or end
	found_start = false;	// start has been found
	found_end = false;		// end has been found
	stream_buf->found_start = stream_buf->found_end = false;
	stream_buf->part_start = stream_buf->part_end = false;
    stream_buf->idx = stream_buf->buf_idx = stream_buf->good_idx = 0;
    strcpy(stream_buf->match_start, WORD_START);
    //strcpy(stream_buf->match_end, "See the entry ></a></p>");
    strcpy(stream_buf->match_end, WORD_END);
    stream_buf->buf_len = max_buf_len;
    stream_buf->buffer = (char*) calloc(max_buf_len, sizeof(char));
	stream_buf->ptr = stream_buf->buffer;
	strcpy(stream_buf->buffer, "not used");
}

int stream_buf_match(STREAM_BUF *stream_buf, char *buf, int blen, bool bStart)
{
	/* Look for match of either start or end of region of interest.
	 * If match is found, copy to stream_buf->
	 * buf: http stream buffer, may only be 512 bytes
	 * len: length of buf
	 * bStart: true if looking for start, false if looking for end
	 * return: 0 if match not found, 1 if found_start, 2 if found_end
	 * NOTE: when found_start==true then bStart should be set to false by calling routine
	 */
	int retval = 0;
	static char* match_ptr;
	esp_log_level_set(TAG, ESP_LOG_DEBUG);
	
	if (bStart) {
		// look for start match
		if (!part_match) {
			match_ptr = stream_buf->match_start;
		}
	}
	else {
		// look for end match
		if (!part_match) {
			match_ptr = stream_buf->match_end;
			ESP_LOGD(TAG, "1: match_ptr = %s", match_ptr);
		}
	}
	
	for (int i = 0; i < blen; i++) {
		if (*buf == *match_ptr) {
			ESP_LOGD(TAG, "match char = %c", *buf);
			if (!part_match) {
				// save stream_buf location in case we have to back out
				stream_buf->good_idx = stream_buf->idx;
			}
			part_match = true;
			stream_buf->buffer[stream_buf->idx] = *buf;
			stream_buf->idx++;
			ESP_ERROR_CHECK((stream_buf->idx >= STREAM_BUF_LEN) ? ESP_FAIL : ESP_OK);
			match_ptr++;
			buf++;
			if (*match_ptr == '\0') {
				// found the match we're looking for
				if (bStart) {
					// found start
					bStart = false;
					ESP_LOGI(TAG, "found match_start");
					part_match = false;
					found_start = true;
					stream_buf->good_idx = stream_buf->idx;
					match_ptr = stream_buf->match_end;
					ESP_LOGD(TAG, "2: match_ptr = %s", match_ptr);
				}
				else {
					// found end
					ESP_LOGI(TAG, "found match_end");
					part_match = false;
					found_end = true;
					stream_buf->buffer[stream_buf->idx] = '\0';
					break;
				}
			}
		}
		else {
			part_match = false;
			if (found_start) {
				// all chars copied to stream_buf until found_end==true
				stream_buf->buffer[stream_buf->idx++] = *buf;
				// ==== ERROR: we never found match_end. Print the buffer. ==== //
				if (false && stream_buf->idx >= STREAM_BUF_LEN) {
					stream_buf->buffer[stream_buf->idx - 1] = '\0';
					printf(stream_buf->buffer);
				}
				ESP_ERROR_CHECK((stream_buf->idx >= STREAM_BUF_LEN) ? ESP_FAIL : ESP_OK);
			}
			else {
				stream_buf->idx = stream_buf->good_idx;	// go back
			}
			if (bStart) {
				match_ptr = stream_buf->match_start;
			}
			else {
				match_ptr = stream_buf->match_end;
			}
			buf++;
		}
	}
	if (found_start) retval = 1;
	if (found_end) retval = 2;
	return retval;
}

#if 0
int main(void)
{
	// open HTML file and read it, 512 bytes at a time, as though we're streaming from HTTPS server
	char buf[512];
	const char* word_file = "merriam.html";
	char* web_buffer = (char*)calloc(RING_LEN, sizeof(char));
	int file_len = 0;
	bool bStart = true;
	FILE* fp = fopen(word_file, "r");
	stream_buf_init(web_buffer);

	// For each buffer look for start/end matches
	int read_stat;
	int match_stat;
	printf("Start read\n");
	while (1) {

		read_stat = fread(buf, 1, 512, fp);
		file_len += read_stat;
		// Look for match
		match_stat = stream_buf_match(buf, read_stat, bStart);
		if (match_stat == 1 && bStart) {
			// found match_start
			printf("found match_start at %d, while file_len = %d\n", stream_buf->idx, file_len);
			bStart = false;		// now look for match_end
		}
		if (read_stat < 512 || match_stat == 2) {
			// Reached end of file or found match_end
			printf("found match_end at %d, while file_len = %d\n", stream_buf->idx, file_len);
			fclose(fp);
			break;
		}
	}
	printf("Finish read: %d\n", file_len);
	printf("stream_buf len = %d\n", stream_buf->idx);
	printf("buffer len = %d\n%s\n", strlen(stream_buf->buffer), stream_buf->buffer);
	return 0;
}
#endif