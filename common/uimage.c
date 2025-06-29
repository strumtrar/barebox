// SPDX-License-Identifier: GPL-2.0-only
/*
 * uimage.c - uimage handling code
 *
 * Copyright (c) 2011 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * partly based on U-Boot uImage code
 */
#include <common.h>
#include <image.h>
#include <malloc.h>
#include <errno.h>
#include <crc.h>
#include <libbb.h>
#include <libfile.h>
#include <uncompress.h>
#include <fcntl.h>
#include <fs.h>
#include <rtc.h>
#include <filetype.h>
#include <memory.h>
#include <zero_page.h>

static inline int uimage_is_multi_image(struct uimage_handle *handle)
{
	return (handle->header.ih_type == IH_TYPE_MULTI) ? 1 : 0;
}

void uimage_print_contents(struct uimage_handle *handle)
{
	struct image_header *hdr = &handle->header;

	printf("   Image Name:   %.*s\n", IH_NMLEN, hdr->ih_name);

	if (IS_ENABLED(CONFIG_TIMESTAMP)) {
		struct rtc_time tm;

		printf("   Created:      ");
		to_tm(hdr->ih_time, &tm);
		printf("%4d-%02d-%02d  %2d:%02d:%02d UTC\n",
				tm.tm_year, tm.tm_mon, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec);
	}

#if defined(CONFIG_BOOTM_SHOW_TYPE)
	printf("   OS:           %s\n", image_get_os_name(hdr->ih_os));
	printf("   Architecture: %s\n", image_get_arch_name(hdr->ih_arch));
	printf("   Type:         %s\n", image_get_type_name(hdr->ih_type));
	printf("   Compression:  %s\n", image_get_comp_name(hdr->ih_comp));
#endif
	printf("   Data Size:    %d Bytes = %s\n", hdr->ih_size,
			size_human_readable(hdr->ih_size));
	printf("   Load Address: %08x\n", hdr->ih_load);
	printf("   Entry Point:  %08x\n", hdr->ih_ep);

	if (uimage_is_multi_image(handle)) {
		int i;

		printf("   Contents:\n");

		for (i = 0; i < handle->nb_data_entries; i++) {
			struct uimage_handle_data *data = &handle->ihd[i];

			printf("      Image %d: %ld (%s)\n", i,
					data->len, size_human_readable(data->len));
		}
	}
}
EXPORT_SYMBOL(uimage_print_contents);

ssize_t uimage_get_size(struct uimage_handle *handle, unsigned int image_no)
{
	if (image_no >= handle->nb_data_entries)
		return -EINVAL;

	return handle->ihd[image_no].len;
}
EXPORT_SYMBOL(uimage_get_size);

/*
 * open a uimage. This will check the header contents and
 * return a handle to the uImage
 */
struct uimage_handle *uimage_open(const char *filename)
{
	int fd;
	uint32_t checksum;
	struct uimage_handle *handle;
	struct image_header *header;
	int i;
	int ret;
	char *copy = NULL;

	if (is_tftp_fs(filename)) {
		ret = cache_file(filename, &copy);
		if (ret)
			return NULL;
		filename = copy;
	}

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		printf("could not open: %m\n");
		free(copy);
		return NULL;
	}

	handle = xzalloc(sizeof(struct uimage_handle));
	header = &handle->header;

	handle->copy = copy;

	if (read_full(fd, header, sizeof(*header)) != sizeof(*header)) {
		printf("could not read: %m\n");
		goto err_out;
	}

	if (uimage_to_cpu(header->ih_magic) != IH_MAGIC) {
		printf("Bad Magic Number\n");
		goto err_out;
	}

	checksum = uimage_to_cpu(header->ih_hcrc);
	header->ih_hcrc = 0;

	if (crc32(0, header, sizeof(*header)) != checksum) {
		printf("Bad Header Checksum\n");
		goto err_out;
	}

	/* convert header to cpu native endianness */
	header->ih_magic = uimage_to_cpu(header->ih_magic);
	header->ih_hcrc = uimage_to_cpu(header->ih_hcrc);
	header->ih_time = uimage_to_cpu(header->ih_time);
	header->ih_size = uimage_to_cpu(header->ih_size);
	header->ih_load = uimage_to_cpu(header->ih_load);
	header->ih_ep = uimage_to_cpu(header->ih_ep);
	header->ih_dcrc = uimage_to_cpu(header->ih_dcrc);

	if (header->ih_name[0]) {
		handle->name = xzalloc(IH_NMLEN + 1);
		strncpy(handle->name, header->ih_name, IH_NMLEN);
	} else {
		handle->name = xstrdup(filename);
	}

	if (uimage_is_multi_image(handle)) {
		size_t offset;

		for (i = 0; i < MAX_MULTI_IMAGE_COUNT; i++) {
			u32 size;

			ret = read_full(fd, &size, sizeof(size));
			if (ret != sizeof(size))
				goto err_out;

			if (!size)
				break;

			handle->ihd[i].len = uimage_to_cpu(size);
		}

		handle->nb_data_entries = i;

		/* offset of the first image in a multifile image */
		offset = 0;

		for (i = 0; i < handle->nb_data_entries; i++) {
			handle->ihd[i].offset = offset;
			offset += (handle->ihd[i].len + 3) & ~3;
		}

		handle->data_offset = sizeof(struct image_header) +
			sizeof(u32) * (handle->nb_data_entries + 1);
	} else {
		handle->ihd[0].offset = 0;
		handle->ihd[0].len = header->ih_size;
		handle->nb_data_entries = 1;
		handle->data_offset = sizeof(struct image_header);
	}

	/*
	 * fd is now at the first data word
	 */
	handle->fd = fd;

	return handle;
err_out:
	close(fd);

	free(handle->name);
	if (handle->copy) {
		unlink(handle->copy);
		free(handle->copy);
	}
	free(handle);

	return NULL;
}
EXPORT_SYMBOL(uimage_open);

/*
 * close a uImage previously opened with uimage_open
 */
void uimage_close(struct uimage_handle *handle)
{
	close(handle->fd);

	if (handle->copy) {
		unlink(handle->copy);
		free(handle->copy);
	}

	free(handle->name);
	free(handle);
}
EXPORT_SYMBOL(uimage_close);

static int uimage_fd;

static long uimage_fill(void *buf, unsigned long len)
{
	return read_full(uimage_fd, buf, len);
}

static int uncompress_copy(unsigned char *inbuf_unused, long len,
		long(*fill)(void*, unsigned long),
		long(*flush)(void*, unsigned long),
		unsigned char *outbuf_unused,
		long *pos,
		void(*error_fn)(char *x))
{
	int ret;
	void *buf = xmalloc(PAGE_SIZE);

	while (len) {
		int now = min_t(long, len, PAGE_SIZE);
		ret = fill(buf, now);
		if (ret < 0)
			goto err;
		ret = flush(buf, now);
		if (ret < 0)
			goto err;
		len -= now;
	}

	ret = 0;
err:
	free(buf);
	return ret;
}

/*
 * Verify the data crc of an uImage
 */
int uimage_verify(struct uimage_handle *handle)
{
	u32 crc = 0;
	int len, ret;
	loff_t off;
	void *buf;

	off = sizeof(struct image_header);
	if (lseek(handle->fd, off, SEEK_SET) != off)
		return -errno;

	buf = xmalloc(PAGE_SIZE);

	len = handle->header.ih_size;
	while (len) {
		int now = min(len, PAGE_SIZE);
		ret = read(handle->fd, buf, now);
		if (ret < 0)
			goto err;
		if (!ret)
			break;
		crc = crc32(crc, buf, ret);
		len -= ret;
	}

	if (crc != handle->header.ih_dcrc) {
		printf("Bad Data CRC: 0x%08x != 0x%08x\n",
				crc, handle->header.ih_dcrc);
		ret = -EINVAL;
		goto err;
	}

	ret = 0;
err:
	free(buf);

	return ret;
}
EXPORT_SYMBOL(uimage_verify);

/*
 * Load a uimage, flushing output to flush function
 */
int uimage_load(struct uimage_handle *handle, unsigned int image_no,
		long(*flush)(void*, unsigned long))
{
	image_header_t *hdr = &handle->header;
	struct uimage_handle_data *iha;
	int ret;
	loff_t off;
	int (*uncompress_fn)(unsigned char *inbuf, long len,
		    long(*fill)(void*, unsigned long),
	            long(*flush)(void*, unsigned long),
		    unsigned char *output,
	            long *pos,
		    void(*error)(char *x));

	if (image_no >= handle->nb_data_entries)
		return -EINVAL;

	iha = &handle->ihd[image_no];

	off = iha->offset + handle->data_offset;
	if (lseek(handle->fd, off, SEEK_SET) != off)
		return -errno;

	/* if ramdisk U-Boot expect to ignore the compression type */
	if (hdr->ih_comp == IH_COMP_NONE || hdr->ih_type == IH_TYPE_RAMDISK)
		uncompress_fn = uncompress_copy;
	else
		uncompress_fn = uncompress;

	uimage_fd = handle->fd;

	ret = uncompress_fn(NULL, iha->len, uimage_fill, flush,
				NULL, NULL,
				uncompress_err_stdout);
	return ret;
}
EXPORT_SYMBOL(uimage_load);

static void *uimage_buf;
static size_t uimage_size;
static struct resource *uimage_resource;

static long uimage_sdram_flush(void *buf, unsigned long len)
{
	if (uimage_size + len > resource_size(uimage_resource)) {
		resource_size_t start = uimage_resource->start;
		resource_size_t size = resource_size(uimage_resource) + len;

		release_sdram_region(uimage_resource);

		uimage_resource = request_sdram_region("uimage",
				start, size, MEMTYPE_LOADER_CODE,
				MEMATTRS_RWX);
		if (!uimage_resource) {
			resource_size_t prsize = start + size - 1;
			printf("unable to request SDRAM %pa - %pa\n",
				&start, &prsize);
			return -ENOMEM;
		}
	}

	if (zero_page_contains((unsigned long)uimage_buf + uimage_size))
		zero_page_memcpy(uimage_buf + uimage_size, buf, len);
	else
		memcpy(uimage_buf + uimage_size, buf, len);

	uimage_size += len;

	return len;
}

/*
 * Load an uImage to a dynamically allocated sdram resource.
 * the resource must be freed afterwards with release_sdram_region
 */
struct resource *uimage_load_to_sdram(struct uimage_handle *handle,
		int image_no, unsigned long load_address)
{
	int ret;
	ssize_t size;
	resource_size_t start = (resource_size_t)load_address;

	uimage_buf = (void *)load_address;
	uimage_size = 0;

	size = uimage_get_size(handle, image_no);
	if (size < 0)
		return NULL;

	uimage_resource = request_sdram_region("uimage",
				start, size, MEMTYPE_LOADER_CODE,
				MEMATTRS_RWX);
	if (!uimage_resource) {
		printf("unable to request SDRAM 0x%08llx-0x%08llx\n",
			(unsigned long long)start,
			(unsigned long long)start + size - 1);
		return NULL;
	}

	ret = uimage_load(handle, image_no, uimage_sdram_flush);
	if (ret) {
		release_sdram_region(uimage_resource);
		return NULL;
	}

	return uimage_resource;
}
EXPORT_SYMBOL(uimage_load_to_sdram);

void *uimage_load_to_buf(struct uimage_handle *handle, int image_no,
		size_t *outsize)
{
	u32 size;
	int ret;
	loff_t off;
	struct uimage_handle_data *ihd;
	char ftbuf[128];
	enum filetype ft;
	void *buf;

	if (image_no >= handle->nb_data_entries)
		return NULL;

	ihd = &handle->ihd[image_no];

	off = ihd->offset + handle->data_offset;
	if (lseek(handle->fd, off, SEEK_SET) != off)
		return NULL;

	if (handle->header.ih_comp == IH_COMP_NONE) {
		buf = malloc(ihd->len);
		if (!buf)
			return NULL;

		ret = read_full(handle->fd, buf, ihd->len);
		if (ret < ihd->len) {
			free(buf);
			return NULL;
		}
		size = ihd->len;
		goto out;
	}

	ret = read_full(handle->fd, ftbuf, 128);
	if (ret != 128)
		return NULL;

	ft = file_detect_type(ftbuf, 128);
	if ((int)ft < 0)
		return NULL;

	if (ft != filetype_gzip)
		return NULL;

	off = ihd->offset + handle->data_offset + ihd->len - 4;
	if (lseek(handle->fd, off, SEEK_SET) != off)
		return NULL;

	ret = read_full(handle->fd, &size, 4);
	if (ret != 4)
		return NULL;

	size = le32_to_cpu(size);

	off = ihd->offset + handle->data_offset;
	if (lseek(handle->fd, off, SEEK_SET) != off)
		return NULL;

	buf = malloc(size);
	ret = uncompress_fd_to_buf(handle->fd, buf, uncompress_err_stdout);
	if (ret) {
		free(buf);
		return NULL;
	}

out:
	if (outsize)
		*outsize = size;

	return buf;
}
