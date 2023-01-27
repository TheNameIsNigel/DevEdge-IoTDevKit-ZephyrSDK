/*
 * Copyright (c) 2022 T-Mobile USA, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Device Firmware Update (DFU) support for SiLabs Pearl Gecko
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/device.h>
#include <soc.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/fs/fs.h>
#include "mbedtls/sha1.h"
#include "dfu_gecko_lib.h"
#include <zephyr/sys/byteorder.h>

// SHAs are set to 0 since they are unknown before a build
const struct dfu_file_t dfu_files_mcu[] = {
	{
		"Gecko MCU 1/4",
		"/tmo/zephyr.slot0.bin",
		"tmo_shell.tmo_dev_edge.slot0.bin",
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
	},
	{
		"Gecko MCU 2/4",
		"/tmo/zephyr.slot1.bin",
		"tmo_shell.tmo_dev_edge.slot1.bin",
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
	},
	{
		"Gecko MCU 3/4",
		"/tmo/zephyr.slot0.bin.sha1",
		"tmo_shell.tmo_dev_edge.slot0.bin.sha1",
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
	},
	{
		"Gecko MCU 4/4",
		"/tmo/zephyr.slot1.bin.sha1",
		"tmo_shell.tmo_dev_edge.slot1.bin.sha1",
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
	},

	{"","","",""}
};

#define DFU_XFER_SIZE_2K    2048UL
#define DFU_CHUNK_SIZE      2048UL
#define DFU_IN_BETWEEN_FILE 0UL
#define DFU_START_OF_FILE   1UL
#define DFU_END_OF_FILE     2UL
#define DFU_FW_VER_SIZE     20UL

// slot partition addresses
#define GECKO_IMAGE_SLOT_0_SECTOR 0x10000
#define GECKO_IMAGE_SLOT_1_SECTOR 0x80000

typedef enum gecko_app_state_e {
	GECKO_INITIAL_STATE = 0,
	GECKO_INIT_STATE,
	GECKO_FW_UPGRADE,
	GECKO_FW_UPGRADE_DONE
} gecko_app_state_t;

// gecko FW update application control block
typedef struct gecko_app_cb_s {
	// gecko FW update application state
	gecko_app_state_t state;
} gecko_app_cb_t;

// application control block
gecko_app_cb_t gecko_app_cb;

// FW send variable , buffer
static uint32_t chunk_cnt = 0u, chunk_check = 0u, offset = 0u, fw_image_size = 0u;
static int32_t status = 0;
static uint8_t image_buffer[DFU_CHUNK_SIZE] = { 0 };
static uint8_t check_buf[DFU_CHUNK_SIZE + 1];
static int requested_slot_to_upgrade = -1;

#define GECKO_INCRE_PAGE 0
#define GECKO_INIT_PAGE 1
#define GECKO_FLASH_SECTOR 0x00000
extern int read_image_from_flash(uint8_t *flash_read_buffer, int readBytes, uint32_t flashStartSector, int ImageFileNum);

static struct fs_file_t geckofile = {0};
static int readbytes = 0;
static int totalreadbytes = 0;
static int totalwritebytes = 0;

static struct fs_file_t gecko_sha1_file = {0};
static mbedtls_sha1_context gecko_sha1_ctx;
static unsigned char gecko_sha1_output[DFU_SHA1_LEN];
static unsigned char gecko_expected_sha1[DFU_SHA1_LEN*2];
static unsigned char gecko_expected_sha1_final[DFU_SHA1_LEN];

extern const struct device *gecko_flash_dev;

static uint32_t crc32;
extern uint32_t crc32_ieee_update(uint32_t crc, const uint8_t * data, size_t len );

#define IMAGE_MAGIC                 0x96f3b83d
#define IMAGE_MAGIC_V1              0x96f3b83c
#define IMAGE_MAGIC_NONE            0xffffffff
#define IMAGE_TLV_INFO_MAGIC        0x6907
#define IMAGE_TLV_PROT_INFO_MAGIC   0x6908

#define IMAGE_HEADER_SIZE           32

struct image_version {
	uint8_t iv_major;
	uint8_t iv_minor;
	uint16_t iv_revision;
	uint32_t iv_build_num;
};

/** Image header.  All fields are in little endian byte order. */
struct image_header {
	uint32_t ih_magic;
	uint32_t ih_load_addr;
	uint16_t ih_hdr_size;           /* Size of image header (bytes). */
	uint16_t ih_protect_tlv_size;   /* Size of protected TLV area (bytes). */
	uint32_t ih_img_size;           /* Does not include header. */
	uint32_t ih_flags;              /* IMAGE_F_[...]. */
	struct image_version ih_ver;
	uint32_t _pad1;
};

static int compare_sha1(int slot_to_upgrade)
{
	printf("\n\tSHA1 compare for file zephyr.slot%d.bin\n", slot_to_upgrade);

	printf("\tExpected SHA1:\n\t\t");
	for (int i = 0; i < DFU_SHA1_LEN; i++) {
		printf("%02x ", gecko_expected_sha1_final[i]);
	}

	int sha1_fails = 0;
	for (int i = 0; i < DFU_SHA1_LEN; i++) {
		if (gecko_sha1_output[i] != gecko_expected_sha1_final[i]) {
			sha1_fails++;
			break;
		}
	}

	if (sha1_fails) {
		printf("\n\nSHA1 error: The computed file SHA1 doesn't match expected\n");
		return -1;
	}
	else
	{
		printf("\n\tSHA1 matches");
		return 0;
	}
}

static int slot_version_cmp(struct image_version *ver1,
		struct image_version *ver2)
{
	printf("slot image header version compare");

	if (ver1->iv_major > ver2->iv_major) {
		printf ("slot image header Major version compare 1. %u vs %u\n", ver1->iv_major, ver2->iv_major);
		return 0;
	}
	if (ver1->iv_major < ver2->iv_major) {
		printf ("slot image header Major version compare 2. %u vs %u\n", ver1->iv_major, ver2->iv_major);
		return 1;
	}
	/* The major version numbers are equal, continue comparison. */
	if (ver1->iv_minor > ver2->iv_minor) {
		printf ("slot image header Minor version compare 3. %u vs %u\n", ver1->iv_minor, ver2->iv_minor);
		return 0;
	}
	if (ver1->iv_minor < ver2->iv_minor) {
		printf("slot image header Minor version compare 4. %u vs %u\n", ver1->iv_minor, ver2->iv_minor);
		return 1;
	}
	/* The minor version numbers are equal, continue comparison. */
	if (ver1->iv_revision > ver2->iv_revision) {
		printf("slot image header revision version compare 5. %u vs %u\n", ver1->iv_revision, ver2->iv_revision );
		return 0;
	}
	if (ver1->iv_revision < ver2->iv_revision) {
		printf("slot image header revision version compare 6. %u vs %u\n", ver1->iv_revision, ver2->iv_revision );
		return 1;
	}

	printf("Error: slot image header version's are both equal\n");
	return -1;
}

int get_gecko_fw_version (void)
{
	int slot0_has_image = 0;
	int slot1_has_image = 0;
	int active_slot = -1;
	uint32_t page_addr_slot_0 = GECKO_IMAGE_SLOT_0_SECTOR;
	uint32_t page_addr_slot_1 = GECKO_IMAGE_SLOT_1_SECTOR;
	uint8_t read_buf[IMAGE_HEADER_SIZE];
	struct image_header slot0_hdr;
	struct image_header slot1_hdr;

	flash_read(gecko_flash_dev, page_addr_slot_0, read_buf, IMAGE_HEADER_SIZE);

	memcpy(&slot0_hdr, &read_buf, IMAGE_HEADER_SIZE);

	flash_read(gecko_flash_dev, page_addr_slot_1, read_buf, IMAGE_HEADER_SIZE);

	memcpy(&slot1_hdr, &read_buf, IMAGE_HEADER_SIZE);

	if (slot0_hdr.ih_magic == IMAGE_MAGIC) {
		printf("Pearl Gecko Slot 0 FW Version = %u.%u.%u+%u\n",
				slot0_hdr.ih_ver.iv_major,
				slot0_hdr.ih_ver.iv_minor,
				slot0_hdr.ih_ver.iv_revision,
				slot0_hdr.ih_ver.iv_build_num);
		slot0_has_image = 1;
		active_slot = 0;
	}
	else {
		printf("No bootable image/version found for Pearl Gecko slot 0\n");
	}

	if (slot1_hdr.ih_magic == IMAGE_MAGIC) {
		printf("Pearl Gecko Slot 1 FW Version = %u.%u.%u+%u\n",
				slot1_hdr.ih_ver.iv_major,
				slot1_hdr.ih_ver.iv_minor,
				slot1_hdr.ih_ver.iv_revision,
				slot1_hdr.ih_ver.iv_build_num);
		slot1_has_image = 1;
		active_slot = 1;
	}
	else {
		printf("No bootable image/version found for Pearl Gecko slot 1\n");
	}

	if (slot0_has_image && slot1_has_image) {
		printf("Pearl Gecko slot 0 and slot 1 contain a bootable active image\n");
		active_slot = slot_version_cmp(&slot0_hdr.ih_ver, &slot1_hdr.ih_ver);
		if (active_slot < 0) {
			return -1;
		}
		else {
			printf("Pearl Gecko slot %d is the current active image\n", active_slot);
		}
	}
	else if (slot0_has_image) {
		printf("Only Pearl Gecko slot 0 contains a bootable active image\n");
	}
	else if (slot1_has_image) {
		printf("Only Pearl Gecko slot 1 contains a bootable active image\n");
	}
	else {
		printf("Pearl Gecko contains no bootable images\n");
		return -1;
	}

	return 0;
}

// This function gets the size of the Gecko zephyr firmware
static uint32_t get_gecko_fw_size(void)
{
	int notdone = 1;
	totalreadbytes = 0;

	while (notdone)
	{
		readbytes = fs_read(&geckofile, image_buffer, DFU_XFER_SIZE_2K);
		if (readbytes < 0) {
			printf("Could not read file /tmo/zephyr.bin\n");
			return -1;
		}

		totalreadbytes += readbytes;
		/* Compute the SHA1 for this image while we get the size */
		mbedtls_sha1_update(&gecko_sha1_ctx, (unsigned char *)image_buffer, readbytes);

		if (readbytes == 0) {
			notdone = 0;
		}
	}

	mbedtls_sha1_finish(&gecko_sha1_ctx, gecko_sha1_output);
	printf("GECKO zephyr image size = %d\n", (uint32_t)totalreadbytes);

	printf("\tComputed File SHA1:\n\t\t");
	for (int i = 0; i < DFU_SHA1_LEN; i++) {
		printf("%02x ", gecko_sha1_output[i]);
	}

	return totalreadbytes;
}

/* Convert SHA1 ASCII hex to binary */
static void sha_hex_to_bin(char *sha_hex_in, char *sha_bin_out, int len)
{
	size_t i;
	char sha_ascii;
	unsigned char tempByte ;
	for (i = 0; i < len ; i++) {
		sha_ascii = *sha_hex_in;
		if (sha_ascii >= 97) {
			tempByte = sha_ascii - 97 + 10;
		} else if (sha_ascii >= 65) {
			tempByte = sha_ascii - 65 + 10;
		} else {
			tempByte = sha_ascii - 48;
		}
		/* In this ascii to binary encode, loop implementation
		 * the even SHA1 ascii characters are processed in the first pass,
		 * and the odd SHA1 characters are processed in the second pass
		 * of the current output byte
		 */
		if (i%2 == 0) {
			sha_bin_out[i/2] = tempByte << 4;
		} else {
			sha_bin_out[i/2] |= tempByte;
		}
		sha_hex_in++;
	}
}

// This function gets the sha1 of the Gecko zephyr firmware
static int get_gecko_sha1(void)
{
	readbytes = fs_read(&gecko_sha1_file, gecko_expected_sha1, DFU_SHA1_LEN*2);
	if ((readbytes < 0) || (readbytes != DFU_SHA1_LEN*2)) {
		printf("Could not read file /tmo/zephyr.bin.sha1\n");
		return -1;
	}

	sha_hex_to_bin(gecko_expected_sha1, gecko_expected_sha1_final, DFU_SHA1_LEN*2);

	return 0;
}

static int write_image_chunk_to_flash(int imageBytes, uint8_t* writedata, uint32_t startSector, int pageReset)
{
	int ret = 0;
	static uint32_t page = 0;

	if (pageReset) {
		page = 0;
		return 0;
	}

	uint32_t page_addr = startSector + (page * DFU_XFER_SIZE_2K);

	page++;

	// printf("\n1. readbytes %d page_addr %x\n", imageBytes, page_addr);
	if (flash_erase(gecko_flash_dev, page_addr, DFU_XFER_SIZE_2K) != 0) {
		printf("\nGecko 2K page erase failed\n");
	}

	/* This will also zero pad out the last 2K page write with the image remainder bytes. */
	if (flash_write(gecko_flash_dev, page_addr, writedata, DFU_XFER_SIZE_2K) != 0) {
		printf("Gecko flash write internal ERROR!");
		return -EIO;
	}

	flash_read(gecko_flash_dev, page_addr, check_buf, imageBytes);
	if (memcmp(writedata, check_buf, imageBytes) != 0) {
		printf("\nGecko flash erase-write-read ERROR!\n");
		return -EIO;
	}

	totalwritebytes += imageBytes;
	// printf("2. write flash addr %x total %d\n", page_addr, totalwritebytes);
	return ret;
}

static int file_read_flash(uint32_t offset)
{
	readbytes = fs_read(&geckofile, image_buffer, DFU_XFER_SIZE_2K);
	if (readbytes < 0) {
		printf("Could not read file /tmo/zephyr.slotx.bin\n");
		status = -1;
		return -1;
	}

	totalreadbytes += readbytes;
	//printf("\nreadbytes %d totalreadbytes %d\n", readbytes, totalreadbytes);

	if (readbytes > 0) {
		if (requested_slot_to_upgrade == 0) {
			write_image_chunk_to_flash(readbytes, image_buffer, GECKO_IMAGE_SLOT_0_SECTOR, GECKO_INCRE_PAGE);
		}
		else {
			write_image_chunk_to_flash(readbytes, image_buffer, GECKO_IMAGE_SLOT_1_SECTOR, GECKO_INCRE_PAGE);
		}

		crc32 = crc32_ieee_update(crc32, image_buffer, readbytes);
	}

	status = 0;
	return 0;
}

static uint8_t fw_upgrade_done = 0;
int32_t dfu_gecko_write_image(int slot_to_upgrade, char *bin_file, char *sha_file)
{
	char requested_binary_file[DFU_FILE_LEN];
	char requested_sha_file[DFU_FILE_LEN];
	requested_slot_to_upgrade = slot_to_upgrade;
	
	strcpy(requested_binary_file, bin_file);
	strcpy(requested_sha_file, sha_file);

	printf("Checking for presence of correct Gecko slot %d image file\n", slot_to_upgrade);
	if (slot_to_upgrade == 0) {
		if (fs_open(&geckofile, requested_binary_file, FS_O_READ) != 0) {
			printf("The Gecko FW file %s is missing\n", requested_binary_file);
			return 1;
		}
		else {
			printf("The required Gecko FW file %s is present\n", requested_binary_file);
		}

		if (fs_open(&gecko_sha1_file, requested_sha_file, FS_O_READ) != 0) {
			printf("The SHA1 digest file %s is missing\n",requested_sha_file);
			return 1;
		}
		else {
			printf("The required SHA1 Digest file %s is present\n", requested_sha_file);
		}
	}
	else {
		if (fs_open(&geckofile, requested_binary_file, FS_O_READ) != 0) {
			printf("The file %s is missing\n", requested_binary_file);
			return 1;
		}
		else {
			printf("The required Gecko FW file %s is present\n", requested_binary_file);
		}

		if (fs_open(&gecko_sha1_file, requested_sha_file, FS_O_READ) != 0) {
			printf("The Gecko FW file %s is missing\n", requested_sha_file);
			return 1;
		}
		else {
			printf("The required SHA1 Digest file %s is present\n", requested_sha_file);
		}
	}

	/* We do a dummy call here to init (reset) the incrementing page address var */
	write_image_chunk_to_flash(readbytes, image_buffer, GECKO_FLASH_SECTOR, GECKO_INIT_PAGE);

	readbytes = 0;
	totalreadbytes = 0;
	totalwritebytes = 0;

	while (!fw_upgrade_done) {
		switch (gecko_app_cb.state) {
			case GECKO_INITIAL_STATE:
				{
					printf("GECKO FW update started\n");
					/* update wlan application state */
					gecko_app_cb.state = GECKO_FW_UPGRADE;
					mbedtls_sha1_init(&gecko_sha1_ctx);
					memset(gecko_sha1_output, 0, sizeof(gecko_sha1_output));
					mbedtls_sha1_starts(&gecko_sha1_ctx);
				}
				/* no break */

			case GECKO_FW_UPGRADE:
				{
					/* Send the first chunk to extract header */
					fw_image_size = get_gecko_fw_size();
					if ((fw_image_size == 0) || (fw_image_size < DFU_CHUNK_SIZE)) {
						printf("\nERROR  - GECKO FW is too small\n");
						return -1;
					}

					int sha1_exist = get_gecko_sha1();
					if (sha1_exist != 0) {
						printf("\nERROR  - GECKO SHA1 is missing!\n");
						return -1;
					}

					int sha1_is_good = compare_sha1(slot_to_upgrade);
					if (sha1_is_good != 0) {
						printf("\nERROR  - GECKO SHA1 is miscompares !\n");
						return -1;
					}

					/* Calculate the total number of chunks */
					chunk_check = (fw_image_size / DFU_CHUNK_SIZE);
					if (fw_image_size % DFU_CHUNK_SIZE) {
						chunk_check += 1;
					}

					printf("zephyr.bin image_size = %d num of 2048 chunks = %d\n", fw_image_size, chunk_check);
					fs_seek(&geckofile, 0, FS_SEEK_SET);

					readbytes = 0;
					totalreadbytes = 0;
					totalwritebytes = 0;

					/* Loop until all the chunks are read and written */
					while (offset <= fw_image_size) {
						if (chunk_cnt != 0) {
							if (file_read_flash(offset) != 0) {
								printf("file system flash read failed\n");
								return (-1);
							}
							//printf("chunk_cnt: %d\n", chunk_cnt);
						}
						if (chunk_cnt == 0) {
							printf("\nGECKO FW update - starts here with - 1st Chunk\n");
							if (status != 0) {
								printf("1st Chunk GECKO_ERROR: %d\n", status);
								return (-1);
							}
						} else if (chunk_cnt == (chunk_check -1)) {
							printf("\nwriting last chunk\n");
							if (file_read_flash(offset) != 0) {
								printf("file system flash read failed\n");
								return (-1);
							}
							if (status != 0) {
								printf("last Chunk GECKO_ERROR: %d\n", status);
								break;
							}
							printf("\r\nGECKO FW update success\n");
							gecko_app_cb.state = GECKO_FW_UPGRADE_DONE;
							break;
						} else   {
							printk(".");
							//printf("\nGecko FW update - continues with in-between Chunks\n");
							if (status != 0) {
								printf("in-between Chunks GECKO_ERROR: %d\n", status);
								break;
							}
						}
						offset += readbytes;
						memset(image_buffer, 0, sizeof(image_buffer));
						chunk_cnt++;
					}       /* end While Loop */
				}               /* End case of  */
				break;

			case GECKO_FW_UPGRADE_DONE:
				{
					fw_upgrade_done = 1;
					fs_close(&geckofile);

					printf("\tCalculated program CRC32 is %x\n", crc32);
					printf("\ttotal bytes read       = %d bytes\n", totalreadbytes);
					printf("GECKO FW update has completed - rebooting now\n");
					k_sleep(K_SECONDS(3));
					sys_reboot(SYS_REBOOT_COLD);
				}
				break;

			default:
				printf("\nerror: dfu_gecko_write_image: default case\n");
				break;
		} /* end of switch */

	}
	return status;
} /* end of routine */

int dfu_mcu_firmware_upgrade(int slot_to_upgrade, char *bin_file, char *sha_file)
{
	int ret = 0;
	printf("*** Performing the Pearl Gecko FW update ***\n");
	ret = dfu_gecko_write_image(slot_to_upgrade, bin_file, sha_file);
	return ret;
}

/* Convert the desired type to system endianness and icnrement the buffer. This is just a wrapper to
 * avoid writing the following a ton of times: sys_le32_to_cpu(*((uint32_t*)var));
 * var=((uint8_t*)var)+sizeof(uint32_t);
 */
#define POP(var, sz)                                                                               \
	sys_le##sz##_to_cpu(*((uint##sz##_t *)var));                                               \
	var = ((uint8_t *)var) + sizeof(uint##sz##_t);

/**
 * @brief Deserialize the magic header once read from flash
 *
 * @param dst The dest structure to write into
 * @param read_buf The buffer read from flash
 */
static void deserialize_magic_hdr(struct image_header *dst, uint8_t *read_buf)
{
	if (!dst || !read_buf) {
		return;
	}

	dst->ih_magic = POP(read_buf, 32);
	dst->ih_load_addr = POP(read_buf, 32);
	dst->ih_hdr_size = POP(read_buf, 16);
	dst->ih_protect_tlv_size = POP(read_buf, 16);
	dst->ih_img_size = POP(read_buf, 32);
	dst->ih_flags = POP(read_buf, 32);

	dst->ih_ver.iv_major = read_buf[0];
	read_buf++;
	dst->ih_ver.iv_minor = read_buf[0];
	read_buf++;
	dst->ih_ver.iv_revision = POP(read_buf, 16);
	dst->ih_ver.iv_build_num = POP(read_buf, 32);
}

/**
 * @brief Get the oldest slot number, invalid slots are always considered the oldest, choses 0 if a
 * tie
 *
 * @return int 0 or 1 if a determination could be made, -1 otherwise
 */
int get_oldest_slot()
{
	struct image_header slot0_hdr;
	struct image_header slot1_hdr;
	uint8_t read_buf[DFU_IMAGE_HDR_LEN];
	bool slot0_has_image = false;
	bool slot1_has_image = false;
	int oldest_slot = 0;

	flash_read(gecko_flash_dev, DFU_SLOT0_FLASH_ADDR, read_buf,
		   DFU_IMAGE_HDR_LEN);
	deserialize_magic_hdr(&slot0_hdr, read_buf);

	flash_read(gecko_flash_dev, DFU_SLOT1_FLASH_ADDR, read_buf,
		   DFU_IMAGE_HDR_LEN);
	deserialize_magic_hdr(&slot1_hdr, read_buf);

	if (slot0_hdr.ih_magic == DFU_IMAGE_MAGIC) {
		LOG_DEBUG("%s Slot 0 FW Version = %u.%u.%u+%u" ENDL, CONFIG_MCU_NAME,
			  slot0_hdr.ih_ver.iv_major, slot0_hdr.ih_ver.iv_minor,
			  slot0_hdr.ih_ver.iv_revision, slot0_hdr.ih_ver.iv_build_num);
		slot0_has_image = true;
		oldest_slot = 1;
	} else {
		LOG_DEBUG("No bootable image/version found for %s slot 0\n",
			  CONFIG_MCU_NAME);
	}

	if (slot1_hdr.ih_magic == DFU_IMAGE_MAGIC) {
		LOG_DEBUG("%s Slot 1 FW Version = %u.%u.%u+%u" ENDL, CONFIG_MCU_NAME,
			  slot1_hdr.ih_ver.iv_major, slot1_hdr.ih_ver.iv_minor,
			  slot1_hdr.ih_ver.iv_revision, slot1_hdr.ih_ver.iv_build_num);
		slot1_has_image = true;
		oldest_slot = 0;
	} else {
		LOG_DEBUG("No bootable image/version found for %s slot 1" ENDL,
			  CONFIG_MCU_NAME);
	}

	if (slot0_has_image && slot1_has_image) {
		LOG_DEBUG("%s slot 0 and slot 1 contain a bootable active image" ENDL,
			  CONFIG_MCU_NAME);
		oldest_slot = slot_version_cmp(&slot0_hdr.ih_ver, &slot1_hdr.ih_ver);
		if (oldest_slot < 0) {
			return -1;
		}
		/* The given function finds the *newest* version, flip that */
		oldest_slot = (oldest_slot == 1) ? 0 : 1;
	} else if (!slot0_has_image && !slot1_has_image) {
		/* This should never happen and usually means no bootloader or an invalid image is
		 * running. */
		LOG_ERROR("No valid %s slots found, defaulting to slot 0 (S0 magic: %zu, S1 magic: "
			  "%zu)" ENDL,
			  CONFIG_MCU_NAME, slot0_hdr.ih_magic, slot1_hdr.ih_magic);
		/* TODO Return whichever slot is not being used. */
		oldest_slot = 0;
	}

	return oldest_slot;
}
