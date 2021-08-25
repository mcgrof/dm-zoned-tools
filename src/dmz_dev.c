// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * This file is part of dm-zoned tools.
 * Copyright (C) 2016, Western Digital.  All rights reserved.
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 *
 * Authors: Damien Le Moal (damien.lemoal@wdc.com)
 */
#include "dmz.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <mntent.h>
#include <dirent.h>

#include <blkid/blkid.h>

/*
 * Translate block number to device
 */
struct dmz_block_dev *dmz_block_to_bdev(struct dmz_dev *dev,
					__u64 block, __u64 *ret_block)
{
	int i;

	for (i = dev->nr_bdev - 1; i >= 0; i--) {
		if (block >= dev->bdev[i].block_offset) {
			*ret_block = block - dev->bdev[i].block_offset;
			return &dev->bdev[i];
		}
	}

	*ret_block = (__u64)-1;

	return NULL;
}

/*
 * Translate sector to device
 */
struct dmz_block_dev * dmz_sector_to_bdev(struct dmz_dev *dev,
					  __u64 sector, __u64 *ret_sector)
{
	int i;

	for (i = dev->nr_bdev - 1; i >= 0; i--) {
		__u64 sector_offset =
			dmz_blk2sect(dev->bdev[i].block_offset);
		if (sector >= sector_offset) {
			*ret_sector = sector - sector_offset;
			return &dev->bdev[i];
		}
	}

	*ret_sector = (__u64)-1;

	return NULL;
}

unsigned int dmz_block_zone_id(struct dmz_dev *dev, __u64 block)
{
	return block / dev->zone_nr_blocks;
}

/*
 * Test if the device is mounted.
 */
static int dmz_bdev_mounted(struct dmz_block_dev *bdev)
{
	struct mntent *mnt = NULL;
	FILE *file = NULL;

	file = setmntent("/proc/mounts", "r");
	if (file == NULL)
		return 0;

	while ((mnt = getmntent(file)) != NULL) {
		if (strcmp(bdev->path, mnt->mnt_fsname) == 0)
			break;
	}
	endmntent(file);

	return mnt ? 1 : 0;
}

/*
 * Test if the device is already used as a target backend.
 */
static int dmz_bdev_busy(struct dmz_block_dev *bdev, char *holder)
{
	char path[128];
	struct dirent **namelist;
	int n, ret = 0;

	snprintf(path, sizeof(path),
		 "/sys/class/block/%s/holders",
		 bdev->name);

	n = scandir(path, &namelist, NULL, alphasort);
	if (n < 0) {
		fprintf(stderr, "scandir %s failed\n", path);
		return -1;
	}

	while (n--) {
		if (strcmp(namelist[n]->d_name, "..") != 0 &&
		    strcmp(namelist[n]->d_name, ".") != 0) {
			if (holder)
				strncpy(holder, namelist[n]->d_name, PATH_MAX);
			ret = 1;
		}
		free(namelist[n]);
	}
	free(namelist);

	return ret;
}

/*
 * Check if a device is a partition.
 */
static int dmz_bdev_is_partition(struct dmz_block_dev *bdev)
{
	char str[PATH_MAX] = {};
	FILE *file;
	int len;

	len = snprintf(str, sizeof(str),
		       "/sys/class/block/%s/partition",
		       bdev->name);
	if (len >= PATH_MAX) {
		fprintf(stderr, "name %s failed: %s\n", str,
			strerror(ENAMETOOLONG));
		return -1;
	}

	file = fopen(str, "r");
	if (!file) {
		if (errno != ENOENT)
			return -1;
		return 0;
	}

	fclose(file);

	return 1;
}

/*
 * Get a zoned block device model (host-aware or howt-managed).
 */
static int dmz_get_bdev_model(struct dmz_block_dev *bdev)
{
	char str[PATH_MAX] = {};
	FILE *file;
	int res;
	int len;

	/* Cache devices could be partitions. Check that */
	res = dmz_bdev_is_partition(bdev);
	if (res < 0)
		return res;

	if (res) {
		/* This is a partition: only regular devices can have one */
		bdev->type = DMZ_TYPE_REGULAR;
		return 0;
	}

	/* Check that this is a zoned block device */
	len = snprintf(str, sizeof(str),
		       "/sys/block/%s/queue/zoned",
		       bdev->name);

	/* Indicates truncation */
	if (len >= PATH_MAX) {
		fprintf(stderr, "name %s failed: %s\n", str,
			strerror(ENAMETOOLONG));
		return -1;
	}

	file = fopen(str, "r");
	if (!file) {
		fprintf(stderr, "Open %s failed\n", str);
		return -1;
	}
	memset(str, 0, sizeof(str));
	res = fscanf(file, "%s", str);
	fclose(file);

	if (res != 1) {
		fprintf(stderr, "Invalid file %s format\n", str);
		return -1;
	}

	if (strcmp(str, "host-aware") == 0)
		bdev->type = DMZ_TYPE_ZONED_HA;
	else if (strcmp(str, "host-managed") == 0)
		bdev->type = DMZ_TYPE_ZONED_HM;
	else
		bdev->type = DMZ_TYPE_REGULAR;

	return 0;
}

/*
 * Get device capacity and zone size.
 */
static int dmz_get_bdev_capacity(struct dmz_block_dev *bdev)
{
	char str[128];
	FILE *file;
	int res;

	/* Get capacity */
	if (ioctl(bdev->fd, BLKGETSIZE64, &bdev->capacity) < 0) {
		fprintf(stderr,
			"%s: Get capacity failed %d (%s)\n",
			bdev->path, errno, strerror(errno));
		return -1;
	}
	bdev->capacity >>= 9;

	if (bdev->type == DMZ_TYPE_REGULAR)
		return 0;

	/* Get zone size */
	snprintf(str, sizeof(str),
		 "/sys/block/%s/queue/chunk_sectors",
		 bdev->name);
	file = fopen(str, "r");
	if (!file) {
		fprintf(stderr, "Open %s failed\n", str);
		return -1;
	}

	memset(str, 0, sizeof(str));
	res = fscanf(file, "%s", str);
	fclose(file);

	if (res != 1) {
		fprintf(stderr, "Invalid file %s format\n", str);
		return -1;
	}

	bdev->zone_nr_sectors = atol(str);
	if (!bdev->zone_nr_sectors ||
	    (bdev->zone_nr_sectors & DMZ_BLOCK_SECTORS_MASK)) {
		fprintf(stderr,
			"%s: Invalid zone size\n",
			bdev->path);
		return -1;
	}
	bdev->zone_nr_blocks = dmz_sect2blk(bdev->zone_nr_sectors);

	/* Get number of zones */
	bdev->nr_zones = bdev->capacity / bdev->zone_nr_sectors;
	if (bdev->capacity % bdev->zone_nr_sectors)
		bdev->nr_zones++;
	if (!bdev->nr_zones) {
		fprintf(stderr, "%s: invalid number of zones\n", bdev->path);
		return -1;
	}

	return 0;
}

/*
 * Print a device zone information.
 */
static void dmz_print_zone(struct dmz_dev *dev,
			   struct dmz_block_dev *bdev,
			   struct blk_zone *zone)
{

	if (dmz_zone_cond(zone) == BLK_ZONE_COND_READONLY) {
		printf("Zone %06u (%s): readonly %s zone\n",
		       dmz_zone_id(dev, zone), bdev->name,
		       dmz_zone_cond_str(zone));
		return;
	}

	if (dmz_zone_cond(zone) == BLK_ZONE_COND_OFFLINE) {
		printf("Zone %06u (%s): offline %s zone\n",
		       dmz_zone_id(dev, zone), bdev->name,
		       dmz_zone_cond_str(zone));
		return;
	}

	if (dmz_zone_conv(zone)) {
		printf("Zone %06u (%s): Conventional, cond 0x%x (%s), "
		       "sector %llu, %llu sectors\n",
		       dmz_zone_id(dev, zone), bdev->name,
		       dmz_zone_cond(zone),
		       dmz_zone_cond_str(zone),
		       dmz_zone_sector(zone),
		       dmz_zone_length(zone));
		return;
	}

	printf("Zone %06u (%s): type 0x%x (%s), cond 0x%x (%s), need_reset %d, "
	       "non_seq %d, sector %llu, %llu sectors, wp sector %llu\n",
	       dmz_zone_id(dev, zone), bdev->name,
	       dmz_zone_type(zone),
	       dmz_zone_type_str(zone),
	       dmz_zone_cond(zone),
	       dmz_zone_cond_str(zone),
	       dmz_zone_need_reset(zone),
	       dmz_zone_non_seq(zone),
	       dmz_zone_sector(zone),
	       dmz_zone_length(zone),
	       dmz_zone_wp_sector(zone));
}

#ifdef HAVE_BLK_ZONE_REP_V2
static __u64 dmz_zone_capacity(struct blk_zone *blkz)
{
	/*
	 * If dmzadm was compiled using a system supporting zone capacity but
	 * is executed on another system without zone capacity support, the
	 * capacity field will always be reported as 0. In this case, simply
	 * return the zone length.
	 */
	if (!blkz->capacity)
		return dmz_zone_length(blkz);
	return blkz->capacity;
}
#else
static __u64 dmz_zone_capacity(struct blk_zone *blkz)
{
	return dmz_zone_length(blkz);
}
#endif

#define DMZ_REPORT_ZONES_BUFSZ	524288

/*
 * Get a device zone configuration.
 */
int dmz_get_dev_zones(struct dmz_dev *dev)
{
	struct blk_zone_report *rep = NULL;
	unsigned int rep_max_zones;
	struct blk_zone *blkz;
	unsigned int i, nr_zones;
	__u64 sector;
	int ret = -1, d;

	dev->nr_zones = 0;
	for (d = 0; d < dev->nr_bdev; d++)
		dev->nr_zones += dev->bdev[d].nr_zones;

	/* Allocate zone array */
	dev->zones = calloc(dev->nr_zones, sizeof(struct blk_zone));
	if (!dev->zones) {
		fprintf(stderr, "Not enough memory\n");
		return -1;
	}

	/* Get a buffer for zone report */
	rep = malloc(DMZ_REPORT_ZONES_BUFSZ);
	if (!rep) {
		fprintf(stderr, "Not enough memory\n");
		goto out;
	}
	rep_max_zones =
		(DMZ_REPORT_ZONES_BUFSZ - sizeof(struct blk_zone_report))
		/ sizeof(struct blk_zone);

	sector = 0;
	nr_zones = 0;
	while (sector < dev->capacity) {
		__u64 sector_offset, bdev_sector;
		struct dmz_block_dev *bdev;

		bdev = dmz_sector_to_bdev(dev, sector, &bdev_sector);
		if (bdev->type == DMZ_TYPE_REGULAR) {
			__u64 zone_len = dev->zone_nr_sectors;

			/* Emulate zone information */
			blkz = &dev->zones[nr_zones];
			blkz->start = sector;
			if (blkz->start + zone_len > bdev->capacity)
				zone_len = bdev->capacity - blkz->start;
			blkz->len = zone_len;
			blkz->wp = (__u64)-1;
			blkz->type = BLK_ZONE_TYPE_UNKNOWN;
			blkz->cond = BLK_ZONE_COND_NOT_WP;
			if (dev->flags & DMZ_VVERBOSE)
				dmz_print_zone(dev, bdev, blkz);
			nr_zones++;
			sector += dev->zone_nr_sectors;
			continue;
		}

		/* Get zone information */
		sector_offset = dmz_blk2sect(bdev->block_offset);
		memset(rep, 0, DMZ_REPORT_ZONES_BUFSZ);
		rep->sector = bdev_sector;
		rep->nr_zones = rep_max_zones;
		if (dev->flags & DMZ_VVERBOSE)
			printf("%s: report zones sector %llu(%llu) zones %u start %u\n",
			       bdev->name, rep->sector, sector, rep->nr_zones,
			       nr_zones);
		ret = ioctl(bdev->fd, BLKREPORTZONE, rep);
		if (ret != 0) {
			fprintf(stderr,
				"%s: Get zone information failed %d (%s)\n",
				bdev->name, errno, strerror(errno));
			goto out;
		}

		if (!rep->nr_zones)
			break;

		blkz = (struct blk_zone *)(rep + 1);
		for (i = 0; i < rep->nr_zones; i++) {

			/* Check zone size */
			if (dmz_zone_length(blkz) != dev->zone_nr_sectors &&
			    dmz_zone_sector(blkz) + dmz_zone_length(blkz) != bdev->capacity) {
				fprintf(stderr,
					"%s: Invalid zone %u size\n",
					bdev->name,
					dmz_zone_id(dev, blkz));
				ret = -1;
				goto out;
			}

			/* Check zone capacity */
			if (dmz_zone_capacity(blkz) < dmz_zone_length(blkz)) {
				fprintf(stderr,
					"%s: Unsupported device with zone "
					"capacity smaller than zone size\n",
					bdev->name);
				ret = -1;
				goto out;
			}

			if (nr_zones >= dev->nr_zones) {
				fprintf(stderr,
					"%s: Invalid zone %u start %llu\n",
					bdev->name, nr_zones, blkz->start);
				ret = -1;
				goto out;
			}
			blkz->start += sector_offset;
			blkz->wp += sector_offset;
			if (dev->flags & DMZ_VVERBOSE)
				dmz_print_zone(dev, bdev, blkz);

			dev->zones[nr_zones] = *blkz;
			nr_zones++;

			sector = dmz_zone_sector(blkz) + dmz_zone_length(blkz);
			blkz++;
		}

	}

	if (dev->nr_zones != nr_zones) {
		fprintf(stderr,
			"%s: Invalid number of zones (expected %u, got %u)\n",
			dev->label,
			dev->nr_zones, nr_zones);
		ret = -1;
		goto out;
	}

	if (sector != nr_zones * dev->zone_nr_sectors) {
		fprintf(stderr,
			"%s: Invalid zones (last sector reported is %llu, "
			"expected %llu)\n",
			dev->label,
			sector, dev->capacity);
		ret = -1;
		goto out;
	}

out:
	free(rep);

	return ret;
}

/*
 * Get a device information.
 */
static int dmz_get_bdev_info(struct dmz_block_dev *bdev)
{
	if (dmz_get_bdev_model(bdev) < 0)
		return -1;

	if (dmz_get_bdev_capacity(bdev) < 0)
		return -1;

	return 0;
}

/*
 * Use libblkid to check for existing file systems on the disk.
 * Return -1 on error, 0 if something valid is detected on the disk
 * and 1 if the disk appears to be unused.
 */
static int dmz_check_overwrite(struct dmz_block_dev *bdev)
{
	const char *type;
	blkid_probe pr;
	int ret = -1;

	pr = blkid_new_probe_from_filename(bdev->path);
	if (!pr)
		goto out;

	ret = blkid_probe_enable_superblocks(pr, 1);
	if (ret < 0)
		goto out;

	ret = blkid_probe_enable_partitions(pr, 1);
	if (ret < 0)
		goto out;

	ret = blkid_do_fullprobe(pr);
	if (ret < 0 || ret == 1) {
		/* 1 means that nothing was found */
		goto out;
	}

	/* Analyze what was found on the disk */
	ret = blkid_probe_lookup_value(pr, "TYPE", &type, NULL);
	if (ret == 0) {
		fprintf(stderr,
			"%s appears to contain an existing filesystem (%s)\n",
			bdev->path, type);
		goto out;
	}

	ret = blkid_probe_lookup_value(pr, "PTTYPE", &type, NULL);
	if (ret == 0) {
		fprintf(stderr,
			"%s appears to contain a partition table (%s)\n",
			bdev->path, type);
		goto out;
	}

	fprintf(stderr,
		"%s appears to contain something according to blkid\n",
		bdev->path);
	ret = 0;

out:
	if (pr)
		blkid_free_probe(pr);

	if (ret == 0)
		fprintf(stderr, "Use the --force option to overwrite\n");
	else if (ret < 0)
		fprintf(stderr,
			"%s: probe failed, cannot detect existing filesystem\n",
			bdev->name);

	return ret;
}

/*
 * Open a device.
 */
int dmz_open_bdev(struct dmz_block_dev *bdev, enum dmz_op op, int flags)
{
	struct stat st;
	int ret;

	bdev->name = basename(bdev->path);

	/* Check that this is a block device */
	if (stat(bdev->path, &st) < 0) {
		fprintf(stderr,
			"Get %s stat failed %d (%s)\n",
			bdev->path,
			errno, strerror(errno));
		return -1;
	}

	if (!S_ISBLK(st.st_mode)) {
		fprintf(stderr,
			"%s is not a block device\n",
			bdev->path);
		return -1;
	}

	if (op == DMZ_OP_FORMAT && (!(flags & DMZ_OVERWRITE))) {
		/* Check for existing valid content */
		ret = dmz_check_overwrite(bdev);
		if (ret <= 0)
			return -1;
	}

	if (dmz_bdev_mounted(bdev)) {
		fprintf(stderr,
			"%s is mounted\n",
			bdev->path);
		return -1;
	}

	if (dmz_bdev_busy(bdev, NULL)) {
		fprintf(stderr,
			"%s is in use\n",
			bdev->path);
		return -1;
	}

	/* Open device */
	bdev->fd = open(bdev->path, O_RDWR | O_LARGEFILE);
	if (bdev->fd < 0) {
		fprintf(stderr,
			"Open %s failed %d (%s)\n",
			bdev->path,
			errno, strerror(errno));
		return -1;
	}

	/* Get device capacity and zone configuration */
	if (dmz_get_bdev_info(bdev) < 0) {
		dmz_close_bdev(bdev);
		return -1;
	}

	return 0;
}

/*
 * Get the holder of a device
 */
int dmz_get_bdev_holder(struct dmz_block_dev *bdev, char *holder)
{
	struct stat st;

	bdev->name = basename(bdev->path);

	/* Check that this is a block device */
	if (stat(bdev->path, &st) < 0) {
		fprintf(stderr,
			"Get %s stat failed %d (%s)\n",
			bdev->path,
			errno, strerror(errno));
		return -1;
	}

	if (!S_ISBLK(st.st_mode)) {
		fprintf(stderr,
			"%s is not a block device\n",
			bdev->path);
		return -1;
	}

	if (dmz_bdev_mounted(bdev)) {
		fprintf(stderr,
			"%s is mounted\n",
			bdev->path);
		return -1;
	}

	if (!dmz_bdev_busy(bdev, holder))
		memset(holder, 0, PATH_MAX);

	return 0;
}

/*
 * Close an open device.
 */
void dmz_close_bdev(struct dmz_block_dev *bdev)
{
	if (bdev->fd >= 0) {
		close(bdev->fd);
		bdev->fd = -1;
	}
}

/*
 * Read a metadata block.
 */
int dmz_read_block(struct dmz_dev *dev, __u64 block, __u8 *buf)
{
	__u64 read_block;
	struct dmz_block_dev *bdev =
		dmz_block_to_bdev(dev, block, &read_block);
	ssize_t ret;

	ret = pread(bdev->fd, (char *)buf, DMZ_BLOCK_SIZE,
		    read_block << DMZ_BLOCK_SHIFT);

	if (ret != DMZ_BLOCK_SIZE) {
		fprintf(stderr,
			"%s: Read block %llu failed %d (%s)\n",
			bdev->name,
			read_block,
			errno, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Write a metadata block.
 */
int dmz_write_block(struct dmz_dev *dev, __u64 block, __u8 *buf)
{
	__u64 write_block;
	struct dmz_block_dev *bdev =
		dmz_block_to_bdev(dev, block, &write_block);
	ssize_t ret;

	ret = pwrite(bdev->fd, (char *)buf, DMZ_BLOCK_SIZE,
		     write_block << DMZ_BLOCK_SHIFT);
	if (ret != DMZ_BLOCK_SIZE) {
		fprintf(stderr,
			"%s: Write block %llu failed %d (%s)\n",
			bdev->name,
			block,
			errno, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Flush the write cache of all block devices of a DM device.
 */
int dmz_sync_dev(struct dmz_dev *dev)
{
	struct dmz_block_dev *bdev;
	int i;

	/* Sync all disks */
	printf("Syncing disk%s\n", dev->nr_bdev > 1 ? "s" : "");

	for (i = 0; i < dev->nr_bdev; i++) {
		bdev = &dev->bdev[i];
		if (fsync(bdev->fd) < 0) {
			fprintf(stderr,
				"%s: fsync failed %d (%s)\n",
				bdev->name,
				errno, strerror(errno));
			return -1;
		}
	}

	return 0;
}
