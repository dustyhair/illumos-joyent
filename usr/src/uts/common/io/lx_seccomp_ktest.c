/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include <sys/ktest.h>
#include <sys/kmem.h>
#include <sys/debug.h>
#include <sys/stddef.h>
#include "lx_seccomp_bpf.h"

CTASSERT(sizeof (lx_scbpf_insn_t) == 8);
CTASSERT(sizeof (lx_seccomp_data_t) == LX_SCBPF_DATASIZE);
CTASSERT(offsetof(lx_seccomp_data_t, instruction_pointer) == 8);
CTASSERT(offsetof(lx_seccomp_data_t, args) == 16);

#define	SC_PROGRAM_BYTES	((LX_SCBPF_MAXINSNS + 1) * \
	sizeof (lx_scbpf_insn_t))
#define	SC_WORK_BYTES	(LX_SCBPF_MAXINSNS * sizeof (uint32_t))

static uint32_t
sc_le32(const uchar_t *p)
{
	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void
sc_vectors(ktest_ctx_hdl_t *ctx)
{
	uchar_t *bytes;
	size_t size, pos = 16;
	uint32_t count, i;
	uint32_t *flow = NULL;
	lx_scbpf_insn_t *prog = NULL;

	ktest_get_input(ctx, &bytes, &size);
	if (size < 16 || sc_le32(bytes) != 0x4c585342 ||
	    sc_le32(bytes + 4) != 1 || sc_le32(bytes + 12) != 0) {
		KT_ERROR(ctx, "invalid seccomp BPF fixture header");
		return;
	}
	count = sc_le32(bytes + 8);
	if (count == 0 || count > 131072) {
		KT_ERROR(ctx, "invalid fixture count %u", count);
		return;
	}
	flow = kmem_alloc(SC_WORK_BYTES, KM_SLEEP);
	prog = kmem_alloc(SC_PROGRAM_BYTES, KM_SLEEP);
	for (i = 0; i < count; i++) {
		lx_seccomp_data_t data;
		uint32_t ninsns, valid, value;
		size_t insn_bytes;
		int actual;

		if (size - pos < 16 + sizeof (data)) {
			KT_ERROR(ctx, "truncated fixture record %u", i);
			goto out;
		}
		ninsns = sc_le32(bytes + pos);
		valid = sc_le32(bytes + pos + 4);
		value = sc_le32(bytes + pos + 8);
		if (ninsns > LX_SCBPF_MAXINSNS + 1 || valid > 1 ||
		    sc_le32(bytes + pos + 12) != 0) {
			KT_ERROR(ctx, "invalid fixture record %u", i);
			goto out;
		}
		bcopy(bytes + pos + 16, &data, sizeof (data));
		pos += 16 + sizeof (data);
		insn_bytes = ninsns * sizeof (*prog);
		if (size - pos < insn_bytes) {
			KT_ERROR(ctx, "truncated instructions in record %u", i);
			goto out;
		}
		bcopy(bytes + pos, prog, insn_bytes);
		pos += insn_bytes;
		actual = lx_seccomp_bpf_validate(prog, ninsns, flow);
		if (actual != valid) {
			KT_FAIL(ctx, "record %u validity %d, expected %u", i,
			    actual, valid);
			goto out;
		}
		if (actual &&
		    lx_seccomp_bpf_eval(prog, ninsns, &data) != value) {
			KT_FAIL(ctx, "record %u evaluation mismatch", i);
			goto out;
		}
	}
	if (pos != size) {
		KT_ERROR(ctx, "unexpected trailing fixture bytes");
		goto out;
	}
	KT_PASS(ctx);
out:
	kmem_free(prog, SC_PROGRAM_BYTES);
	kmem_free(flow, SC_WORK_BYTES);
}

static struct modlmisc sc_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "lx seccomp BPF tests"
};

static struct modlinkage sc_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &sc_modlmisc, NULL }
};

int
_init(void)
{
	ktest_module_hdl_t *km = NULL;
	ktest_suite_hdl_t *ks = NULL;
	int error;

	if ((error = ktest_create_module("lx_seccomp", &km)) != 0)
		return (error);
	if ((error = ktest_add_suite(km, "bpf", &ks)) != 0 ||
	    (error = ktest_add_test(ks, "vectors", sc_vectors,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_register_module(km)) != 0) {
		ktest_free_module(km);
		return (error);
	}
	if ((error = mod_install(&sc_modlinkage)) != 0)
		ktest_unregister_module("lx_seccomp");
	return (error);
}

int
_fini(void)
{
	int error = mod_remove(&sc_modlinkage);

	if (error == 0)
		ktest_unregister_module("lx_seccomp");
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&sc_modlinkage, modinfop));
}
