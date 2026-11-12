#pragma once
#include <sys/types.h>
#include <cstdint>
struct archive;
struct archive_entry;
#define ARCHIVE_OK 0
#define ARCHIVE_WARN (-20)
#define ARCHIVE_EOF 1
#define AE_IFDIR 0040000
#define AE_IFREG 0100000
archive* archive_read_new();
int archive_read_support_filter_all(archive*);
int archive_read_support_format_all(archive*);
int archive_read_open_filename(archive*, const char*, size_t);
const char* archive_error_string(archive*);
int archive_read_free(archive*);
int archive_read_next_header(archive*, archive_entry**);
int archive_read_data_skip(archive*);
ssize_t archive_read_data(archive*, void*, size_t);
int64_t archive_filter_bytes(archive*, int);
const char* archive_entry_pathname(archive_entry*);
unsigned archive_entry_filetype(archive_entry*);
