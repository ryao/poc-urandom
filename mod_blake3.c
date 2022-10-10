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
#define	RNG_BUFSIZE	1024 * 63

#define	MIN(x, y)	((x) < (y) ? (x):(y))

void __percpu *spl_pseudo_entropy;

typedef struct {
	BLAKE3_CTX ctx;
	uint64_t seek;

	uint8_t *buf;
	uint32_t pos;
} rng_t;

static void rng_nextkey(rng_t *rng)
{
		uint8_t key[BLAKE3_KEY_LEN];
		get_random_bytes(key, sizeof (key));
		Blake3_InitKeyed(&rng->ctx, key);
		rng->seek = 0;
}

static void rng_nextdata(rng_t *rng)
{
	/* check if current key usage should be limited */
	if (rng->seek + RNG_BUFSIZE > MAX_PER_KEY)
		rng_nextkey(rng);

	/* get next bytes for current key */
	Blake3_FinalSeek(&rng->ctx, rng->seek, rng->buf, RNG_BUFSIZE);
	rng->seek += RNG_BUFSIZE;
	rng->pos = 0;
}

static void copy_to_user_loop(void __user *to, const void *from, unsigned long len)
{
	unsigned long done = 0, left = len;

	while (left != 0) {
		left = copy_to_user(to + done, from + done, len - done);
		done = len - left;
	}
}

static ssize_t
poc_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	rng_t *rng;
	size_t done = 0;

	if (!access_ok(buffer, len))
		return (-EFAULT);

	rng = get_cpu_ptr(spl_pseudo_entropy);
	while (done != len) {
		size_t todo = MIN(len - done, RNG_BUFSIZE - rng->pos);
		copy_to_user_loop(buffer + done, rng->buf + rng->pos, todo);
		rng->pos += todo;
		done += todo;
		if (rng->pos == RNG_BUFSIZE)
			rng_nextdata(rng);
	}
	put_cpu_ptr(spl_pseudo_entropy);

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
	uint8_t key[2];
	int i = 0;

	get_random_bytes(key, sizeof (key));
	if (key[0] == 0 && key[1] == 0) {
		printk("SPL: get_random_bytes() returned 0 "
		    "when generating random seed.");
		return (-EAGAIN);
	}

	spl_pseudo_entropy = __alloc_percpu(sizeof (rng_t), sizeof (uint64_t));
	if (!spl_pseudo_entropy)
		return (-ENOMEM);

	for_each_possible_cpu(i) {
		rng_t *rng = per_cpu_ptr(spl_pseudo_entropy, i);
		rng_nextkey(rng);
		rng->buf = vmalloc(RNG_BUFSIZE);
		if (rng->buf == NULL)
			return (-ENOMEM);
		rng_nextdata(rng);
	}

	// SSE41 and AVX2 need fpu_begin()/fpu_end() :/
	if (zfs_sse4_1_available())
		blake3_impl_setname("sse41");

	// SSE41 seems faster for smaller units
	if (zfs_avx2_available())
		blake3_impl_setname("avx2");

	return (0);
}

static void
spl_random_fini(void)
{
	int i = 0;

	for_each_possible_cpu(i) {
		rng_t *rng = per_cpu_ptr(spl_pseudo_entropy, i);
		vfree(rng->buf);
	}
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
