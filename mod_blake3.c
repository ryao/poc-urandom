/*
 * Copyright (C) 2022 Richard Yao
 * Copyright (C) 2022 Tino Reichardt
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/random.h>
#include <linux/vmalloc.h>

#include "blake3.h"
#include "blake3_impl.h"

#define	POC_DRIVER	"urandom-blake3"
#define	MAX_PER_KEY	0xdeadbeaf

void __percpu *spl_pseudo_entropy;

typedef struct {
	BLAKE3_CTX ctx;
	uint64_t seek;
} rng_t;

static ssize_t
poc_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	rng_t *rng;
	char *buf;
	unsigned long copied = 0, left = len;

	if (!access_ok(buffer, len))
		return (-EFAULT);

	buf = vmalloc(len);
	if (buf == NULL)
		return (-ENOMEM);

	rng = get_cpu_ptr(spl_pseudo_entropy);

	/* check if current key usage is too much */
	if (rng->seek + len > MAX_PER_KEY) {
		uint8_t key[BLAKE3_KEY_LEN];
		get_random_bytes(key, sizeof (key));
		Blake3_InitKeyed(&rng->ctx, key);
		rng->seek = 0;
	}

	/* get next bytes for current key */
	Blake3_FinalSeek(&rng->ctx, rng->seek, buf, len);

	/*
	 * XXX this is not the KDF function which should perform
	 * better ... it's MAC currently
	 */

	/* increment seek counter */
	rng->seek += len;

	put_cpu_ptr(spl_pseudo_entropy);

	while (left != 0) {
		left = copy_to_user(buffer + copied, buf + copied, len);
		copied = len - left;
	}
	vfree(buf);

	return (len);
}

static struct file_operations poc_fops =
{
	.read		= poc_read,
	.owner		= THIS_MODULE,
};

static struct miscdevice poc_misc = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= POC_DRIVER,
	.fops		= &poc_fops,
	.mode		= S_IRWXU | S_IRWXG | S_IRWXO
};

static int __init
spl_random_init(void)
{
	uint8_t key[BLAKE3_KEY_LEN];
	int i = 0;

	spl_pseudo_entropy = __alloc_percpu(sizeof (rng_t), sizeof (uint64_t));
	if (!spl_pseudo_entropy)
		return (-ENOMEM);

	get_random_bytes(key, sizeof (key));
	if (key[0] == 0 && key[1] == 0) {
		printk("SPL: get_random_bytes() returned 0 "
		    "when generating random seed.");
		return (-EAGAIN);
	}

	for_each_possible_cpu(i) {
		rng_t *rng = per_cpu_ptr(spl_pseudo_entropy, i);
		get_random_bytes(key, sizeof (key));
		Blake3_InitKeyed(&rng->ctx, key);
		rng->seek = 0;
	}

	// SSE41 and AVX2 need fpu_begin()/fpu_end() :/
	if (zfs_sse4_1_available())
		blake3_impl_setname("sse41");

	// SSE41 seems faster for smaller units
	//if (zfs_avx2_available())
	//	blake3_impl_setname("avx2");

	return (0);
}

static void
spl_random_fini(void)
{
	free_percpu(spl_pseudo_entropy);
}

int
init_module(void)
{
	int error;

	if ((error = spl_random_init()))
		return (error);

	error = misc_register(&poc_misc);
	if (error) {
		spl_random_fini();
		return (error);
	}

	return (0);
}

void
cleanup_module(void)
{
	misc_deregister(&poc_misc);
	spl_random_fini();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Yao, Tino Reichardt");
MODULE_DESCRIPTION("RFC BLAKE3 CSPRNG for OpenZFS");
MODULE_VERSION("1.0");
