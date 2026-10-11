#pragma once

#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif

/* The player only needs zlib inflate/deflate. Skip ZIP, stdio, and timestamps. */
#ifndef MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#endif
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO
#endif
#ifndef MINIZ_NO_TIME
#define MINIZ_NO_TIME
#endif
