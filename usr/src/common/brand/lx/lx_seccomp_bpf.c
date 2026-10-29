/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */

/*
 * A seccomp-specific classic BPF verifier and interpreter.  Packet byte
 * loads, indirect loads and socket-filter ancillary data are not accepted.
 * Metadata loads are aligned 32-bit words in the x86 Linux ABI's byte order.
 * This core does not authorize filter installation or intercept syscalls.
 */
#include "lx_seccomp_bpf.h"

typedef enum sc_kind {
	SC_BAD, SC_LDA, SC_LDX, SC_ABS, SC_LDM, SC_LDXM, SC_LEN, SC_XLEN,
	SC_ST, SC_STX, SC_ALUK, SC_ALUX, SC_NEG, SC_JA, SC_JK, SC_JX,
	SC_RK, SC_RA, SC_TAX, SC_TXA
} sc_kind_t;

#define	SC_MEMMASK	0xffffU

/* Exact opcode matching also rejects undefined high instruction bits. */
static sc_kind_t
sc_kind(uint16_t code)
{
	switch (code) {
	case 0x00: return (SC_LDA);
	case 0x01: return (SC_LDX);
	case 0x20: return (SC_ABS);
	case 0x60: return (SC_LDM);
	case 0x61: return (SC_LDXM);
	case 0x80: return (SC_LEN);
	case 0x81: return (SC_XLEN);
	case 0x02: return (SC_ST);
	case 0x03: return (SC_STX);
	case 0x04: case 0x14: case 0x24: case 0x34: case 0x44:
	case 0x54: case 0x64: case 0x74: case 0xa4:
		return (SC_ALUK);
	case 0x0c: case 0x1c: case 0x2c: case 0x3c: case 0x4c:
	case 0x5c: case 0x6c: case 0x7c: case 0xac:
		return (SC_ALUX);
	case 0x84: return (SC_NEG);
	case 0x05: return (SC_JA);
	case 0x15: case 0x25: case 0x35: case 0x45:
		return (SC_JK);
	case 0x1d: case 0x2d: case 0x3d: case 0x4d:
		return (SC_JX);
	case 0x06: return (SC_RK);
	case 0x16: return (SC_RA);
	case 0x07: return (SC_TAX);
	case 0x87: return (SC_TXA);
	default: return (SC_BAD);
	}
}

int
lx_seccomp_bpf_validate(const lx_scbpf_insn_t *prog, uint32_t ninsns,
    uint32_t *flow)
{
	uint32_t i, mask = 0;
	sc_kind_t last;

	if (prog == 0 || flow == 0 || ninsns == 0 ||
	    ninsns > LX_SCBPF_MAXINSNS)
		return (0);
	last = sc_kind(prog[ninsns - 1].code);
	if (last != SC_RK && last != SC_RA)
		return (0);
	for (i = 0; i < ninsns; i++)
		flow[i] = SC_MEMMASK;

	for (i = 0; i < ninsns; i++) {
		const lx_scbpf_insn_t *p = &prog[i];
		sc_kind_t kind = sc_kind(p->code);
		uint32_t remain = ninsns - i - 1;
		uint32_t op = p->code >> 4;

		mask &= flow[i];
		switch (kind) {
		case SC_BAD:
			return (0);
		case SC_ABS:
			if ((p->k & 3) != 0 || p->k > LX_SCBPF_DATASIZE - 4)
				return (0);
			break;
		case SC_LDM:
		case SC_LDXM:
		case SC_ST:
		case SC_STX:
			if (p->k >= LX_SCBPF_MEMWORDS)
				return (0);
			if (kind == SC_ST || kind == SC_STX) {
				mask |= 1U << p->k;
			} else if ((mask & (1U << p->k)) == 0) {
				return (0);
			}
			break;
		case SC_ALUK:
			if (op == 3 && p->k == 0)
				return (0);
			if ((op == 6 || op == 7) && p->k >= 32)
				return (0);
			break;
		case SC_JA:
			if (p->k >= remain)
				return (0);
			break;
		case SC_JK:
		case SC_JX:
			if (p->jt >= remain || p->jf >= remain)
				return (0);
			break;
		default:
			break;
		}

		/*
		 * Match classic Linux scratch validation even for instructions
		 * skipped by RET.  Only jumps end linear mask propagation.
		 * Each destination intersects all incoming store masks.
		 */
		if (kind == SC_JA) {
			flow[i + 1 + p->k] &= mask;
			mask = SC_MEMMASK;
		} else if (kind == SC_JK || kind == SC_JX) {
			flow[i + 1 + p->jt] &= mask;
			flow[i + 1 + p->jf] &= mask;
			mask = SC_MEMMASK;
		}
	}
	return (1);
}

static uint32_t
sc_word(const lx_seccomp_data_t *data, uint32_t offset)
{
	if (offset == 0)
		return ((uint32_t)data->nr);
	if (offset == 4)
		return (data->arch);
	if (offset < 16)
		return ((uint32_t)(data->instruction_pointer >>
		    ((offset & 4) != 0 ? 32 : 0)));
	return ((uint32_t)(data->args[(offset - 16) / 8] >>
	    ((offset & 4) != 0 ? 32 : 0)));
}

uint32_t
lx_seccomp_bpf_eval(const lx_scbpf_insn_t *prog, uint32_t ninsns,
    const lx_seccomp_data_t *data)
{
	uint32_t a = 0, x = 0, mem[LX_SCBPF_MEMWORDS] = { 0 };
	uint32_t pc = 0;

	if (prog == 0 || data == 0 || ninsns == 0 ||
	    ninsns > LX_SCBPF_MAXINSNS)
		return (0);

	while (pc < ninsns) {
		const lx_scbpf_insn_t *p = &prog[pc++];
		sc_kind_t kind = sc_kind(p->code);
		uint32_t v, jump;
		int take;

		switch (kind) {
		case SC_LDA: a = p->k; break;
		case SC_LDX: x = p->k; break;
		case SC_ABS:
			if ((p->k & 3) != 0 || p->k > LX_SCBPF_DATASIZE - 4)
				return (0);
			a = sc_word(data, p->k);
			break;
		case SC_LEN: a = LX_SCBPF_DATASIZE; break;
		case SC_XLEN: x = LX_SCBPF_DATASIZE; break;
		case SC_LDM:
		case SC_LDXM:
		case SC_ST:
		case SC_STX:
			if (p->k >= LX_SCBPF_MEMWORDS)
				return (0);
			if (kind == SC_LDM)
				a = mem[p->k];
			else if (kind == SC_LDXM)
				x = mem[p->k];
			else if (kind == SC_ST)
				mem[p->k] = a;
			else
				mem[p->k] = x;
			break;
		case SC_ALUK:
		case SC_ALUX:
			v = kind == SC_ALUK ? p->k : x;
			switch (p->code >> 4) {
			case 0: a += v; break;
			case 1: a -= v; break;
			case 2: a *= v; break;
			case 3:
				if (v == 0)
					return (0);
				a /= v;
				break;
			case 4: a |= v; break;
			case 5: a &= v; break;
			case 6: a <<= (v & 31); break;
			case 7: a >>= (v & 31); break;
			case 10: a ^= v; break;
			default: return (0);
			}
			break;
		case SC_NEG: a = 0U - a; break;
		case SC_JA:
			if (p->k >= ninsns - pc)
				return (0);
			pc += p->k;
			break;
		case SC_JK:
		case SC_JX:
			v = kind == SC_JK ? p->k : x;
			switch (p->code >> 4) {
			case 1: take = a == v; break;
			case 2: take = a > v; break;
			case 3: take = a >= v; break;
			case 4: take = (a & v) != 0; break;
			default: return (0);
			}
			jump = take ? p->jt : p->jf;
			if (jump >= ninsns - pc)
				return (0);
			pc += jump;
			break;
		case SC_RK: return (p->k);
		case SC_RA: return (a);
		case SC_TAX: x = a; break;
		case SC_TXA: a = x; break;
		default: return (0);
		}
	}
	return (0);
}
