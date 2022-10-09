/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2021-2022 Tino Reichardt <milky-zfs@mcmilk.de>
 */

#include "blake3.h"
#include "blake3_impl.h"

static const blake3_ops_t *const blake3_impls[] = {
	&blake3_generic_impl,
#if defined(__aarch64__) || \
	(defined(__x86_64) && defined(HAVE_SSE2)) || \
	(defined(__PPC64__) && defined(__LITTLE_ENDIAN__))
	&blake3_sse2_impl,
#endif
#if defined(__aarch64__) || \
	(defined(__x86_64) && defined(HAVE_SSE4_1)) || \
	(defined(__PPC64__) && defined(__LITTLE_ENDIAN__))
	&blake3_sse41_impl,
#endif
#if defined(__x86_64) && defined(HAVE_SSE4_1) && defined(HAVE_AVX2)
	&blake3_avx2_impl,
#endif
#if defined(__x86_64) && defined(HAVE_AVX512F) && defined(HAVE_AVX512VL)
	&blake3_avx512_impl,
#endif
};

/* Select BLAKE3 implementation */
#define	IMPL_FASTEST	(UINT32_MAX)
#define	IMPL_CYCLE	(UINT32_MAX - 1)

#define	IMPL_READ(i)	(*(volatile uint32_t *) &(i))

/* Indicate that benchmark has been done */
static boolean_t blake3_initialized = B_FALSE;

/* Implementation that contains the fastest methods */
static blake3_ops_t blake3_fastest_impl = {
	.name = "fastest"
};

/* Hold all supported implementations */
static const blake3_ops_t *blake3_supp_impls[ARRAY_SIZE(blake3_impls)];
static uint32_t blake3_supp_impls_cnt = 0;

/* Currently selected implementation */
static uint32_t blake3_impl_chosen = IMPL_FASTEST;

static struct blake3_impl_selector {
	const char *name;
	uint32_t sel;
} blake3_impl_selectors[] = {
	{ "cycle",	IMPL_CYCLE },
	{ "fastest",	IMPL_FASTEST }
};

/* check the supported implementations */
static void blake3_impl_init(void)
{
	int i, c;

	/* init only once */
	if (likely(blake3_initialized))
		return;

	/* move supported implementations into blake3_supp_impls */
	for (i = 0, c = 0; i < ARRAY_SIZE(blake3_impls); i++) {
		const blake3_ops_t *impl = blake3_impls[i];

		if (impl->is_supported && impl->is_supported())
			blake3_supp_impls[c++] = impl;
	}
	blake3_supp_impls_cnt = c;

	/* first init generic impl, may be changed via set_fastest() */
	memcpy(&blake3_fastest_impl, blake3_impls[0],
	    sizeof (blake3_fastest_impl));
	blake3_initialized = B_TRUE;
}

/* get number of supported implementations */
uint32_t
blake3_impl_getcnt(void)
{
	blake3_impl_init();
	return (blake3_supp_impls_cnt);
}

/* get id of selected implementation */
uint32_t
blake3_impl_getid(void)
{
	return (IMPL_READ(blake3_impl_chosen));
}

/* get name of selected implementation */
const char *
blake3_impl_getname(void)
{
	uint32_t impl = IMPL_READ(blake3_impl_chosen);

	blake3_impl_init();
	switch (impl) {
	case IMPL_FASTEST:
		return ("fastest");
	case IMPL_CYCLE:
		return ("cycle");
	default:
		return (blake3_supp_impls[impl]->name);
	}
}

/* setup id as fastest implementation */
void
blake3_impl_set_fastest(uint32_t id)
{
	/* setup fastest impl */
	memcpy(&blake3_fastest_impl, blake3_supp_impls[id],
	    sizeof (blake3_fastest_impl));
}

/* set implementation by id */
void
blake3_impl_setid(uint32_t id)
{
	blake3_impl_init();
	switch (id) {
	case IMPL_FASTEST:
		atomic_swap_32(&blake3_impl_chosen, IMPL_FASTEST);
		break;
	case IMPL_CYCLE:
		atomic_swap_32(&blake3_impl_chosen, IMPL_CYCLE);
		break;
	default:
		atomic_swap_32(&blake3_impl_chosen, id);
		break;
	}
}

/* set implementation by name */
int
blake3_impl_setname(const char *val)
{
	uint32_t impl = IMPL_READ(blake3_impl_chosen);
	size_t val_len;
	int i, err = -EINVAL;

	blake3_impl_init();
	val_len = strlen(val);

	/* check mandatory implementations */
	for (i = 0; i < ARRAY_SIZE(blake3_impl_selectors); i++) {
		const char *name = blake3_impl_selectors[i].name;

		if (val_len == strlen(name) &&
		    strncmp(val, name, val_len) == 0) {
			impl = blake3_impl_selectors[i].sel;
			err = 0;
			break;
		}
	}

	if (err != 0 && blake3_initialized) {
		/* check all supported implementations */
		for (i = 0; i < blake3_supp_impls_cnt; i++) {
			const char *name = blake3_supp_impls[i]->name;

			if (val_len == strlen(name) &&
			    strncmp(val, name, val_len) == 0) {
				impl = i;
				err = 0;
				break;
			}
		}
	}

	if (err == 0) {
		atomic_swap_32(&blake3_impl_chosen, impl);
	}

	return (err);
}

const blake3_ops_t *
blake3_impl_get_ops(void)
{
	static uint32_t cycle_count = 0;
	const blake3_ops_t *ops = NULL;
	uint32_t impl = IMPL_READ(blake3_impl_chosen);
	uint32_t idx;

	blake3_impl_init();
	switch (impl) {
	case IMPL_FASTEST:
		ops = &blake3_fastest_impl;
		break;
	case IMPL_CYCLE:
		/* Cycle through supported implementations */
		idx = (++cycle_count) % blake3_supp_impls_cnt;
		ops = blake3_supp_impls[idx];
		break;
	default:
		ops = blake3_supp_impls[impl];
		break;
	}

	return (ops);
}
