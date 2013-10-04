#ifndef _DATABUF_H_
#define _DATABUF_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DataBuffer;
typedef struct DataBuffer DataBuffer;

/**
* Creates a data buffer with the given capacity.
*/
DataBuffer *data_buf_create(size_t capacity);

/**
* Destroys a data buffer.
*/
void data_buf_destroy(DataBuffer *buf);

/**
* Returns number of bytes available for reading.
*/
size_t data_buf_available(const DataBuffer *buf);

/**
* Gets the number of bytes available for writing.
*/
size_t data_buf_space(const DataBuffer *buf);

/**
* Reads from a data buffer.
*
*
*/
size_t data_buf_read(DataBuffer *buf, unsigned char *data, size_t data_len);
size_t data_buf_unread(DataBuffer *buf, const unsigned char *data, size_t data_len);
size_t data_buf_write(DataBuffer *buf, const unsigned char *data, size_t data_len);

#ifdef __cplusplus
}
#endif


#endif
