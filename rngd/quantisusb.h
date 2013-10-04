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

/**
 QuantisUSB Asynchronous library

*/

#ifndef _QUANTISUSB_H_
#define _QUANTISUSB_H_

#include <stddef.h>
#include <sys/time.h>

/* Export Macros */
#if defined(_WIN32)
#  if defined(QUANTISUSB_DLL)
#    define QUANTISUSB_PUBLIC __declspec(dllexport)
#  else
#    define QUANTISUSB_PUBLIC __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#    if __GNUC__ >= 4
#      define QUANTISUSB_PUBLIC __attribute__ ((visibility("default")))
#    endif
#endif

#if !defined(QUANTISUSB_PUBLIC)
#  define QUANTISUSB_PUBLIC
#endif

/* Private symbols */
#if defined(__GNUC__) && !defined(_WIN32)
#  if __GNUC__ >= 4
#    define QUANTISUSB_PRIVATE __attribute__ ((visibility("hidden")))
#  endif
#endif

#if !defined(QUANTISUSB_PRIVATE)
#  define QUANTISUSB_PRIVATE
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
* @defgroup QuantisUSBContext QuantisUSBContext
* Quantis USB context.
*/

/**
* @defgroup QuantisUSBDevice QuantisUSBDevice
* Quantis USB device.
*/


struct QuantisUSBContext;

/**
* @ingroup QuantisUSBContext
* @struct QuantisUSBContext <quantisusb.h>
* @ref QuantisUSBContext struct.
*/
typedef struct QuantisUSBContext QuantisUSBContext;

struct QuantisUSBDevice;

/**
* @ingroup QuantisUSBDevice
* @struct QuantisUSBDevice <quantisusb.h>
* @ref QuantisUSBDevice struct.
*/
typedef struct QuantisUSBDevice QuantisUSBDevice;

/**
* @addtogroup QuantisUSBContext
* @{
*/

/**
* Called when data is available from a device.
*/
typedef void (*QuantisUSBReadCallback) (QuantisUSBDevice *device, const unsigned char *data, int data_len);

/**
* Called when an error occurs when reading from the device. Also called when read is cancelled and
* errno will be set to ECANCELLED in that case.
*/
typedef void (*QuantisUSBErrorCallback) (QuantisUSBDevice *device);


/**
* Called when a device appears or disappears.
*/
typedef void (*QuantisUSBDeviceCallback) (QuantisUSBDevice *device, int present);

/**
* Called when a device is about to be opened.
* Return 0 to prevent the device from opening.
*/
typedef int (*QuantisUSBDeviceShouldOpenCallback) (QuantisUSBDevice *device);

/**
* Called when an error is logged. errno will be set accordingly so
* it is possible to use perror to get a more detailed message.
*/
typedef void (*QuantisUSBErrorLogger) (QuantisUSBContext *, const char *);

/**
* Initializes a new context.
*
*/
QUANTISUSB_PUBLIC QuantisUSBContext *quantis_usb_init(QuantisUSBReadCallback read_callback,
                                    QuantisUSBErrorCallback error_callback,
                                    QuantisUSBDeviceCallback device_callback,
				    QuantisUSBDeviceShouldOpenCallback should_open_callback,
				    QuantisUSBErrorLogger error_log,
                                    void *user_data);

/**
* Destroys the given context
*/
QUANTISUSB_PUBLIC void quantis_usb_destroy(QuantisUSBContext *context);

/**
* Enumerates all currently connected devices.
* Note: 
* Returns: 0 on success, -1 otherwise.
*/
QUANTISUSB_PUBLIC int quantis_usb_enumerate(QuantisUSBContext *context);

/**
* Enables device hotplug.
* Please note that enumeration takes place immediately if enumerate is non-zero.
*/
QUANTISUSB_PUBLIC int quantis_usb_enable_hotplug(QuantisUSBContext *context, int enumerate);

/**
* Disables device hotplug
*/
QUANTISUSB_PUBLIC int quantis_usb_disable_hotplug(QuantisUSBContext *context);

/**
* Gets the number of open devices.
* Note: You must have already called quantis_usb_enable_hotplug with enumerate 
* or quantis_usb_enumerate to get anything other than 0.
*/
QUANTISUSB_PUBLIC size_t quantis_usb_device_count(QuantisUSBContext *ctx);

/* Iterating through opened devices */

/**
* Gets the first device for the context. The first device is the device opened last.
*/
QUANTISUSB_PUBLIC QuantisUSBDevice *quantis_usb_get_first_device(QuantisUSBContext *ctx);


/**
* Reads data from all available devices.
*/
QUANTISUSB_PUBLIC void quantis_usb_read_all(QuantisUSBContext *context);


/**
* Polling function for applications not polling other file descriptors.
*/
QUANTISUSB_PUBLIC int quantis_usb_poll(QuantisUSBContext *context, struct timeval *timeout);

/**
* Must be called before calling select. The timeout passed may be modified
* by this function but it will never be higher than what is passed.
*
* This function assumes that all file descriptors in your select set are non-blocking.
*
* Returns: 0 on success, -1 otherwise.
*/
QUANTISUSB_PUBLIC int quantis_usb_before_poll(QuantisUSBContext *ctx, int *nfds,
                            fd_set *readfdset, fd_set *writefdset, fd_set *errorfdset,
                            struct timeval *timeout);

/**
* Must be called after calling select. This function will cause all pending events to be processed.
*
* @param ctx The context.
* @param timeout_expired Whether select returned due to a timeout. Usually !select_status.
* @param readfdset fd_set for read.
* @param writefdset fd_set for write.
* @param errorfdset fd_set for error.
*/
QUANTISUSB_PUBLIC int quantis_usb_after_poll(QuantisUSBContext *ctx, int timeout_expired,
                             const fd_set *readfdset, const fd_set *writefdset, const fd_set *errorfdset);


/**
* Gets the user data associated with the context.
*/
QUANTISUSB_PUBLIC void *quantis_usb_get_user_data(QuantisUSBContext *ctx);


/** @} */

/**
* Device funtions
*
* @addtogroup QuantisUSBDevice
* @{
*/

/**
* Gets the next device.
*/
QUANTISUSB_PUBLIC QuantisUSBDevice *quantis_usb_device_get_next(QuantisUSBDevice *device);

/**
* Gets the previous device.
*/
QUANTISUSB_PUBLIC QuantisUSBDevice *quantis_usb_device_get_prev(QuantisUSBDevice *device);

/**
* Closes the given device.
*/
QUANTISUSB_PUBLIC void quantis_usb_close_device(QuantisUSBDevice *device, QuantisUSBDevice *prev);

/**
* Gets the context for the given device.
*/
QUANTISUSB_PUBLIC QuantisUSBContext *quantis_usb_device_get_context(QuantisUSBDevice *device);

/**
 Requests data from the device.
*/
QUANTISUSB_PUBLIC int quantis_usb_read(QuantisUSBDevice *device);

/**
* Cancels read from the device.
*/
QUANTISUSB_PUBLIC int quantis_usb_read_cancel(QuantisUSBDevice *device);

/**
* Gets the serial number for the given device.
*
* Returns: 0 on success, -1 otherwise
*/
QUANTISUSB_PUBLIC int quantis_usb_get_serial_number(QuantisUSBDevice *device, char *buffer, int buffer_len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
