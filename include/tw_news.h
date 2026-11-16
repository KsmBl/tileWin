#ifndef _TW_NEWS_H
#define _TW_NEWS_H
#include <stddef.h>

/*
 * NEWS.md of the data directory: what each release brought, the newest first,
 * a section for each ("## 1.6") with a line for each change ("- **Name** ...").
 * The About page shows the section of the version installed; the taskbar says
 * once after an update that there is one.
 */

/* The release of a version as the build writes it: "1.6" of "1.6-61f8de8 on main (...)". */
void tw_news_release(const char *version, char *out, size_t size);
/*
 * The changes of that release in NEWS.md (path NULL: the one of the data
 * directory), each without its "- "; NULL-terminated, to free with
 * tw_news_free. NULL when it has no section.
 */
char **tw_news_items(const char *path, const char *release);
void tw_news_free(char **items);
/* The bold name of a change, "Lists that scroll:" of "**Lists that scroll:** the ..." */
void tw_news_title(const char *item, char *out, size_t size);

#endif
