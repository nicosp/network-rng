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

/*
* Asynchronous library for Quantis USB HW RNGs.
*/

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/time.h>
#include <libusb.h>

#include "quantisusb.h"

/*
* lsusb output from Quantis USB.
*
Device Descriptor:
  bLength                18
  bDescriptorType         1
  bcdUSB               2.00
  bDeviceClass          255 Vendor Specific Class
  bDeviceSubClass         0 
  bDeviceProtocol         0 
  bMaxPacketSize0        64
  idVendor           0x0aba Ellisys
  idProduct          0x0102 
  bcdDevice            2.00
  iManufacturer           1 id Quantique
  iProduct                2 Quantis USB
  iSerial                 3 FFFFFFFFFF
  bNumConfigurations      1
  Configuration Descriptor:
    bLength                 9
    bDescriptorType         2
    wTotalLength           25
    bNumInterfaces          1
    bConfigurationValue     1
    iConfiguration          0 
    bmAttributes         0x80
      (Bus Powered)
    MaxPower              300mA
    Interface Descriptor:
      bLength                 9
      bDescriptorType         4
      bInterfaceNumber        0
      bAlternateSetting       0
      bNumEndpoints           1
      bInterfaceClass       255 Vendor Specific Class
      bInterfaceSubClass      0 
      bInterfaceProtocol      0 
      iInterface              0 
      Endpoint Descriptor:
        bLength                 7
        bDescriptorType         5
        bEndpointAddress     0x86  EP 6 IN
        bmAttributes            2
          Transfer Type            Bulk
          Synch Type               None
          Usage Type               Data
        wMaxPacketSize     0x0200  1x 512 bytes
        bInterval               0
Device Qualifier (for other device speed):
  bLength                10
  bDescriptorType         6
  bcdUSB               2.00
  bDeviceClass          255 Vendor Specific Class
  bDeviceSubClass         0 
  bDeviceProtocol         0 
  bMaxPacketSize0        64
  bNumConfigurations      1
Device Status:     0x0000
  (Bus Powered)

*/

/* USB Vendor Id */
#define VENDOR_ID_ELLISYS 0x0aba

/* USB Device Id */
#define DEVICE_ID_QUANTIS_USB 0x0102

/* USB Device class */
#define USB_DEVICE_CLASS LIBUSB_CLASS_VENDOR_SPEC

#define USB_DEVICE_CONFIGURATION 1


struct QuantisPollFd {
	int fd;
	short events;
};

typedef struct QuantisPollFd QuantisPollFd;

struct QuantisUSBContext {
	libusb_context* ctx;
	libusb_hotplug_callback_handle hotplug_handle;
	int hotplug_ref;

	void *user_data;
	QuantisUSBReadCallback read_callback;
	QuantisUSBErrorCallback error_callback;
	QuantisUSBDeviceCallback device_callback;
	QuantisUSBDeviceShouldOpenCallback should_open_callback;
	QuantisUSBErrorLogger error_log;

	size_t device_count;
	QuantisUSBDevice *devices;

	int usb_timeout_effective;
	int usb_events_available;

	size_t poll_fds_length;
	size_t poll_fds_count;
	QuantisPollFd *poll_fds;
};

struct QuantisUSBDevice {
	QuantisUSBContext *context;
	struct QuantisUSBDevice *next;

	struct libusb_device_descriptor desc;
	libusb_device_handle *device_handle;

	uint8_t endpoint_address;
	unsigned int max_packet_size;
	struct libusb_transfer *transfer;
	int read_in_progress;
};


static QuantisUSBDevice *quantis_usb_open_device(QuantisUSBContext *ctx, libusb_device *dev);

static int hotplug_callback(struct libusb_context *ctx, struct libusb_device *dev,
                     libusb_hotplug_event event, void *user_data);

static void usb_transfer_set_errno(enum libusb_transfer_status status)
{
	switch(status) {
		case LIBUSB_TRANSFER_COMPLETED:
			errno = 0;
			break;
		case LIBUSB_TRANSFER_ERROR:
			errno = EIO;
			break;
		case LIBUSB_TRANSFER_TIMED_OUT:
			errno = ETIMEDOUT;
			break;
		case LIBUSB_TRANSFER_CANCELLED:
			errno = ECANCELED;
			break;
		case LIBUSB_TRANSFER_STALL:
			errno = EIO;
			break;
		case LIBUSB_TRANSFER_NO_DEVICE:
			errno = ENODEV;
			break;
		case LIBUSB_TRANSFER_OVERFLOW:
			errno = EIO;
			break;
		default:
			errno = EIO;
			break;
	}
}


static void usb_set_errno(enum libusb_error status)
{
	switch(status) {
		case LIBUSB_SUCCESS:
			errno = 0;
			break;
		case LIBUSB_ERROR_IO:
			errno = EIO;
			break;
		case LIBUSB_ERROR_NO_MEM:
			errno = ENOMEM;
			break;
		case LIBUSB_ERROR_ACCESS:
			errno = EACCES;
			break;
		case LIBUSB_ERROR_NO_DEVICE:
			errno = ENODEV;
			break;
		case LIBUSB_ERROR_BUSY:
			errno = EBUSY;
			break;
		case LIBUSB_ERROR_TIMEOUT:
			errno = ETIMEDOUT;
			break;
		case LIBUSB_ERROR_NOT_SUPPORTED:
			errno = ENOTSUP;
			break;
		case LIBUSB_ERROR_INTERRUPTED:
			errno = EINTR;
			break;
		default:
			errno = EIO;
			break;
	}
}

static void quantis_ctx_log_error(QuantisUSBContext *ctx, const char *msg)
{
	if (ctx->error_log) {
		ctx->error_log(ctx, msg);
	} else {
		perror(msg);
	}
}


static void quantis_ctx_pollfd_added_cb(int fd, short events, void *user_data)
{
	QuantisUSBContext *ctx;
	QuantisPollFd *poll_fds;

	ctx = (QuantisUSBContext*)user_data;

	/* Array full or not allocated */
	if (ctx->poll_fds_length == ctx->poll_fds_count) {
		poll_fds = realloc(ctx->poll_fds, sizeof(QuantisPollFd) * (ctx->poll_fds_length + 16));
		if (!poll_fds) {
			quantis_ctx_log_error(ctx, "Unable to allocate memory for poll_fds");
			return;
		}

		ctx->poll_fds = poll_fds;
		ctx->poll_fds_length += 16;

	}

	ctx->poll_fds[ctx->poll_fds_count].fd = fd;
	ctx->poll_fds[ctx->poll_fds_count].events = events;
	ctx->poll_fds_count++;
}

static void quantis_ctx_pollfd_removed_cb(int fd, void *user_data)
{
	QuantisUSBContext *ctx;
	size_t i;

	ctx = (QuantisUSBContext*)user_data;

	for (i=0; i < ctx->poll_fds_count; i++) {
		if (ctx->poll_fds[i].fd == fd) {
			ctx->poll_fds_count--;
			memmove(&ctx->poll_fds[i], &ctx->poll_fds[i+1], (ctx->poll_fds_count - i) * sizeof(QuantisPollFd));
			break;
		}
	}
}


static void quantis_ctx_init_pollfds(QuantisUSBContext *ctx)
{
	int i;
	const struct libusb_pollfd **usb_poll_fds;
	const struct libusb_pollfd *poll_fd;

	usb_poll_fds = libusb_get_pollfds(ctx->ctx);
	if (usb_poll_fds) {
		/* Add the fd sets */
		for (i=0; (poll_fd = usb_poll_fds[i]) != NULL; i++) {
			if (poll_fd->fd < 0) {
				continue;
			}

			quantis_ctx_pollfd_added_cb(poll_fd->fd, poll_fd->events, ctx);
		}
	}

	free(usb_poll_fds);

	libusb_set_pollfd_notifiers(ctx->ctx,
		quantis_ctx_pollfd_added_cb,
		quantis_ctx_pollfd_removed_cb,
		ctx);
}

void *quantis_usb_get_user_data(QuantisUSBContext *ctx)
{
	if (!ctx) return NULL;

	return ctx->user_data;
}

int quantis_usb_enable_hotplug(QuantisUSBContext *context, int enumerate)
{
	int status;

	if (!context) {
		return -1;
	}

	if (context->hotplug_handle) {
		context->hotplug_ref++;
		return 0;
	}

	status = libusb_hotplug_register_callback(context->ctx,
			LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED|LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
			(enumerate? LIBUSB_HOTPLUG_ENUMERATE : 0),
			VENDOR_ID_ELLISYS,
			DEVICE_ID_QUANTIS_USB,
			USB_DEVICE_CLASS,
			hotplug_callback,
			context,
			&context->hotplug_handle);

	if (status < 0) {
		usb_set_errno(status);
		return -1;
	}

	context->hotplug_ref++;
	return 0;
}

int quantis_usb_disable_hotplug(QuantisUSBContext *context)
{
	if (!context || !context->hotplug_ref) {
		return -1;
	}

	context->hotplug_ref--;

	if (!context->hotplug_ref && context->hotplug_handle) {
		libusb_hotplug_deregister_callback(context->ctx, context->hotplug_handle);
		context->hotplug_handle = 0;
	}

	return 0;
}

int quantis_usb_enumerate(QuantisUSBContext *context)
{
	ssize_t n_devices;
	struct libusb_device **device_list = NULL;
	struct libusb_device* dev;
	int i;
	struct libusb_device_descriptor desc;


	n_devices = libusb_get_device_list(context->ctx, &device_list);
	if (n_devices < 0) {
		usb_set_errno(n_devices);
		return -1;
	}

	for (i = 0; device_list[i] != NULL; i++) {
		dev = device_list[i];
		memset(&desc, 0, sizeof(desc));

		if (libusb_get_device_descriptor(dev, &desc) < 0) {
			continue;
		}

		if ((desc.idVendor != VENDOR_ID_ELLISYS) || (desc.idProduct == DEVICE_ID_QUANTIS_USB)) {
			continue;
		}

		quantis_usb_open_device(context, dev);
	}
	

	libusb_free_device_list(device_list, 1);

	return 0;
}


QuantisUSBContext *quantis_usb_init(QuantisUSBReadCallback read_callback, QuantisUSBErrorCallback error_callback, QuantisUSBDeviceCallback device_callback,
					QuantisUSBDeviceShouldOpenCallback should_open_callback, QuantisUSBErrorLogger error_log,
					void *user_data)
{
	QuantisUSBContext *ctx;

	ctx = malloc(sizeof(struct QuantisUSBContext));
	if (!ctx) {
		errno = ENOMEM;
		return NULL;
	}
	memset(ctx, 0, sizeof(struct QuantisUSBContext));

	ctx->read_callback = read_callback;
	ctx->error_callback = error_callback;
	ctx->device_callback = device_callback;
	ctx->should_open_callback = should_open_callback;
	ctx->error_log = error_log;
	ctx->user_data = user_data;

	if (libusb_init(&ctx->ctx) != LIBUSB_SUCCESS) {
		free(ctx);
		return NULL;
	}

	quantis_ctx_init_pollfds(ctx);

	return ctx;
}

void quantis_usb_destroy(QuantisUSBContext *ctx)
{
	if (!ctx) return;

	/* Remove pollfd notifiers. We will free them anyway */
	libusb_set_pollfd_notifiers(ctx->ctx,
		NULL,
		NULL,
		NULL);

	if (ctx->poll_fds) {
		free(ctx->poll_fds);
	}

	/* Stop hotplug */
	if (ctx->hotplug_ref) {
		quantis_usb_disable_hotplug(ctx);
	}

	/* Close all devices */
	while (ctx->devices) {
		QuantisUSBDevice *curr;
		
		curr = ctx->devices;
		ctx->devices = ctx->devices->next;
		quantis_usb_close_device(curr, NULL);
	}	

	if (ctx->ctx) {
		libusb_exit(ctx->ctx);
	}

	free(ctx);
}

static int usb_process(QuantisUSBContext *ctx)
{
	struct timeval zero_tv;
	int status;

	memset(&zero_tv, 0, sizeof(struct timeval));

	status = libusb_handle_events_locked(ctx->ctx, &zero_tv);

	if (status) {
		usb_set_errno(status);
		return -1;
	}

	return 0;
}

int quantis_usb_before_poll(QuantisUSBContext *ctx, int *nfds, fd_set *readfdset, fd_set *writefdset, fd_set *errorfdset, struct timeval *timeout)
{
	int status;
	struct timeval tv;
	int i;

	ctx->usb_timeout_effective = 0;
	ctx->usb_events_available = 0;

	if (!libusb_pollfds_handle_timeouts(ctx->ctx)) {
		status = libusb_get_next_timeout(ctx->ctx, &tv);
		if (status < 0) {
			usb_set_errno(status);
			return -1;
		}

		/* Timeout pending */
		if (status > 0) {

			/* Events are already available. Set the timeout to zero and set flag to process them immediately in after_poll
	                   without selecting any libusb fds. This allows us to never process events in this function to play nice with
			   async applications that are able to process events only after select.
		        */
			if (!tv.tv_sec && !tv.tv_usec) {
				if(timeout) {
					memset(timeout, 0, sizeof(struct timeval));
				}
				ctx->usb_events_available = 1;
				return 0;
			}

			/* Make sure the timeout is lower than or equal to libusb timeout. */
			if (timeout) {
				if (tv.tv_sec < timeout->tv_sec 
				    || (tv.tv_sec == timeout->tv_sec && tv.tv_usec < timeout->tv_usec)) {
					ctx->usb_timeout_effective = 1;
					memcpy(timeout, &tv, sizeof(struct timeval));
				}
			}
		}
	}

	for (i=0; i < ctx->poll_fds_count; i++) {
		if (ctx->poll_fds[i].fd >= (*nfds)) {
			*nfds = ctx->poll_fds[i].fd + 1;
		}

		if (ctx->poll_fds[i].events & POLLIN) {
			FD_SET(ctx->poll_fds[i].fd, readfdset);
		}

		if (ctx->poll_fds[i].events & POLLOUT) {
			FD_SET(ctx->poll_fds[i].fd, writefdset);
		}

		if (ctx->poll_fds[i].events & POLLERR) {
			FD_SET(ctx->poll_fds[i].fd, errorfdset);
		}
	}

	return 0;
}

int quantis_usb_after_poll(QuantisUSBContext *ctx, int timeout_expired, const fd_set *readfdset, const fd_set *writefdset, const fd_set *errorfdset)
{
	int has_events;
	int i;

	/* Check if we must process events regardsless of the fd_sets. This needs to happen when:
         * - libusb indicated we have events available in before_poll (no timeout and no libusb fds were selected).
         * - The timeout requested from libusb has expired
	*/
	if (ctx->usb_events_available || (timeout_expired && ctx->usb_timeout_effective)) {
		return usb_process(ctx);
	}

	/* Check if any of libusb file descriptors have events */
	has_events = 0;
	for (i=0; i < ctx->poll_fds_count; i++) {

		if (FD_ISSET(ctx->poll_fds[i].fd, readfdset)) {
			has_events = 1;
			break;
		}

		if (FD_ISSET(ctx->poll_fds[i].fd, writefdset)) {
			has_events = 1;
			break;
		}

		if (FD_ISSET(ctx->poll_fds[i].fd, errorfdset)) {
			has_events = 1;
			break;
		}
	}

	if (has_events) {
		return usb_process(ctx);
	}

	return 0;
}

int quantis_usb_poll(QuantisUSBContext *context, struct timeval *timeout)
{
	int nfds;
	fd_set readfds;
	fd_set writefds;
	fd_set errorfds;
	int status;

	nfds = 0;
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_ZERO(&errorfds);

	status = quantis_usb_before_poll(context, &nfds, &readfds, &writefds, &errorfds, timeout);
	if (status) {
		return status;
	}

	status = select(nfds, &readfds, &writefds, NULL, timeout);
	if (status < 0) {
		return status;
	}

	return quantis_usb_after_poll(context, !status, &readfds, &writefds, &errorfds);
}

size_t quantis_usb_device_count(QuantisUSBContext *ctx)
{
	if (!ctx) return 0;

	return ctx->device_count;
}


static void read_callback(struct libusb_transfer *transfer)
{
	QuantisUSBDevice *device;
	QuantisUSBContext *context;

	device = (QuantisUSBDevice *)transfer->user_data;
	context = device->context;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		if (context->read_callback) {
			context->read_callback(device, transfer->buffer, transfer->actual_length);
		}
	} else {
		usb_transfer_set_errno(transfer->status);

		if (context->error_callback) {			
			context->error_callback(device);
		}
	}

	device->read_in_progress = 0;
}

void quantis_usb_read_all(QuantisUSBContext *context)
{
	QuantisUSBDevice *device;
	QuantisUSBDevice *prev = NULL;
	QuantisUSBDevice *next;
	int read_status;

	for (device = context->devices; device;) {
		read_status = quantis_usb_read(device);

		if (read_status < 0 && errno != EAGAIN && errno != EINTR) {
			quantis_ctx_log_error(context, "quantisusb read error");
			next = device->next;
			quantis_usb_close_device(device, prev);
			device = next;
			continue;
		}

		prev = device;
		device = device->next;
	}
}

int quantis_usb_read(QuantisUSBDevice *device)
{
	int status;

	if (!device || !device->transfer) {
		return -1;
	}

	if (device->read_in_progress) {
		errno = EAGAIN;
		return -1;
	}

	device->read_in_progress = 1;

	status = libusb_submit_transfer(device->transfer);
	if (status) {
		device->read_in_progress = 0;
		usb_set_errno(status);
		return -1;
	}

	return 0;
}

int quantis_usb_read_cancel(QuantisUSBDevice *device)
{
	int status;

	if (!device) {
		errno = EINVAL;
		return -1;
	}

	if (!device->read_in_progress) {
		return 0;
	}

	status = libusb_cancel_transfer(device->transfer);

	if (status == LIBUSB_ERROR_NOT_FOUND) {
		return 0;
	}

	if (status) {
		usb_set_errno(status);
	}

	return 0;
}

int quantis_usb_get_serial_number(QuantisUSBDevice *device, char *buffer, int buffer_len)
{
	int status;

	status = libusb_get_string_descriptor_ascii(device->device_handle,
                                              device->desc.iSerialNumber,
                                              (unsigned char*)buffer,
                                              buffer_len);

	if (status < 0) {
		usb_set_errno(status);
		status = -1;
	}

	return status;
}

static struct libusb_transfer *quantis_usb_create_transfer(QuantisUSBDevice *device)
{
	unsigned char *buffer;
	size_t buffer_len;

	buffer_len = device->max_packet_size * 16;

	/* Paranoia: Make sure the length fits into an int 
           and it's still a multiple of max_packet_size
        */
	while (buffer_len > INT_MAX) {
		buffer_len -= device->max_packet_size;
	}

	buffer = malloc(buffer_len);

	if (!buffer) {
		errno = ENOMEM;
		return NULL;
	}

	memset(buffer, 0, buffer_len);

	device->transfer = libusb_alloc_transfer(0);
	if (!device->transfer) {
		free(buffer);
		errno = ENOMEM;
		return NULL;
	}

	libusb_fill_bulk_transfer(device->transfer, device->device_handle, device->endpoint_address,
	buffer,
	(int)buffer_len,
	read_callback,
	device,
	0);

	return device->transfer;
}

QuantisUSBContext *quantis_usb_device_get_context(QuantisUSBDevice *device)
{
	if (!device) return NULL;

	return device->context;
}

QuantisUSBDevice *quantis_usb_get_first_device(QuantisUSBContext *ctx)
{
	if (!ctx) return NULL;

	return ctx->devices;
}

QuantisUSBDevice *quantis_usb_device_get_next(QuantisUSBDevice *device)
{
	if (!device) return NULL;

	return device->next;
}

QuantisUSBDevice *quantis_usb_device_get_prev(QuantisUSBDevice *device)
{	
	QuantisUSBDevice *prev;

	if (!device || device == device->context->devices) return NULL;

	for (prev = device->context->devices; prev && prev->next != device; prev = prev->next) {}

	return prev;
}

static void quantis_usb_destroy_device(QuantisUSBDevice *device)
{
	if (device->device_handle) {
		libusb_release_interface(device->device_handle, 0);
		libusb_close(device->device_handle);
	}

	if (device->transfer) {
		if (device->transfer->buffer) {
			free(device->transfer->buffer);
			device->transfer->buffer = NULL;
		}

		libusb_free_transfer(device->transfer);
		device->transfer = NULL;
	}

	free(device);
}

void quantis_usb_close_device(QuantisUSBDevice *device, QuantisUSBDevice *prev)
{
	if (!device) return;

	if (device->read_in_progress) {
		quantis_usb_read_cancel(device);
	}

	if (prev) {
		prev->next = device->next;
	} else {
		device->context->devices = device->next;
	}

	device->context->device_count--;

	if (device->context->device_callback) {
		device->context->device_callback(device, 0);
	}

	quantis_usb_destroy_device(device);
}


static QuantisUSBDevice *quantis_usb_open_device(QuantisUSBContext *ctx, libusb_device *dev)
{
	enum libusb_error status;
	int device_configuration;
	struct QuantisUSBDevice *device;
	struct libusb_config_descriptor *usbConfig = NULL;
	const struct libusb_endpoint_descriptor *endpoint;

	device = malloc(sizeof(struct QuantisUSBDevice));

	if (!device) {
		errno = ENOMEM;
		return NULL;
	}

	memset(device, 0, sizeof(struct QuantisUSBDevice));
	device->context = ctx;

	status = libusb_get_device_descriptor(dev, &device->desc);
	if (status) {
		usb_set_errno(status);
		free(device);
		quantis_ctx_log_error(ctx, "libusb_get_device_descriptor");
		return NULL;
	}

	/* Check that the device ids are correct */
	if ((device->desc.idVendor != VENDOR_ID_ELLISYS) || (device->desc.idProduct != DEVICE_ID_QUANTIS_USB)) {
		errno = EINVAL;
		free(device);
      		return NULL;
	}

	/* Make sure there is only one configuration */
	if (device->desc.bNumConfigurations != 1) {
		errno = EINVAL;
		quantis_ctx_log_error(ctx, "invalid number of configurations");
		free(device);
		return NULL;
	}

	status = libusb_open(dev, &device->device_handle);
	if (status) {
		usb_set_errno(status);
		quantis_ctx_log_error(ctx, "libusb_open");
		free(device);
		return NULL;
	}

	if (ctx->should_open_callback) {
		if (!ctx->should_open_callback(device)) {
			libusb_close(device->device_handle);
			free(device);
			return NULL;
		}
	}


        /* Set the active configuration for the device.*/

	/* Get the current one to avoid setting it to the same value as
	* it will cause an unecessary soft reset on the device.
	*/
	status = libusb_get_configuration(device->device_handle, &device_configuration);
	if (status) {
		usb_set_errno(status);
		perror("libusb_get_configuration");

		libusb_close(device->device_handle);
		free(device);
		return NULL;
	}

        /* Set the active configuration for the device.*/
	if (device_configuration != USB_DEVICE_CONFIGURATION) {
		status = libusb_set_configuration(device->device_handle, USB_DEVICE_CONFIGURATION);
		if (status) {
			usb_set_errno(status);
			quantis_ctx_log_error(ctx, "libusb_set_configuration");

			libusb_close(device->device_handle);
			free(device);
			return NULL;
		}
	}

	/* Claim interface */
	status = libusb_claim_interface(device->device_handle, 0);
	if (status) {
		usb_set_errno(status);
		libusb_close(device->device_handle);
		free(device);
		return NULL;
	}

	/* Make sure we are still in the right configuration */
	status = libusb_get_configuration(device->device_handle, &device_configuration);
	if (status) {
		usb_set_errno(status);
		quantis_ctx_log_error(ctx, "libusb_get_configuration");
		quantis_usb_destroy_device(device);
		return NULL;
	}

	status = libusb_get_config_descriptor(dev, 0, &usbConfig);
	if (status) {
		usb_set_errno(status);
		quantis_ctx_log_error(ctx, "libusb_get_config_descriptor");
		quantis_usb_destroy_device(device);
		return NULL;
	}

	/* Make sure there is only one interface */
	if (usbConfig->bNumInterfaces != 1) {
		errno = EINVAL;
		quantis_ctx_log_error(ctx, "invalid bNumInterfaces");
		libusb_free_config_descriptor(usbConfig);
		quantis_usb_destroy_device(device);
		return NULL;
	}

	if (usbConfig->interface[0].num_altsetting <= 0) {
		errno = EINVAL;
		quantis_ctx_log_error(ctx, "invalid num_altsetting");

		libusb_free_config_descriptor(usbConfig);
		quantis_usb_destroy_device(device);
		return NULL;
	}

	/* Check that we have at least one endpoint */
	if (usbConfig->interface[0].altsetting[0].bNumEndpoints < 1) {
		errno = EINVAL;
		quantis_ctx_log_error(ctx, "invalid bNumEndpoints");
		libusb_free_config_descriptor(usbConfig);
		quantis_usb_destroy_device(device);
		return NULL;
	}


	endpoint = &usbConfig->interface[0].altsetting[0].endpoint[0];


	/* Make sure the endpoint uses bulk transfers */
	if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_BULK) == 0) {
		errno = EINVAL;
		quantis_ctx_log_error(ctx, "invalid bmAttributes (BULK not set)");
		libusb_free_config_descriptor(usbConfig);
		quantis_usb_destroy_device(device);
		return NULL;
	}

	device->endpoint_address = endpoint->bEndpointAddress;

	/* Make sure the endpoint is device-to-host */
	if ((enum libusb_endpoint_direction)(device->endpoint_address & 0x80) != LIBUSB_ENDPOINT_IN) {
		errno = EINVAL;
		quantis_ctx_log_error(ctx, "invalid endpoint (Invalid direction)");
		libusb_free_config_descriptor(usbConfig);
		quantis_usb_destroy_device(device);
		return NULL;
	}


	device->max_packet_size = endpoint->wMaxPacketSize;

	libusb_free_config_descriptor(usbConfig);

	device->transfer = quantis_usb_create_transfer(device);
	if (!device->transfer) {
		quantis_ctx_log_error(ctx, "quantis_usb_create_transfer");
		quantis_usb_destroy_device(device);
		return NULL;
	}

	if (!ctx->devices) {
		ctx->devices = device;
	} else {
		device->next = ctx->devices;
		ctx->devices = device;
	}

	ctx->device_count++;

	if (ctx->device_callback) {
		ctx->device_callback(device, 1);
	}

	return device;
}

static int hotplug_callback(struct libusb_context *ctx, struct libusb_device *dev,
                     libusb_hotplug_event event, void *user_data)
{
	QuantisUSBContext *context;

	context = (QuantisUSBContext *)user_data;

	if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
		errno = 0;
		if (!quantis_usb_open_device(context, dev)) {
			if (errno) {
				quantis_ctx_log_error(context, "Could not open USB device");
			}
		}

	} else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
		QuantisUSBDevice *device;
		QuantisUSBDevice *prev;

		prev = NULL;

		for (device = context->devices; device; device = device->next) {
			if (libusb_get_device(device->device_handle) == dev) {
				quantis_usb_close_device(device, prev);
				break;
			}
			prev = device;
		}
	}

	return 0;
}
