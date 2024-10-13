#define STREAM_BUF_LEN 20000

typedef struct {
	int buf_len;
	int buf_idx;	// where to write next chunk of stream
	char* buffer;
	int idx;
	int good_idx;
	char* ptr;
	char match_start[40];
	char match_end[40];
	bool found_start, found_end;
	bool part_start, part_end;
} STREAM_BUF;

