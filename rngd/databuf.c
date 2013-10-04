/*
 Copyright (c) 2013, Nicos Panayides <nicosp@gmail.com>
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "databuf.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct DataBuffer {
	unsigned char *buffer;
	size_t capacity;
	size_t size;
	size_t beg_index;
	size_t end_index;
};

DataBuffer *data_buf_create(size_t capacity)
{
	DataBuffer *data_buf;

	data_buf = malloc(sizeof(DataBuffer));

	if (!data_buf) {
		return NULL;
	}

	memset(data_buf, 0, sizeof(DataBuffer));

	data_buf->buffer = malloc(capacity);
	if (!data_buf->buffer) {
		free(data_buf);
		return NULL;
	}

	/* Not required */
	memset(data_buf->buffer, 0, capacity);

	data_buf->capacity = capacity;

	return data_buf;
}

void data_buf_destroy(DataBuffer *buf)
{
	if (!buf) {
		return;
	}

	if (buf->buffer) {
		free(buf->buffer);
	}

	free(buf);
}

size_t data_buf_write(DataBuffer *buf, const unsigned char *data, size_t data_len)
{
	size_t capacity;
	size_t bytes_to_write;

	if (!data_len) return 0;

	capacity = buf->capacity;
	bytes_to_write = (data_len > capacity - buf->size)? capacity - buf->size: data_len;

 	/* We can write everything in one go */
	if (bytes_to_write <= capacity - buf->end_index) {
		memcpy(buf->buffer + buf->end_index, data, bytes_to_write);
		buf->end_index += bytes_to_write;
		if (buf->end_index == capacity) buf->end_index= 0;
  	} else {
		size_t size_1 = capacity - buf->end_index;
		size_t size_2 = bytes_to_write - size_1;

		memcpy(buf->buffer + buf->end_index, data, size_1);
		memcpy(buf->buffer, data + size_1, size_2);
		buf->end_index = size_2;
	}

	buf->size += bytes_to_write;

	return bytes_to_write;
}

size_t data_buf_available(const DataBuffer *buf)
{
	if (!buf) return 0;

	return buf->size;
}

size_t data_buf_space(const DataBuffer *buf)
{
	if (!buf) return 0;

	return buf->capacity - buf->size;
}


size_t data_buf_read(DataBuffer *buf, unsigned char *data, size_t data_len)
{
	size_t capacity;
	size_t bytes_to_read;

	if (!data_len) return 0;

	capacity = buf->capacity;
	bytes_to_read = (data_len > buf->size)?buf->size:data_len;


	/* Read in a single step */
	if (bytes_to_read <= capacity - buf->beg_index) {
		memcpy(data, buf->buffer + buf->beg_index, bytes_to_read);
		buf->beg_index += bytes_to_read;
  	} else {
		size_t size_1 = capacity - buf->beg_index;
		size_t size_2 = bytes_to_read - size_1;
		
		memcpy(data, buf->buffer + buf->beg_index, size_1);
		memcpy(data + size_1, buf->buffer, size_2);
		buf->beg_index = size_2;
  	}

	if (buf->beg_index == capacity) buf->beg_index = 0;
  	buf->size -= bytes_to_read;

	return bytes_to_read;
}

size_t data_buf_unread(DataBuffer *buf, const unsigned char *data, size_t data_len)
{
	size_t capacity;
	size_t bytes_to_write;
	const unsigned char *data_p;

	if (!data_len) return 0;

	capacity = buf->capacity;
	bytes_to_write = (data_len > capacity - buf->size)? capacity - buf->size: data_len;

	if (!bytes_to_write) return 0;

	/* Make sure we prefer to copy the last bytes to preserve the original order of data */
	data_p = (data+data_len) - bytes_to_write;

	/* Update the begin index */
	if (buf->beg_index >= bytes_to_write) {
		buf->beg_index = buf->beg_index - bytes_to_write;
	} else {
		buf->beg_index = buf->capacity - bytes_to_write;
	}

 	/* We can write everything in one go */
	if (bytes_to_write <= capacity - buf->beg_index) {
		memcpy(buf->buffer + buf->beg_index, data_p, bytes_to_write);
  	} else {
		size_t size_1 = capacity - buf->beg_index;
		size_t size_2 = bytes_to_write - size_1;

		memcpy(buf->buffer + buf->beg_index, data_p, size_1);
		memcpy(buf->buffer, data_p + size_1, size_2);
	}

	buf->size += bytes_to_write;

	return bytes_to_write;
}

