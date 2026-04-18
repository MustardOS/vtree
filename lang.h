#ifndef LANG_H
#define LANG_H

#define MAX_LANG_FILES  32
#define MAX_LANG_NAME   64
#define MAX_LANG_KEYS  300
#define MAX_LANG_KEY_LEN 64
#define MAX_LANG_VAL_LEN 256

void        lang_init(void);
void        lang_reload(void);
const char *tr(const char *key);

extern int  lang_file_count;
extern char lang_names[MAX_LANG_FILES][MAX_LANG_NAME];
extern int  current_lang_idx;

#endif
