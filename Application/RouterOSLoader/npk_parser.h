// SPDX-License-Identifier: GPL-3.0-only

#ifndef NPK_PARSER_H
#define NPK_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	NPK_STATUS_OK = 0,
	NPK_STATUS_INVALID_ARGUMENT,
	NPK_STATUS_NO_MEMORY,
	NPK_STATUS_TRUNCATED,
	NPK_STATUS_BAD_MAGIC,
	NPK_STATUS_BAD_FORMAT,
	NPK_STATUS_ZLIB_ERROR,
	NPK_STATUS_NO_CONTENT_CONTAINER,
	NPK_STATUS_NOT_FOUND,
	NPK_STATUS_NOT_A_FILE
} npk_status_t;

typedef enum {
	NPK_ENTRY_TYPE_OTHER = 0,
	NPK_ENTRY_TYPE_FILE,
	NPK_ENTRY_TYPE_DIR
} npk_entry_type_t;

typedef struct {
	char *name;
	uint16_t mode;
	uint32_t size;
	npk_entry_type_t type;
	const void *data;
} npk_entry_t;

typedef struct {
	const void *source_buffer;
	size_t source_size;
	void *content_buffer;
	size_t content_size;
	npk_entry_t *entries;
	size_t entry_count;
} npk_package_t;

npk_status_t npk_parse_buffer(const void *buffer, size_t buffer_size, npk_package_t **out_package);
npk_status_t npk_extract_file(const npk_package_t *package, const char *filename, void **out_data,
			      size_t *out_size);
void npk_package_free(npk_package_t *package);
void npk_file_buffer_free(void *buffer);
const char *npk_status_string(npk_status_t status);

#ifdef __cplusplus
}
#endif

#endif