// SPDX-License-Identifier: GPL-3.0-only

#include "npk_parser.h"

#include "uefi_libc.h"
#include <Library/ZlibLib.h>

#ifndef S_IFMT
#define S_IFMT 0170000
#endif
#ifndef S_IFDIR
#define S_IFDIR 0040000
#endif
#ifndef S_IFREG
#define S_IFREG 0100000
#endif

#define NPK_MAGIC_BYTES 0xBAD0F11E
#define NPK_ZLIB_COMPRESSED_DATA 4
#define NPK_HEADER_SIZE 8U
#define NPK_CONTAINER_HEADER_SIZE 6U
#define NPK_ZLIB_OBJECT_HEADER_SIZE 30U

static uint16_t read_le16(const uint8_t *buffer)
{
	return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t read_le32(const uint8_t *buffer)
{
	return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
	       ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

static npk_entry_type_t npk_entry_type_from_mode(uint16_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return NPK_ENTRY_TYPE_DIR;
	case S_IFREG:
		return NPK_ENTRY_TYPE_FILE;
	default:
		return NPK_ENTRY_TYPE_OTHER;
	}
}

static void npk_free_entries(npk_entry_t *entries, size_t entry_count)
{
	size_t index;

	if (entries == NULL)
		return;

	for (index = 0; index < entry_count; index++)
		free(entries[index].name);

	free(entries);
}

static npk_status_t npk_inflate_payload(const uint8_t *input, size_t input_len, uint8_t **output,
					size_t *output_len)
{
	z_stream stream;
	uint8_t *buffer;
	size_t capacity;
	int ret;

	if (output == NULL || output_len == NULL)
		return NPK_STATUS_INVALID_ARGUMENT;

	capacity = input_len * 4;
	if (capacity < 4096)
		capacity = 4096;

	buffer = malloc(capacity);
	if (buffer == NULL)
		return NPK_STATUS_NO_MEMORY;

	memset(&stream, 0, sizeof(stream));
	stream.zalloc = zcalloc;
	stream.zfree = zcfree;
	stream.opaque = Z_NULL;
	stream.next_in = (Bytef *)input;
	stream.avail_in = input_len;

	ret = inflateInit(&stream);
	if (ret != Z_OK) {
		free(buffer);
		return NPK_STATUS_ZLIB_ERROR;
	}

	for (;;) {
		if (stream.total_out == capacity) {
			uint8_t *grown;

			capacity *= 2;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				inflateEnd(&stream);
				free(buffer);
				return NPK_STATUS_NO_MEMORY;
			}
			buffer = grown;
		}

		stream.next_out = buffer + stream.total_out;
		stream.avail_out = capacity - stream.total_out;

		ret = inflate(&stream, Z_NO_FLUSH);
		if (ret == Z_STREAM_END)
			break;
		if (ret != Z_OK) {
			inflateEnd(&stream);
			free(buffer);
			return NPK_STATUS_ZLIB_ERROR;
		}
	}

	*output_len = stream.total_out;
	inflateEnd(&stream);

	if (*output_len == 0) {
		free(buffer);
		buffer = NULL;
	} else {
		uint8_t *shrunk;

		shrunk = realloc(buffer, *output_len);
		if (shrunk != NULL)
			buffer = shrunk;
	}

	*output = buffer;
	return NPK_STATUS_OK;
}

static npk_status_t npk_append_entry(npk_package_t *package, const uint8_t *name_ptr,
				     uint16_t name_len, uint16_t mode,
				     const uint8_t *payload_ptr, uint32_t payload_len)
{
	npk_entry_t *grown_entries;
	char *name;
	npk_entry_t *entry;

	grown_entries = realloc(package->entries, (package->entry_count + 1) * sizeof(*grown_entries));
	if (grown_entries == NULL)
		return NPK_STATUS_NO_MEMORY;

	package->entries = grown_entries;

	name = malloc((size_t)name_len + 1);
	if (name == NULL)
		return NPK_STATUS_NO_MEMORY;

	memcpy(name, name_ptr, name_len);
	name[name_len] = '\0';

	entry = &package->entries[package->entry_count++];
	entry->name = name;
	entry->mode = mode;
	entry->size = payload_len;
	entry->type = npk_entry_type_from_mode(mode);
	entry->data = payload_ptr;

	return NPK_STATUS_OK;
}

static npk_status_t npk_parse_entries(npk_package_t *package)
{
	const uint8_t *buffer = package->content_buffer;
	size_t buffer_len = package->content_size;
	size_t offset = 0;

	while (offset < buffer_len) {
		uint16_t mode;
		uint32_t payload_len;
		uint16_t name_len;
		size_t required;
		const uint8_t *name_ptr;
		const uint8_t *payload_ptr;
		npk_status_t status;

		if (buffer_len - offset < NPK_ZLIB_OBJECT_HEADER_SIZE)
			return NPK_STATUS_TRUNCATED;

		mode = read_le16(buffer + offset);
		payload_len = read_le32(buffer + offset + 24);
		name_len = read_le16(buffer + offset + 28);
		required = NPK_ZLIB_OBJECT_HEADER_SIZE + (size_t)name_len + (size_t)payload_len;
		if (required > buffer_len - offset)
			return NPK_STATUS_TRUNCATED;

		name_ptr = buffer + offset + NPK_ZLIB_OBJECT_HEADER_SIZE;
		payload_ptr = name_ptr + name_len;

		status = npk_append_entry(package, name_ptr, name_len, mode, payload_ptr, payload_len);
		if (status != NPK_STATUS_OK)
			return status;

		offset += required;
	}

	return NPK_STATUS_OK;
}

static npk_status_t npk_find_and_inflate_content(const uint8_t *buffer, size_t buffer_size,
					 npk_package_t *package)
{
	uint32_t payload_len;
	size_t offset = NPK_HEADER_SIZE;
	size_t payload_end;

	payload_len = read_le32(buffer + 4);
	if ((size_t)payload_len > buffer_size - NPK_HEADER_SIZE)
		return NPK_STATUS_TRUNCATED;

	payload_end = NPK_HEADER_SIZE + (size_t)payload_len;

	while (offset < payload_end) {
		uint16_t container_id;
		uint32_t container_payload_len;
		size_t container_size;
		npk_status_t status;

		if (payload_end - offset < NPK_CONTAINER_HEADER_SIZE)
			return NPK_STATUS_TRUNCATED;

		container_id = read_le16(buffer + offset);
		container_payload_len = read_le32(buffer + offset + 2);
		container_size = NPK_CONTAINER_HEADER_SIZE + (size_t)container_payload_len;
		if (container_size > payload_end - offset)
			return NPK_STATUS_TRUNCATED;

		if (container_id == NPK_ZLIB_COMPRESSED_DATA) {
			status = npk_inflate_payload(buffer + offset + NPK_CONTAINER_HEADER_SIZE,
					     container_payload_len,
					     (uint8_t **)&package->content_buffer,
					     &package->content_size);
			if (status != NPK_STATUS_OK)
				return status;

			return npk_parse_entries(package);
		}

		offset += container_size;
	}

	return NPK_STATUS_NO_CONTENT_CONTAINER;
}

npk_status_t npk_parse_buffer(const void *buffer, size_t buffer_size, npk_package_t **out_package)
{
	const uint8_t *bytes = buffer;
	npk_package_t *package;
	npk_status_t status;

	if (out_package != NULL)
		*out_package = NULL;

	if (buffer == NULL || out_package == NULL || buffer_size < NPK_HEADER_SIZE)
		return NPK_STATUS_INVALID_ARGUMENT;

	if (read_le32(bytes) != NPK_MAGIC_BYTES)
		return NPK_STATUS_BAD_MAGIC;

	package = calloc(1, sizeof(*package));
	if (package == NULL)
		return NPK_STATUS_NO_MEMORY;

	package->source_buffer = buffer;
	package->source_size = buffer_size;

	status = npk_find_and_inflate_content(bytes, buffer_size, package);
	if (status != NPK_STATUS_OK) {
		npk_package_free(package);
		return status;
	}

	*out_package = package;
	return NPK_STATUS_OK;
}

npk_status_t npk_extract_file(const npk_package_t *package, const char *filename, void **out_data,
			      size_t *out_size)
{
	size_t index;

	if (package == NULL || filename == NULL || out_data == NULL || out_size == NULL)
		return NPK_STATUS_INVALID_ARGUMENT;

	*out_data = NULL;
	*out_size = 0;

	for (index = 0; index < package->entry_count; index++) {
		const npk_entry_t *entry = &package->entries[index];
		void *copy;

		if (strcmp(entry->name, filename) != 0)
			continue;
		if (entry->type != NPK_ENTRY_TYPE_FILE)
			return NPK_STATUS_NOT_A_FILE;
		if (entry->size == 0)
			return NPK_STATUS_OK;

		copy = malloc(entry->size);
		if (copy == NULL)
			return NPK_STATUS_NO_MEMORY;

		memcpy(copy, entry->data, entry->size);
		*out_data = copy;
		*out_size = entry->size;
		return NPK_STATUS_OK;
	}

	return NPK_STATUS_NOT_FOUND;
}

void npk_package_free(npk_package_t *package)
{
	if (package == NULL)
		return;

	npk_free_entries(package->entries, package->entry_count);
	free(package->content_buffer);
	free(package);
}

void npk_file_buffer_free(void *buffer)
{
	free(buffer);
}

const char *npk_status_string(npk_status_t status)
{
	switch (status) {
	case NPK_STATUS_OK:
		return "ok";
	case NPK_STATUS_INVALID_ARGUMENT:
		return "invalid argument";
	case NPK_STATUS_NO_MEMORY:
		return "out of memory";
	case NPK_STATUS_TRUNCATED:
		return "truncated input";
	case NPK_STATUS_BAD_MAGIC:
		return "bad magic";
	case NPK_STATUS_BAD_FORMAT:
		return "bad format";
	case NPK_STATUS_ZLIB_ERROR:
		return "zlib error";
	case NPK_STATUS_NO_CONTENT_CONTAINER:
		return "no content container";
	case NPK_STATUS_NOT_FOUND:
		return "not found";
	case NPK_STATUS_NOT_A_FILE:
		return "not a regular file";
	default:
		return "unknown";
	}
}