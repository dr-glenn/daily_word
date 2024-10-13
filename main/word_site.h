
/* Store all chars between WORD_START and WORD_END in a buffer.
 * Toss all others chars away.
 * Inside the buffer find the word, pronounciation and definition.
 */
// URL for word of the day web site
#define WORD_URL "https://www.merriam-webster.com/word-of-the-day/"
// text that occurs before start of today's word
#define WORD_START "<h2 class=\"word-header-txt\">"
// text that occurs after the word definition
//#define WORD_END "See the entry ></a>"
#define WORD_END "See the entry"

/* www.dictionary.com/e/word-of-the-day
 * More difficult to parse than Merriam-Webster
 *
 * vocabulary.com/word-of-the-day
 * Does not spell out the pronounciation
 * Oh, it does if you follow the link to https://www.vocabulary.com/dictionary/kaleidoscope
 * 
 * https://www.britannica.com/dictionary/eb/word-of-the-day
 */