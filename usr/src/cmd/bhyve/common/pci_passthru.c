/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include <sys/param.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/pciio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <sys/pci.h>

#include <dev/io/iodev.h>
#include <dev/pci/pcireg.h>

#include <machine/iodev.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sysexits.h>
#include <unistd.h>

#include <machine/vmm.h>
#include <vmmapi.h>
#include <sys/ppt_dev.h>

#include "config.h"
#include "debug.h"
#include "pci_passthru.h"
#include "mem.h"

#define	LEGACY_SUPPORT	1

#define MSIX_TABLE_COUNT(ctrl) (((ctrl) & PCIM_MSIXCTRL_TABLE_SIZE) + 1)
#define MSIX_CAPLEN 12
#define	PASSTHRU_NVIDIA_VENDOR_ID	0x10de
/*
 * TU102 startup compatibility path:
 *
 * The preserved working shape keeps the real guest BARs pinned high
 * (BAR1=0x800000000, BAR3=0x810000000, BAR0=0xc0000000). That is the
 * target layout we want to keep.
 *
 * The low-GPA aliases below are not the preferred steady-state model.
 * They exist only to cover the current firmware/GOP first-touch path that
 * still expects:
 *
 * - a low BAR0 firmware alias
 * - an initial low BAR1 compatibility aperture
 * - a low BAR3 alias plus the small BAR3 tail page
 * - post-BAR3 BAR1 windows that start trap-only and are promoted later
 *
 * Keep this block visibly isolated so it can be reduced or removed once the
 * proper startup path is understood.
 */
#define	PASSTHRU_TU102_BAR0_FW_ALIAS_GPA	0xc2000000ULL
#define	PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA	0x160000000ULL
#define	PASSTHRU_TU102_BAR3_FW_ALIAS_GPA	0x170000000ULL
#define	PASSTHRU_TU102_BAR3_TAIL_ALIAS_SIZE	0x1000ULL
#define	PASSTHRU_TU102_BAR1_PAGE_SIZE	0x1000ULL
#define	PASSTHRU_TU102_BAR1_TRACE_LIMIT	1024U
#define	PASSTHRU_TU102_BAR3_TAIL_TRACE_LIMIT	128U
#define	PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS	8U

/*
 * Detailed TU102 startup tracing is runtime-gated because the current
 * compatibility path can generate a large amount of warnx() traffic while
 * BARs are being churned. Keep the trace available for diagnosis, but make
 * it opt-in so the default startup path stays quieter.
 *
 * Enable with: BHYVE_PPT_TU102_TRACE=1
 */
static bool
passthru_tu102_trace_enabled(void)
{
	static int enabled = -1;
	const char *value;

	if (enabled == -1) {
		value = getenv("BHYVE_PPT_TU102_TRACE");
		enabled = (value != NULL && value[0] != '\0' &&
		    !(value[0] == '0' && value[1] == '\0')) ? 1 : 0;
	}

	return (enabled != 0);
}

static void
passthru_tu102_trace(const char *fmt, ...)
{
	va_list ap;

	if (!passthru_tu102_trace_enabled()) {
		return;
	}

	va_start(ap, fmt);
	vwarnx(fmt, ap);
	va_end(ap);
}

struct passthru_softc {
	struct pci_devinst *psc_pi;
	/* ROM is handled like a BAR */
	struct pcibar psc_bar[PCI_BARMAX_WITH_ROM + 1];
	uint64_t psc_bar_gpa[PCI_BARMAX_WITH_ROM + 1];
	uint8_t psc_bar_gpa_valid[PCI_BARMAX_WITH_ROM + 1];
	uint8_t psc_bar_mapped[PCI_BARMAX_WITH_ROM + 1];
	uint8_t psc_bar_fw_alias_mapped[PCI_BARMAX_WITH_ROM + 1];
	pthread_mutex_t psc_alias_lock;
	struct {
		uint64_t gpa;
		uint64_t map_len;
		uint32_t seq;
		uint8_t mapped;
		uint8_t trap_only;
	} psc_bar1_postbar3_cache[PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS];
	uint32_t psc_bar1_postbar3_seq;
	uint32_t psc_bar1_trace_count;
	uint8_t psc_bar1_trace_suppressed;
	uint8_t psc_bar1_alias_first_seen;
	uint8_t psc_bar1_transition_first_seen;
	uint32_t psc_bar3_tail_trace_count;
	uint8_t psc_bar3_tail_trace_suppressed;
	struct {
		int		capoff;
		int		msgctrl;
		int		emulated;
	} psc_msi;
	struct {
		int		capoff;
	} psc_msix;
	int pptfd;
	int msi_limit;
	int msix_limit;
	int psc_msi_host_active;
	int psc_intx_configured;
	int psc_intx_enabled;
	int psc_intx_irq;
	int psc_intx_ioctl_supported;

	cfgread_handler psc_pcir_rhandler[PCI_REGMAX + 1];
	cfgwrite_handler psc_pcir_whandler[PCI_REGMAX + 1];
};

static struct passthru_softc *passthru_tu102_active_sc;
static int passthru_host_bar_read(struct passthru_softc *, int, uint64_t, int,
    uint64_t *);
static int passthru_host_bar_write(struct passthru_softc *, int, uint64_t, int,
    uint64_t);
static void passthru_trace_tu102_bar1_mmio(struct passthru_softc *,
    const char *, uint64_t, uint64_t, int, uint64_t, int);
static void passthru_trace_tu102_bar1_transition(struct passthru_softc *,
    uint64_t);
static void passthru_trace_tu102_bar3_tail_mmio(struct passthru_softc *,
    const char *, uint64_t, uint64_t, int, uint64_t);
static int passthru_tu102_touch_bar1_postbar3_window_locked(
    struct passthru_softc *, uint64_t, uint64_t, int, int, uint64_t);
static void passthru_tu102_drop_bar1_postbar3_window_locked(
    struct passthru_softc *);
static bool passthru_tu102_map_bar1_window_page_locked(struct passthru_softc *,
    int, uint64_t);
static int passthru_setup_intx(struct vmctx *, int, int, int);
static int passthru_vm_setup_pptdev_msi(struct passthru_softc *,
    struct pci_devinst *, struct vmctx *, uint64_t, uint64_t, int);
static int passthru_vm_setup_pptdev_msix(struct passthru_softc *,
    struct pci_devinst *, struct vmctx *, int, uint64_t, uint64_t, uint32_t);

static int
passthru_nvidia_display_fn0(const struct passthru_softc *sc)
{
	struct pci_devinst *pi = sc->psc_pi;

	return (pci_get_cfgdata16(pi, PCIR_VENDOR) ==
	    PASSTHRU_NVIDIA_VENDOR_ID &&
	    pci_get_cfgdata8(pi, PCIR_CLASS) == PCIC_DISPLAY &&
	    pi->pi_func == 0);
}

static void
passthru_trace_tu102_bar1_mmio(struct passthru_softc *sc, const char *op,
    uint64_t fault_gpa, uint64_t window_gpa, int size, uint64_t value,
    int window_slot)
{
	struct pci_devinst *pi;
	uint64_t offset;

	if (sc == NULL || !passthru_nvidia_display_fn0(sc)) {
		return;
	}
	if (sc->psc_bar[1].size == 0) {
		return;
	}
	if (sc->psc_bar1_trace_count >= PASSTHRU_TU102_BAR1_TRACE_LIMIT) {
		if (sc->psc_bar1_trace_suppressed == 0) {
			passthru_tu102_trace("passthru: TU102 BAR1 MMIO trace suppressed "
			    "bdf=%d/%d/%d after %u accesses",
			    sc->psc_pi->pi_bus, sc->psc_pi->pi_slot,
			    sc->psc_pi->pi_func, PASSTHRU_TU102_BAR1_TRACE_LIMIT);
			sc->psc_bar1_trace_suppressed = 1;
		}
		return;
	}

	pi = sc->psc_pi;
	offset = fault_gpa - window_gpa;
	sc->psc_bar1_trace_count++;

	passthru_tu102_trace("passthru: TU102 BAR1_MMIO bdf=%d/%d/%d op=%s "
	    "fault_gpa=0x%lx window_gpa=0x%lx slot=%d offset=0x%lx "
	    "size=%d value=0x%lx host_hpa=0x%lx bar_size=0x%lx count=%u",
	    pi->pi_bus, pi->pi_slot, pi->pi_func,
	    op != NULL ? op : "unknown", (ulong_t)fault_gpa,
	    (ulong_t)window_gpa, window_slot, (ulong_t)offset, size,
	    (ulong_t)value, (ulong_t)(sc->psc_bar[1].addr + offset),
	    (ulong_t)sc->psc_bar[1].size, sc->psc_bar1_trace_count);
}

static void
passthru_trace_tu102_bar1_transition(struct passthru_softc *sc, uint64_t gpa)
{
	uint64_t alias_limit;
	uint64_t transition_base;
	uint64_t native_base;

	if (sc == NULL || !passthru_nvidia_display_fn0(sc) ||
	    sc->psc_bar[1].size == 0 || sc->psc_bar1_transition_first_seen != 0) {
		return;
	}

	alias_limit = PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA + sc->psc_bar[1].size;
	transition_base = PASSTHRU_TU102_BAR3_FW_ALIAS_GPA + sc->psc_bar[3].size;
	if (gpa < transition_base) {
		return;
	}

	native_base = sc->psc_bar_gpa_valid[1] != 0 ?
	    sc->psc_bar_gpa[1] : sc->psc_pi->pi_bar[1].addr;
	sc->psc_bar1_transition_first_seen = 1;
	passthru_tu102_trace("passthru: TU102 BAR1 transition attempt "
	    "fault_gpa=0x%lx alias_base=0x%lx alias_limit=0x%lx "
	    "bar3_alias_limit=0x%lx native_bar1_gpa=0x%lx native_bar1_limit=0x%lx",
	    (ulong_t)gpa, (ulong_t)PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA,
	    (ulong_t)alias_limit, (ulong_t)transition_base, (ulong_t)native_base,
	    (ulong_t)(native_base + sc->psc_bar[1].size));
}

static void
passthru_trace_tu102_bar3_tail_mmio(struct passthru_softc *sc, const char *op,
    uint64_t fault_gpa, uint64_t offset, int size, uint64_t value)
{
	struct pci_devinst *pi;

	if (sc == NULL || !passthru_nvidia_display_fn0(sc)) {
		return;
	}
	if (sc->psc_bar3_tail_trace_count >= PASSTHRU_TU102_BAR3_TAIL_TRACE_LIMIT) {
		if (sc->psc_bar3_tail_trace_suppressed == 0) {
			passthru_tu102_trace("passthru: TU102 BAR3 tail trace suppressed "
			    "bdf=%d/%d/%d after %u accesses",
			    sc->psc_pi->pi_bus, sc->psc_pi->pi_slot,
			    sc->psc_pi->pi_func, PASSTHRU_TU102_BAR3_TAIL_TRACE_LIMIT);
			sc->psc_bar3_tail_trace_suppressed = 1;
		}
		return;
	}

	pi = sc->psc_pi;
	sc->psc_bar3_tail_trace_count++;
	passthru_tu102_trace("passthru: TU102 BAR3_TAIL_MMIO bdf=%d/%d/%d op=%s "
	    "fault_gpa=0x%lx offset=0x%lx size=%d value=0x%lx "
	    "host_hpa=0x%lx bar_size=0x%lx count=%u",
	    pi->pi_bus, pi->pi_slot, pi->pi_func, op != NULL ? op : "unknown",
	    (ulong_t)fault_gpa, (ulong_t)offset, size, (ulong_t)value,
	    (ulong_t)(sc->psc_bar[3].addr + offset),
	    (ulong_t)sc->psc_bar[3].size, sc->psc_bar3_tail_trace_count);
}

static uint64_t
passthru_tu102_fw_alias_gpa(int baridx)
{
	switch (baridx) {
	case 0:
		return (PASSTHRU_TU102_BAR0_FW_ALIAS_GPA);
	case 1:
		return (PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA);
	case 3:
		return (PASSTHRU_TU102_BAR3_FW_ALIAS_GPA);
	default:
		return (0);
	}
}

static bool
passthru_tu102_addr_in_window(uint64_t gpa, uint64_t base, uint64_t size)
{
	return (size != 0 && gpa >= base && gpa - base < size);
}

static uint64_t
passthru_tu102_native_bar_gpa(const struct passthru_softc *sc, int baridx)
{
	if (sc->psc_bar_gpa_valid[baridx] != 0)
		return (sc->psc_bar_gpa[baridx]);
	return (sc->psc_pi->pi_bar[baridx].addr);
}

static uint64_t
passthru_tu102_bar1_postbar3_base(const struct passthru_softc *sc)
{
	/*
	 * This is the current compatibility handoff from the BAR3 tail page into
	 * the advancing post-BAR3 BAR1 windows. It is a temporary recovery
	 * mechanism, not the long-term preferred model; preserve it here so it
	 * can be reverted cleanly once startup no longer depends on it.
	 */
	return (PASSTHRU_TU102_BAR3_FW_ALIAS_GPA + sc->psc_bar[3].size +
	    PASSTHRU_TU102_BAR3_TAIL_ALIAS_SIZE);
}

static bool
passthru_tu102_bar1_postbar3_decode(const struct passthru_softc *sc,
    uint64_t gpa, uint64_t *window_basep, uint64_t *offsetp)
{
	uint64_t base;
	uint64_t size;
	uint64_t delta;

	size = sc->psc_bar[1].size;
	if (size == 0) {
		return (false);
	}

	base = passthru_tu102_bar1_postbar3_base(sc);
	if (gpa < base) {
		return (false);
	}

	delta = gpa - base;
	if (window_basep != NULL) {
		*window_basep = base + (delta / size) * size;
	}
	if (offsetp != NULL) {
		*offsetp = delta % size;
	}
	return (true);
}

static bool
passthru_tu102_bar3_tail_alias_decode(const struct passthru_softc *sc,
    uint64_t gpa, uint64_t *offsetp)
{
	uint64_t alias_base;

	if (sc->psc_bar[3].size < PASSTHRU_TU102_BAR3_TAIL_ALIAS_SIZE) {
		return (false);
	}

	alias_base = PASSTHRU_TU102_BAR3_FW_ALIAS_GPA + sc->psc_bar[3].size;
	if (!passthru_tu102_addr_in_window(gpa, alias_base,
	    PASSTHRU_TU102_BAR3_TAIL_ALIAS_SIZE)) {
		return (false);
	}

	if (offsetp != NULL) {
		*offsetp = sc->psc_bar[3].size - PASSTHRU_TU102_BAR3_TAIL_ALIAS_SIZE +
		    (gpa - alias_base);
	}
	return (true);
}

static int
passthru_tu102_alias_decode(const struct passthru_softc *sc, uint64_t gpa,
    int *baridxp, uint64_t *offsetp)
{
	uint64_t size;

	size = sc->psc_bar[0].size;
	if (passthru_tu102_addr_in_window(gpa, PASSTHRU_TU102_BAR0_FW_ALIAS_GPA,
	    size)) {
		*baridxp = 0;
		*offsetp = gpa - PASSTHRU_TU102_BAR0_FW_ALIAS_GPA;
		return (1);
	}

	size = sc->psc_bar[1].size;
	if (passthru_tu102_addr_in_window(gpa, PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA,
	    size)) {
		*baridxp = 1;
		*offsetp = gpa - PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA;
		return (1);
	}
	if (passthru_tu102_addr_in_window(gpa,
	    passthru_tu102_native_bar_gpa(sc, 1), size)) {
		*baridxp = 1;
		*offsetp = gpa - passthru_tu102_native_bar_gpa(sc, 1);
		return (1);
	}
	for (size_t i = 0; i < PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS; i++) {
		if (sc->psc_bar1_postbar3_cache[i].mapped != 0 &&
		    passthru_tu102_addr_in_window(gpa,
		    sc->psc_bar1_postbar3_cache[i].gpa, size)) {
			*baridxp = 1;
			*offsetp = gpa - sc->psc_bar1_postbar3_cache[i].gpa;
			return (1);
		}
	}

	size = sc->psc_bar[3].size;
	if (passthru_tu102_addr_in_window(gpa, PASSTHRU_TU102_BAR3_FW_ALIAS_GPA,
	    size)) {
		*baridxp = 3;
		*offsetp = gpa - PASSTHRU_TU102_BAR3_FW_ALIAS_GPA;
		return (1);
	}
	if (passthru_tu102_bar3_tail_alias_decode(sc, gpa, offsetp)) {
		*baridxp = 3;
		return (1);
	}

	return (0);
}

static int
passthru_tu102_map_region(struct passthru_softc *sc, int baridx,
    uint64_t gpa, const char *what)
{
	struct vmctx *ctx = sc->psc_pi->pi_vmctx;

	if (vm_map_pptdev_mmio(ctx, sc->pptfd, gpa, sc->psc_bar[baridx].size,
	    sc->psc_bar[baridx].addr) != 0) {
		warnx("pci_passthru: TU102 %s map failed bar=%d gpa=0x%lx "
		    "len=0x%lx hpa=0x%lx", what, baridx, (ulong_t)gpa,
		    (ulong_t)sc->psc_bar[baridx].size,
		    (ulong_t)sc->psc_bar[baridx].addr);
		return (0);
	}

	passthru_tu102_trace("passthru: TU102 %s map bar=%d gpa=0x%lx len=0x%lx hpa=0x%lx",
	    what, baridx, (ulong_t)gpa, (ulong_t)sc->psc_bar[baridx].size,
	    (ulong_t)sc->psc_bar[baridx].addr);
	return (1);
}

static void
passthru_tu102_map_subregion_log(const struct passthru_softc *sc, int baridx,
    uint64_t gpa, uint64_t len, uint64_t hpa, const char *what)
{
	passthru_tu102_trace("passthru: TU102 %s map bar=%d gpa=0x%lx len=0x%lx hpa=0x%lx",
	    what, baridx, (ulong_t)gpa, (ulong_t)len, (ulong_t)hpa);
}

static int
passthru_tu102_map_subregion(struct passthru_softc *sc, int baridx,
    uint64_t gpa, uint64_t len, uint64_t hpa_offset, const char *what)
{
	struct vmctx *ctx = sc->psc_pi->pi_vmctx;
	uint64_t hpa;

	hpa = sc->psc_bar[baridx].addr + hpa_offset;
	if (vm_map_pptdev_mmio(ctx, sc->pptfd, gpa, len, hpa) != 0) {
		warnx("pci_passthru: TU102 %s map failed bar=%d gpa=0x%lx "
		    "len=0x%lx hpa=0x%lx", what, baridx, (ulong_t)gpa,
		    (ulong_t)len, (ulong_t)hpa);
		return (0);
	}

	passthru_tu102_map_subregion_log(sc, baridx, gpa, len, hpa, what);
	return (1);
}

static void
passthru_tu102_unmap_region(struct passthru_softc *sc, int baridx,
    uint64_t gpa, const char *what)
{
	struct vmctx *ctx = sc->psc_pi->pi_vmctx;

	if (vm_unmap_pptdev_mmio(ctx, sc->pptfd, gpa,
	    sc->psc_bar[baridx].size) != 0) {
		warnx("pci_passthru: TU102 %s unmap failed bar=%d gpa=0x%lx",
		    what, baridx, (ulong_t)gpa);
	}
}

static int
passthru_tu102_unmap_subregion(struct passthru_softc *sc, int baridx,
    uint64_t gpa, uint64_t len, const char *what)
{
	struct vmctx *ctx = sc->psc_pi->pi_vmctx;

	if (vm_unmap_pptdev_mmio(ctx, sc->pptfd, gpa, len) != 0) {
		warnx("pci_passthru: TU102 %s unmap failed bar=%d gpa=0x%lx "
		    "len=0x%lx", what, baridx, (ulong_t)gpa, (ulong_t)len);
		return (0);
	}
	return (1);
}

static int
passthru_tu102_ensure_alias_locked(struct passthru_softc *sc, int baridx)
{
	const uint64_t gpa = passthru_tu102_fw_alias_gpa(baridx);

	if (gpa == 0 || sc->psc_bar_mapped[baridx] == 0) {
		return (0);
	}
	if (sc->psc_bar_fw_alias_mapped[baridx] != 0) {
		return (0);
	}
	if (!passthru_tu102_map_region(sc, baridx, gpa, "fixed-alias")) {
		return (0);
	}

	sc->psc_bar_fw_alias_mapped[baridx] = 1;
	return (1);
}

static void
passthru_tu102_drop_alias_locked(struct passthru_softc *sc, int baridx)
{
	const uint64_t gpa = passthru_tu102_fw_alias_gpa(baridx);

	if (gpa == 0 || sc->psc_bar_fw_alias_mapped[baridx] == 0) {
		return;
	}

	passthru_tu102_unmap_region(sc, baridx, gpa, "fixed-alias");
	sc->psc_bar_fw_alias_mapped[baridx] = 0;
}

static int
passthru_tu102_touch_bar1_postbar3_window_locked(struct passthru_softc *sc,
    uint64_t window_gpa, uint64_t fault_gpa, int is_read, int size,
    uint64_t value)
{
	size_t free_slot = PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS;
	size_t victim_slot = PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS;
	uint32_t victim_seq = UINT32_MAX;
	uint64_t old_gpa = 0;
	bool trap_only;
	bool hit = false;
	size_t i;

	if (sc->psc_bar_mapped[1] == 0) {
		return (0);
	}

	for (i = 0; i < PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS; i++) {
		if (sc->psc_bar1_postbar3_cache[i].mapped != 0 &&
		    sc->psc_bar1_postbar3_cache[i].gpa == window_gpa) {
			hit = true;
			sc->psc_bar1_postbar3_cache[i].seq =
			    ++sc->psc_bar1_postbar3_seq;
			if (fault_gpa != 0) {
				passthru_tu102_trace("passthru: TU102 postbar3-window hit slot=%lu "
				    "gpa=0x%lx trap_only=%u fault_gpa=0x%lx off=0x%lx op=%s "
				    "size=%d value=0x%lx", (ulong_t)i,
				    (ulong_t)window_gpa,
				    sc->psc_bar1_postbar3_cache[i].trap_only,
				    (ulong_t)fault_gpa,
				    (ulong_t)(fault_gpa - window_gpa),
				    is_read != 0 ? "read" : "write", size,
				    (ulong_t)value);
			}
			return (0);
		}
		if (sc->psc_bar1_postbar3_cache[i].mapped == 0) {
			if (free_slot == PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS) {
				free_slot = i;
			}
			continue;
		}
		if (sc->psc_bar1_postbar3_cache[i].seq < victim_seq) {
			victim_seq = sc->psc_bar1_postbar3_cache[i].seq;
			victim_slot = i;
		}
	}

	if (hit) {
		return (0);
	}

	i = free_slot != PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS ?
	    free_slot : victim_slot;
	if (i == PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS) {
		return (0);
	}
	/*
	 * Observe the first page of every new post-tail BAR1 slice before
	 * promoting the whole window. The first slice already showed a linear
	 * 0xff fill across its first page, and we need to know if the later
	 * slices follow the same pattern.
	 */
	trap_only = true;
	if (sc->psc_bar1_postbar3_cache[i].mapped != 0) {
		old_gpa = sc->psc_bar1_postbar3_cache[i].gpa;
		if (sc->psc_bar1_postbar3_cache[i].map_len != 0) {
			(void) passthru_tu102_unmap_subregion(sc, 1, old_gpa,
			    sc->psc_bar1_postbar3_cache[i].map_len,
			    "postbar3-window");
		}
	}
	if (!trap_only &&
	    !passthru_tu102_map_region(sc, 1, window_gpa, "postbar3-window")) {
		return (0);
	}
	sc->psc_bar1_postbar3_cache[i].gpa = window_gpa;
	sc->psc_bar1_postbar3_cache[i].map_len = 0;
	sc->psc_bar1_postbar3_cache[i].mapped = 1;
	sc->psc_bar1_postbar3_cache[i].trap_only = trap_only ? 1 : 0;
	sc->psc_bar1_postbar3_cache[i].seq = ++sc->psc_bar1_postbar3_seq;
	sc->psc_bar1_trace_count = 0;
	sc->psc_bar1_trace_suppressed = 0;
	if (fault_gpa == 0) {
		passthru_tu102_trace("passthru: TU102 postbar3-window seed slot=%lu gpa=0x%lx trap_only=%u",
		    (ulong_t)i, (ulong_t)window_gpa,
		    sc->psc_bar1_postbar3_cache[i].trap_only);
	} else {
		passthru_tu102_trace("passthru: TU102 postbar3-window %s slot=%lu old_gpa=0x%lx "
		    "new_gpa=0x%lx trap_only=%u fault_gpa=0x%lx off=0x%lx op=%s size=%d "
		    "value=0x%lx", old_gpa != 0 ? "evict" : "fill",
		    (ulong_t)i, (ulong_t)old_gpa, (ulong_t)window_gpa,
		    sc->psc_bar1_postbar3_cache[i].trap_only,
		    (ulong_t)fault_gpa, (ulong_t)(fault_gpa - window_gpa),
		    is_read != 0 ? "read" : "write", size, (ulong_t)value);
	}
	return (1);
}

static int
passthru_tu102_bar1_window_slot_locked(struct passthru_softc *sc,
    uint64_t window_gpa)
{
	size_t i;

	for (i = 0; i < PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS; i++) {
		if (sc->psc_bar1_postbar3_cache[i].mapped != 0 &&
		    sc->psc_bar1_postbar3_cache[i].gpa == window_gpa) {
			return ((int)i);
		}
	}

	return (-1);
}

static bool
passthru_tu102_map_bar1_window_page_locked(struct passthru_softc *sc,
    int window_slot, uint64_t window_gpa)
{
	size_t victim = PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS;
	uint32_t victim_seq = UINT32_MAX;
	size_t i;

	if (window_slot < 0 ||
	    window_slot >= PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS) {
		return (false);
	}
	if (passthru_tu102_map_subregion(sc, 1, window_gpa,
	    PASSTHRU_TU102_BAR1_PAGE_SIZE, 0, "postbar3-window-page")) {
		sc->psc_bar1_postbar3_cache[window_slot].map_len =
		    PASSTHRU_TU102_BAR1_PAGE_SIZE;
		sc->psc_bar1_postbar3_cache[window_slot].trap_only = 0;
		sc->psc_bar1_postbar3_cache[window_slot].seq =
		    ++sc->psc_bar1_postbar3_seq;
		return (true);
	}

	for (i = 0; i < PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS; i++) {
		if ((int)i == window_slot)
			continue;
		if (sc->psc_bar1_postbar3_cache[i].mapped == 0 ||
		    sc->psc_bar1_postbar3_cache[i].trap_only != 0)
			continue;
		if (sc->psc_bar1_postbar3_cache[i].seq < victim_seq) {
			victim = i;
			victim_seq = sc->psc_bar1_postbar3_cache[i].seq;
		}
	}
	if (victim == PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS)
		return (false);

	passthru_tu102_trace("passthru: TU102 postbar3-window promote retry slot=%d "
	    "gpa=0x%lx evict_slot=%lu evict_gpa=0x%lx", window_slot,
	    (ulong_t)window_gpa, (ulong_t)victim,
	    (ulong_t)sc->psc_bar1_postbar3_cache[victim].gpa);
	(void) passthru_tu102_unmap_subregion(sc, 1,
	    sc->psc_bar1_postbar3_cache[victim].gpa,
	    sc->psc_bar1_postbar3_cache[victim].map_len,
	    "postbar3-window");
	sc->psc_bar1_postbar3_cache[victim].gpa = 0;
	sc->psc_bar1_postbar3_cache[victim].map_len = 0;
	sc->psc_bar1_postbar3_cache[victim].mapped = 0;
	sc->psc_bar1_postbar3_cache[victim].trap_only = 0;
	sc->psc_bar1_postbar3_cache[victim].seq = 0;

	if (!passthru_tu102_map_subregion(sc, 1, window_gpa,
	    PASSTHRU_TU102_BAR1_PAGE_SIZE, 0, "postbar3-window-page")) {
		return (false);
	}

	sc->psc_bar1_postbar3_cache[window_slot].map_len =
	    PASSTHRU_TU102_BAR1_PAGE_SIZE;
	sc->psc_bar1_postbar3_cache[window_slot].trap_only = 0;
	sc->psc_bar1_postbar3_cache[window_slot].seq =
	    ++sc->psc_bar1_postbar3_seq;
	return (true);
}

static void
passthru_tu102_drop_bar1_postbar3_window_locked(struct passthru_softc *sc)
{
	size_t i;

	for (i = 0; i < PASSTHRU_TU102_BAR1_POSTBAR3_CACHE_SLOTS; i++) {
		if (sc->psc_bar1_postbar3_cache[i].mapped == 0) {
			continue;
		}
		if (sc->psc_bar1_postbar3_cache[i].map_len != 0) {
			(void) passthru_tu102_unmap_subregion(sc, 1,
			    sc->psc_bar1_postbar3_cache[i].gpa,
			    sc->psc_bar1_postbar3_cache[i].map_len,
			    "postbar3-window");
		}
		sc->psc_bar1_postbar3_cache[i].gpa = 0;
		sc->psc_bar1_postbar3_cache[i].map_len = 0;
		sc->psc_bar1_postbar3_cache[i].mapped = 0;
		sc->psc_bar1_postbar3_cache[i].trap_only = 0;
		sc->psc_bar1_postbar3_cache[i].seq = 0;
	}
	sc->psc_bar1_postbar3_seq = 0;
}

/*
 * Temporary TU102 startup quirk:
 *
 * Guest firmware churn on TU102 touches PCI_COMMAND aggressively while it
 * probes the option ROM and sibling functions. Forwarding those writes to the
 * physical device has been enough to collapse BAR0 / config-mirror access, so
 * keep the guest-visible shadow exact while clamping the host side. Keep this
 * quirk fenced off so we can delete it once a cleaner startup path exists.
 */
static int
passthru_tu102_host_cmd_sticky_quirk(struct pci_devinst *pi)
{
	return (passthru_nvidia_display_fn0((struct passthru_softc *)pi->pi_arg));
}

static int
passthru_env_enabled(const char *name)
{
	const char *v = getenv(name);

	if (v == NULL || *v == '\0')
		return (0);
	if (strcmp(v, "0") == 0)
		return (0);
	return (1);
}

static int
passthru_tu102_host_cmd_shadow_quirk(struct pci_devinst *pi)
{
	return (passthru_tu102_host_cmd_sticky_quirk(pi));
}

/*
 * Keep the old TU102 fn0 INTx bootstrap knobs available while the runtime MSI
 * path is still under recovery.
 */
static int
passthru_intx_bootstrap_quirk(struct pci_devinst *pi)
{
	const char *v;

	v = getenv("BHYVE_PPT_INTX_BOOTSTRAP");
	if (v != NULL && (*v == '\0' || strcmp(v, "0") == 0))
		return (0);

	return (passthru_tu102_host_cmd_sticky_quirk(pi));
}

static int
passthru_intx_force_on_quirk(struct pci_devinst *pi)
{
	const char *v;

	v = getenv("BHYVE_PPT_INTX_FORCE_ON");
	if (v == NULL || *v == '\0' || strcmp(v, "0") == 0)
		return (0);

	return (passthru_tu102_host_cmd_sticky_quirk(pi) != 0);
}

static int
msi_caplen(int msgctrl)
{
	int len;

	len = 10;		/* minimum length of msi capability */

	if (msgctrl & PCIM_MSICTRL_64BIT)
		len += 4;

#if 0
	/*
	 * Ignore the 'mask' and 'pending' bits in the MSI capability.
	 * We'll let the guest manipulate them directly.
	 */
	if (msgctrl & PCIM_MSICTRL_VECTOR)
		len += 10;
#endif

	return (len);
}

static uint32_t
passthru_read_config(const struct passthru_softc *sc, long reg, int width)
{
	struct ppt_cfg_io pi;

	pi.pci_off = reg;
	pi.pci_width = width;

	if (ioctl(sc->pptfd, PPT_CFG_READ, &pi) != 0) {
		return (0);
	}
	return (pi.pci_data);
}

static int
passthru_setup_intx(struct vmctx *ctx __unused, int pptfd, int ioapic_irq,
    int enable)
{
	struct ppt_intx pi;

	bzero(&pi, sizeof (pi));
	pi.ioapic_irq = ioapic_irq;
	pi.enable = enable;
	return (ioctl(pptfd, PPT_INTX_SETUP, &pi));
}

static int
passthru_host_bar_read(struct passthru_softc *sc, int baridx, uint64_t offset,
    int size, uint64_t *val)
{
	struct ppt_bar_io pbi;
	uint64_t lo;
	uint64_t hi;

	if (size == 8) {
		if (passthru_host_bar_read(sc, baridx, offset, 4, &lo) != 0 ||
		    passthru_host_bar_read(sc, baridx, offset + 4, 4, &hi) != 0) {
			return (-1);
		}
		*val = lo | (hi << 32);
		return (0);
	}

	bzero(&pbi, sizeof (pbi));
	pbi.pbi_bar = baridx;
	pbi.pbi_width = size;
	pbi.pbi_off = offset;

	if (ioctl(sc->pptfd, PPT_BAR_READ, &pbi) != 0) {
		return (-1);
	}

	*val = pbi.pbi_data;
	return (0);
}

static int
passthru_host_bar_write(struct passthru_softc *sc, int baridx, uint64_t offset,
    int size, uint64_t val)
{
	struct ppt_bar_io pbi;

	if (size == 8) {
		if (passthru_host_bar_write(sc, baridx, offset, 4,
		    val & 0xffffffffU) != 0 ||
		    passthru_host_bar_write(sc, baridx, offset + 4, 4,
		    val >> 32) != 0) {
			return (-1);
		}
		return (0);
	}

	bzero(&pbi, sizeof (pbi));
	pbi.pbi_bar = baridx;
	pbi.pbi_width = size;
	pbi.pbi_off = offset;
	pbi.pbi_data = val;

	return (ioctl(sc->pptfd, PPT_BAR_WRITE, &pbi));
}

static void
passthru_write_config(const struct passthru_softc *sc, long reg, int width,
    uint32_t data)
{
	struct ppt_cfg_io pi;

	pi.pci_off = reg;
	pi.pci_width = width;
	pi.pci_data = data;

	(void) ioctl(sc->pptfd, PPT_CFG_WRITE, &pi);
}

static int
passthru_get_bar(struct passthru_softc *sc, int bar, enum pcibar_type *type,
    uint64_t *base, uint64_t *size)
{
	struct ppt_bar_query pb;

	pb.pbq_baridx = bar;

	if (ioctl(sc->pptfd, PPT_BAR_QUERY, &pb) != 0) {
		return (-1);
	}

	switch (pb.pbq_type) {
	case PCI_ADDR_IO:
		*type = PCIBAR_IO;
		break;
	case PCI_ADDR_MEM32:
		*type = PCIBAR_MEM32;
		break;
	case PCI_ADDR_MEM64:
		*type = PCIBAR_MEM64;
		break;
	default:
		err(1, "unrecognized BAR type: %u\n", pb.pbq_type);
		break;
	}

	*base = pb.pbq_base;
	*size = pb.pbq_size;
	return (0);
}

static int
passthru_dev_open(const char *path, int *pptfdp)
{
	int pptfd;

	if ((pptfd = open(path, O_RDWR)) < 0) {
		return (errno);
	}

	/* XXX: verify fd with ioctl? */
	*pptfdp = pptfd;
	return (0);
}

#ifdef LEGACY_SUPPORT
static int
passthru_add_msicap(struct pci_devinst *pi, int msgnum, int nextptr)
{
	int capoff;
	struct msicap msicap;
	u_char *capdata;

	pci_populate_msicap(&msicap, msgnum, nextptr);

	/*
	 * XXX
	 * Copy the msi capability structure in the last 16 bytes of the
	 * config space. This is wrong because it could shadow something
	 * useful to the device.
	 */
	capoff = 256 - roundup(sizeof(msicap), 4);
	capdata = (u_char *)&msicap;
	for (size_t i = 0; i < sizeof(msicap); i++)
		pci_set_cfgdata8(pi, capoff + i, capdata[i]);

	return (capoff);
}
#endif	/* LEGACY_SUPPORT */

static void
passthru_intr_limit(struct passthru_softc *sc, struct msixcap *msixcap)
{
	struct pci_devinst *pi = sc->psc_pi;
	int off;

	/* Reduce the number of MSI vectors if higher than OS limit */
	if ((off = sc->psc_msi.capoff) != 0 && sc->msi_limit != -1) {
		int msi_limit, mmc;

		msi_limit =
		    sc->msi_limit > 16 ? PCIM_MSICTRL_MMC_32 :
		    sc->msi_limit > 8 ? PCIM_MSICTRL_MMC_16 :
		    sc->msi_limit > 4 ? PCIM_MSICTRL_MMC_8 :
		    sc->msi_limit > 2 ? PCIM_MSICTRL_MMC_4 :
		    sc->msi_limit > 1 ? PCIM_MSICTRL_MMC_2 :
		    PCIM_MSICTRL_MMC_1;
		mmc = sc->psc_msi.msgctrl & PCIM_MSICTRL_MMC_MASK;

		if (mmc > msi_limit) {
			sc->psc_msi.msgctrl &= ~PCIM_MSICTRL_MMC_MASK;
			sc->psc_msi.msgctrl |= msi_limit;
			pci_set_cfgdata16(pi, off + 2, sc->psc_msi.msgctrl);
		}
	}

	/* Reduce the number of MSI-X vectors if higher than OS limit */
	if ((off = sc->psc_msix.capoff) != 0 && sc->msix_limit != -1) {
		if (MSIX_TABLE_COUNT(msixcap->msgctrl) > sc->msix_limit) {
			msixcap->msgctrl &= ~PCIM_MSIXCTRL_TABLE_SIZE;
			msixcap->msgctrl |= sc->msix_limit - 1;
			pci_set_cfgdata16(pi, off + 2, msixcap->msgctrl);
		}
	}
}

static int
cfginitmsi(struct passthru_softc *sc)
{
	int i, ptr, capptr, cap, sts, caplen, table_size;
	uint32_t u32;
	struct pci_devinst *pi = sc->psc_pi;
	struct msixcap msixcap;
	char *msixcap_ptr;

	/*
	 * Parse the capabilities and cache the location of the MSI
	 * and MSI-X capabilities.
	 */
	sts = passthru_read_config(sc, PCIR_STATUS, 2);
	if (sts & PCIM_STATUS_CAPPRESENT) {
		ptr = passthru_read_config(sc, PCIR_CAP_PTR, 1);
		while (ptr != 0 && ptr != 0xff) {
			cap = passthru_read_config(sc, ptr + PCICAP_ID, 1);
			if (cap == PCIY_MSI) {
				/*
				 * Copy the MSI capability into the config
				 * space of the emulated pci device
				 */
				sc->psc_msi.capoff = ptr;
				sc->psc_msi.msgctrl = passthru_read_config(sc,
				    ptr + 2, 2);
				sc->psc_msi.emulated = 0;
				caplen = msi_caplen(sc->psc_msi.msgctrl);
				capptr = ptr;
				while (caplen > 0) {
					u32 = passthru_read_config(sc,
					    capptr, 4);
					pci_set_cfgdata32(pi, capptr, u32);
					caplen -= 4;
					capptr += 4;
				}
			} else if (cap == PCIY_MSIX) {
				/*
				 * Copy the MSI-X capability
				 */
				sc->psc_msix.capoff = ptr;
				caplen = 12;
				msixcap_ptr = (char *)&msixcap;
				capptr = ptr;
				while (caplen > 0) {
					u32 = passthru_read_config(sc,
					    capptr, 4);
					memcpy(msixcap_ptr, &u32, 4);
					pci_set_cfgdata32(pi, capptr, u32);
					caplen -= 4;
					capptr += 4;
					msixcap_ptr += 4;
				}
			}
			ptr = passthru_read_config(sc, ptr + PCICAP_NEXTPTR, 1);
		}
	}

	passthru_intr_limit(sc, &msixcap);

	if (sc->psc_msix.capoff != 0) {
		pi->pi_msix.pba_bar =
		    msixcap.pba_info & PCIM_MSIX_BIR_MASK;
		pi->pi_msix.pba_offset =
		    msixcap.pba_info & ~PCIM_MSIX_BIR_MASK;
		pi->pi_msix.table_bar =
		    msixcap.table_info & PCIM_MSIX_BIR_MASK;
		pi->pi_msix.table_offset =
		    msixcap.table_info & ~PCIM_MSIX_BIR_MASK;
		pi->pi_msix.table_count = MSIX_TABLE_COUNT(msixcap.msgctrl);
		pi->pi_msix.pba_size = PBA_SIZE(pi->pi_msix.table_count);

		/* Allocate the emulated MSI-X table array */
		table_size = pi->pi_msix.table_count * MSIX_TABLE_ENTRY_SIZE;
		pi->pi_msix.table = calloc(1, table_size);

		/* Mask all table entries */
		for (i = 0; i < pi->pi_msix.table_count; i++) {
			pi->pi_msix.table[i].vector_control |=
						PCIM_MSIX_VCTRL_MASK;
		}
	}

#ifdef LEGACY_SUPPORT
	/*
	 * If the passthrough device does not support MSI then craft a
	 * MSI capability for it. We link the new MSI capability at the
	 * head of the list of capabilities.
	 */
	if ((sts & PCIM_STATUS_CAPPRESENT) != 0 && sc->psc_msi.capoff == 0) {
		int origptr, msiptr;
		origptr = passthru_read_config(sc, PCIR_CAP_PTR, 1);
		msiptr = passthru_add_msicap(pi, 1, origptr);
		sc->psc_msi.capoff = msiptr;
		sc->psc_msi.msgctrl = pci_get_cfgdata16(pi, msiptr + 2);
		sc->psc_msi.emulated = 1;
		pci_set_cfgdata8(pi, PCIR_CAP_PTR, msiptr);
	}
#endif

	/* Make sure one of the capabilities is present */
	if (sc->psc_msi.capoff == 0 && sc->psc_msix.capoff == 0)
		return (-1);
	else
		return (0);
}

static uint64_t
msix_table_read(struct passthru_softc *sc, uint64_t offset, int size)
{
	struct pci_devinst *pi;
	struct msix_table_entry *entry;
	uint8_t *src8;
	uint16_t *src16;
	uint32_t *src32;
	uint64_t *src64;
	uint64_t data;
	size_t entry_offset;
	uint32_t table_offset;
	int index, table_count;

	pi = sc->psc_pi;

	table_offset = pi->pi_msix.table_offset;
	table_count = pi->pi_msix.table_count;
	if (offset < table_offset ||
	    offset >= table_offset + table_count * MSIX_TABLE_ENTRY_SIZE) {
		switch (size) {
		case 1:
			src8 = (uint8_t *)(pi->pi_msix.mapped_addr + offset);
			data = *src8;
			break;
		case 2:
			src16 = (uint16_t *)(pi->pi_msix.mapped_addr + offset);
			data = *src16;
			break;
		case 4:
			src32 = (uint32_t *)(pi->pi_msix.mapped_addr + offset);
			data = *src32;
			break;
		case 8:
			src64 = (uint64_t *)(pi->pi_msix.mapped_addr + offset);
			data = *src64;
			break;
		default:
			return (-1);
		}
		return (data);
	}

	offset -= table_offset;
	index = offset / MSIX_TABLE_ENTRY_SIZE;
	assert(index < table_count);

	entry = &pi->pi_msix.table[index];
	entry_offset = offset % MSIX_TABLE_ENTRY_SIZE;

	switch (size) {
	case 1:
		src8 = (uint8_t *)((uint8_t *)entry + entry_offset);
		data = *src8;
		break;
	case 2:
		src16 = (uint16_t *)((uint8_t *)entry + entry_offset);
		data = *src16;
		break;
	case 4:
		src32 = (uint32_t *)((uint8_t *)entry + entry_offset);
		data = *src32;
		break;
	case 8:
		src64 = (uint64_t *)((uint8_t *)entry + entry_offset);
		data = *src64;
		break;
	default:
		return (-1);
	}

	return (data);
}

static void
msix_table_write(struct vmctx *ctx, struct passthru_softc *sc,
		 uint64_t offset, int size, uint64_t data)
{
	struct pci_devinst *pi;
	struct msix_table_entry *entry;
	uint8_t *dest8;
	uint16_t *dest16;
	uint32_t *dest32;
	uint64_t *dest64;
	size_t entry_offset;
	uint32_t table_offset, vector_control;
	int index, table_count;

	pi = sc->psc_pi;

	table_offset = pi->pi_msix.table_offset;
	table_count = pi->pi_msix.table_count;
	if (offset < table_offset ||
	    offset >= table_offset + table_count * MSIX_TABLE_ENTRY_SIZE) {
		switch (size) {
		case 1:
			dest8 = (uint8_t *)(pi->pi_msix.mapped_addr + offset);
			*dest8 = data;
			break;
		case 2:
			dest16 = (uint16_t *)(pi->pi_msix.mapped_addr + offset);
			*dest16 = data;
			break;
		case 4:
			dest32 = (uint32_t *)(pi->pi_msix.mapped_addr + offset);
			*dest32 = data;
			break;
		case 8:
			dest64 = (uint64_t *)(pi->pi_msix.mapped_addr + offset);
			*dest64 = data;
			break;
		}
		return;
	}

	offset -= table_offset;
	index = offset / MSIX_TABLE_ENTRY_SIZE;
	assert(index < table_count);

	entry = &pi->pi_msix.table[index];
	entry_offset = offset % MSIX_TABLE_ENTRY_SIZE;

	/* Only 4 byte naturally-aligned writes are supported */
	assert(size == 4);
	assert(entry_offset % 4 == 0);

	vector_control = entry->vector_control;
	dest32 = (uint32_t *)((uint8_t *)entry + entry_offset);
	*dest32 = data;
	/* If MSI-X hasn't been enabled, do nothing */
	if (pi->pi_msix.enabled) {
		/* If the entry is masked, don't set it up */
		if ((entry->vector_control & PCIM_MSIX_VCTRL_MASK) == 0 ||
		    (vector_control & PCIM_MSIX_VCTRL_MASK) == 0) {
			(void) passthru_vm_setup_pptdev_msix(sc, pi, ctx,
			    index, entry->addr, entry->msg_data,
			    entry->vector_control);
		}
	}
}

static int
init_msix_table(struct vmctx *ctx __unused, struct passthru_softc *sc)
{
	struct pci_devinst *pi = sc->psc_pi;
	uint32_t table_size, table_offset;
	int i;

	i = pci_msix_table_bar(pi);
	assert(i >= 0);

        /*
         * Map the region of the BAR containing the MSI-X table.  This is
         * necessary for two reasons:
         * 1. The PBA may reside in the first or last page containing the MSI-X
         *    table.
         * 2. While PCI devices are not supposed to use the page(s) containing
         *    the MSI-X table for other purposes, some do in practice.
         */

	/*
	 * Mapping pptfd provides access to the BAR containing the MSI-X
	 * table. See ppt_devmap() in usr/src/uts/intel/io/vmm/io/ppt.c
	 *
	 * This maps the whole BAR and then mprotect(PROT_NONE) is used below
	 * to prevent access to pages that don't contain the MSI-X table.
	 * When porting this, it was tempting to just map the MSI-X table pages
	 * but that would mean updating everywhere that assumes that
	 * pi->pi_msix.mapped_addr points to the start of the BAR. For now,
	 * keep closer to upstream.
	 */
	pi->pi_msix.mapped_size = sc->psc_bar[i].size;
	pi->pi_msix.mapped_addr = (uint8_t *)mmap(NULL, pi->pi_msix.mapped_size,
	    PROT_READ | PROT_WRITE, MAP_SHARED, sc->pptfd, 0);
	if (pi->pi_msix.mapped_addr == MAP_FAILED) {
		warn("Failed to map MSI-X table BAR on %d", sc->pptfd);
		return (-1);
	}

	table_offset = rounddown2(pi->pi_msix.table_offset, 4096);

	table_size = pi->pi_msix.table_offset - table_offset;
	table_size += pi->pi_msix.table_count * MSIX_TABLE_ENTRY_SIZE;
	table_size = roundup2(table_size, 4096);

	/*
	 * Unmap any pages not containing the table, we do not need to emulate
	 * accesses to them.  Avoid releasing address space to help ensure that
	 * a buggy out-of-bounds access causes a crash.
	 */
	if (table_offset != 0)
		if (mprotect((caddr_t)pi->pi_msix.mapped_addr, table_offset,
		    PROT_NONE) != 0)
			warn("Failed to unmap MSI-X table BAR region");
	if (table_offset + table_size != pi->pi_msix.mapped_size)
		if (mprotect((caddr_t)
		    pi->pi_msix.mapped_addr + table_offset + table_size,
		    pi->pi_msix.mapped_size - (table_offset + table_size),
		    PROT_NONE) != 0)
			warn("Failed to unmap MSI-X table BAR region");

	return (0);
}

static int
passthru_vm_setup_pptdev_msi(struct passthru_softc *sc, struct pci_devinst *pi,
    struct vmctx *ctx, uint64_t addr, uint64_t data, int numvec)
{
	int rc;
	int saved_irq = -1;

	if (numvec == 0 && !sc->psc_msi_host_active) {
		return (0);
	}

	if (numvec > 0 && sc->psc_intx_ioctl_supported &&
	    sc->psc_intx_configured && sc->psc_intx_enabled &&
	    !passthru_intx_force_on_quirk(pi)) {
		saved_irq = sc->psc_intx_irq;
		if (saved_irq <= 0)
			saved_irq = pi->pi_lintr.ioapic_irq;
		rc = passthru_setup_intx(ctx, sc->pptfd, 0, 0);
		if (rc != 0) {
			if (errno == ENOTSUP)
				sc->psc_intx_ioctl_supported = 0;
			return (rc);
		}
		sc->psc_intx_configured = 1;
		sc->psc_intx_enabled = 0;
		sc->psc_intx_irq = 0;
	}

	rc = vm_setup_pptdev_msi(ctx, sc->pptfd, addr, data, numvec);
	if (rc != 0) {
		warnx("ppt msi setup failed bdf=%d:%d:%d pptfd=%d addr=0x%llx "
		    "data=0x%llx numvec=%d intx_cfg=%d intx_en=%d intx_irq=%d "
		    "msi_en=%d msix_en=%d host_msi_active=%d",
		    pi->pi_bus, pi->pi_slot, pi->pi_func, sc->pptfd,
		    (unsigned long long)addr, (unsigned long long)data, numvec,
		    sc->psc_intx_configured, sc->psc_intx_enabled,
		    sc->psc_intx_irq, pi->pi_msi.enabled, pi->pi_msix.enabled,
		    sc->psc_msi_host_active);
	}
	if (rc == 0)
		sc->psc_msi_host_active = (numvec > 0);
	if (rc != 0 && numvec > 0 && saved_irq > 0 &&
	    sc->psc_intx_ioctl_supported && !passthru_intx_force_on_quirk(pi)) {
		if (passthru_setup_intx(ctx, sc->pptfd, saved_irq, 1) == 0) {
			sc->psc_intx_configured = 1;
			sc->psc_intx_enabled = 1;
			sc->psc_intx_irq = saved_irq;
		}
	}

	return (rc);
}

static int
passthru_vm_setup_pptdev_msix(struct passthru_softc *sc, struct pci_devinst *pi,
    struct vmctx *ctx, int idx, uint64_t addr, uint64_t data,
    uint32_t vector_control)
{
	int rc;
	int saved_irq = -1;

	if ((vector_control & PCIM_MSIX_VCTRL_MASK) == 0 &&
	    sc->psc_intx_ioctl_supported && sc->psc_intx_configured &&
	    sc->psc_intx_enabled && !passthru_intx_force_on_quirk(pi)) {
		saved_irq = sc->psc_intx_irq;
		if (saved_irq <= 0)
			saved_irq = pi->pi_lintr.ioapic_irq;
		rc = passthru_setup_intx(ctx, sc->pptfd, 0, 0);
		if (rc != 0) {
			if (errno == ENOTSUP)
				sc->psc_intx_ioctl_supported = 0;
			return (rc);
		}
		sc->psc_intx_configured = 1;
		sc->psc_intx_enabled = 0;
		sc->psc_intx_irq = 0;
	}

	rc = vm_setup_pptdev_msix(ctx, sc->pptfd, idx, addr, data,
	    vector_control);
	if (rc != 0 && (vector_control & PCIM_MSIX_VCTRL_MASK) == 0 &&
	    saved_irq > 0 && sc->psc_intx_ioctl_supported &&
	    !passthru_intx_force_on_quirk(pi)) {
		if (passthru_setup_intx(ctx, sc->pptfd, saved_irq, 1) == 0) {
			sc->psc_intx_configured = 1;
			sc->psc_intx_enabled = 1;
			sc->psc_intx_irq = saved_irq;
		}
	}

	return (rc);
}

static int
cfginitbar(struct vmctx *ctx __unused, struct passthru_softc *sc)
{
	struct pci_devinst *pi = sc->psc_pi;
	uint_t i;

	/*
	 * Initialize BAR registers
	 */
	for (i = 0; i <= PCI_BARMAX; i++) {
		enum pcibar_type bartype;
		uint64_t base, size;
		int error;

		if (passthru_get_bar(sc, i, &bartype, &base, &size) != 0) {
			continue;
		}

		if (bartype != PCIBAR_IO) {
			if (((base | size) & PAGE_MASK) != 0) {
				warnx("passthru device %d BAR %d: "
				    "base %#lx or size %#lx not page aligned\n",
				    sc->pptfd, i, base, size);
				return (-1);
			}
		}

		/* Cache information about the "real" BAR */
		sc->psc_bar[i].type = bartype;
		sc->psc_bar[i].size = size;
		sc->psc_bar[i].addr = base;
		sc->psc_bar[i].lobits = 0;

		/* Allocate the BAR in the guest I/O or MMIO space */
		error = pci_emul_alloc_bar(pi, i, bartype, size);
		if (error)
			return (-1);

		/* Use same lobits as physical bar */
		uint8_t lobits = passthru_read_config(sc, PCIR_BAR(i), 0x01);
		if (bartype == PCIBAR_MEM32 || bartype == PCIBAR_MEM64) {
			lobits &= ~PCIM_BAR_MEM_BASE;
		} else {
			lobits &= ~PCIM_BAR_IO_BASE;
		}
		sc->psc_bar[i].lobits = lobits;
		pi->pi_bar[i].lobits = lobits;

		/*
		 * 64-bit BAR takes up two slots so skip the next one.
		 */
		if (bartype == PCIBAR_MEM64) {
			i++;
			assert(i <= PCI_BARMAX);
			sc->psc_bar[i].type = PCIBAR_MEMHI64;
		}
	}
	return (0);
}

static int
cfginit(struct vmctx *ctx, struct passthru_softc *sc)
{
	int error;
	struct pci_devinst *pi = sc->psc_pi;
	uint16_t cmd;
	uint8_t intline, intpin;

	/*
	 * Copy physical PCI header to virtual config space.  COMMAND,
	 * INTLINE and INTPIN shouldn't be aligned with their physical value
	 * and they are already set by pci_emul_init().
	 */
	cmd = pci_get_cfgdata16(pi, PCIR_COMMAND);
	intline = pci_get_cfgdata8(pi, PCIR_INTLINE);
	intpin = pci_get_cfgdata8(pi, PCIR_INTPIN);
	for (int i = 0; i <= PCIR_MAXLAT; i += 4) {
#ifdef	__FreeBSD__
		pci_set_cfgdata32(pi, i, read_config(&sc->psc_sel, i, 4));
#else
		pci_set_cfgdata32(pi, i, passthru_read_config(sc, i, 4));
#endif
	}

	pci_set_cfgdata16(pi, PCIR_COMMAND, cmd);
	pci_set_cfgdata8(pi, PCIR_INTLINE, intline);
	pci_set_cfgdata8(pi, PCIR_INTPIN, intpin);

	if (cfginitmsi(sc) != 0) {
		warnx("failed to initialize MSI for PCI %d", sc->pptfd);
		return (-1);
	}

	if (cfginitbar(ctx, sc) != 0) {
		warnx("failed to initialize BARs for PCI %d", sc->pptfd);
		return (-1);
	}

	if (pci_msix_table_bar(pi) >= 0) {
		error = init_msix_table(ctx, sc);
		if (error != 0) {
			warnx("failed to initialize MSI-X table for PCI %d",
			    sc->pptfd);
			goto done;
		}
	}

	/* Emulate most PCI header register. */
	if ((error = set_pcir_handler(sc, 0, PCIR_MAXLAT + 1,
	    passthru_cfgread_emulate, passthru_cfgwrite_emulate)) != 0)
		goto done;

	/* Allow access to the physical status register. */
	if ((error = set_pcir_handler(sc, PCIR_COMMAND, 0x04, NULL, NULL)) != 0)
		goto done;

	error = 0;				/* success */
done:
	return (error);
}

int
set_pcir_handler(struct passthru_softc *sc, int reg, int len,
    cfgread_handler rhandler, cfgwrite_handler whandler)
{
	if (reg > PCI_REGMAX || reg + len > PCI_REGMAX + 1)
		return (-1);

	for (int i = reg; i < reg + len; ++i) {
		assert(sc->psc_pcir_rhandler[i] == NULL || rhandler == NULL);
		assert(sc->psc_pcir_whandler[i] == NULL || whandler == NULL);
		sc->psc_pcir_rhandler[i] = rhandler;
		sc->psc_pcir_whandler[i] = whandler;
	}

	return (0);
}

static int
passthru_legacy_config(nvlist_t *nvl, const char *opt)
{
	char *config, *name, *tofree, *value;

	if (opt == NULL)
		return (0);

	config = tofree = strdup(opt);
	while ((name = strsep(&config, ",")) != NULL) {
		value = strchr(name, '=');
		if (value != NULL) {
			*value++ = '\0';
			set_config_value_node(nvl, name, value);
		} else {
			if (strncmp(name, "/dev/ppt", 8) != 0) {
				EPRINTLN("passthru: invalid path \"%s\"", name);
				free(tofree);
				return (-1);
			}
			set_config_value_node(nvl, "path", name);
		}
	}
	free(tofree);
	return (0);
}

static uint16_t
passthru_rom_get_u16(const uint8_t *p)
{
	return ((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/*
 * Some dumped option ROMs claim that another image follows even though the
 * file ends after the current image. Clamp those single-image dumps to LAST in
 * the in-memory guest copy so firmware does not walk off the end of the ROM.
 */
static void
passthru_rom_fixup_chain(uint8_t *rom, size_t rom_file_size)
{
	size_t off = 0;
	int image = 0;

	while (off + 0x1a <= rom_file_size && image < 16) {
		uint16_t pcir;
		size_t hdr_off;
		uint16_t blocks;
		size_t next_off;
		uint8_t indicator;
		uint8_t *hdr;

		if (rom[off] != 0x55 || rom[off + 1] != 0xaa) {
			return;
		}

		pcir = passthru_rom_get_u16(&rom[off + 0x18]);
		hdr_off = off + pcir;
		if (pcir < 0x18 || hdr_off + 0x16 > rom_file_size) {
			return;
		}

		hdr = &rom[hdr_off];
		if (memcmp(hdr, "PCIR", 4) != 0) {
			return;
		}

		blocks = passthru_rom_get_u16(&hdr[0x10]);
		if (blocks == 0) {
			return;
		}

		indicator = hdr[0x15];
		next_off = off + (size_t)blocks * 512;
		if ((indicator & 0x80) != 0) {
			return;
		}

		if (next_off + 2 > rom_file_size ||
		    rom[next_off] != 0x55 || rom[next_off + 1] != 0xaa) {
			uint8_t sum = 0;
			size_t i;

			hdr[0x15] = indicator | 0x80;

			/*
			 * Keep the image checksum valid after forcing LAST so
			 * ROM consumers do not reject the repaired image.
			 */
			if (next_off <= rom_file_size && next_off > off) {
				for (i = off; i < next_off; i++) {
					sum = (uint8_t)(sum + rom[i]);
				}
				if (sum != 0) {
					rom[next_off - 1] =
					    (uint8_t)(rom[next_off - 1] - sum);
				}
			}
			return;
		}

		off = next_off;
		image++;
	}
}

static int
passthru_init_rom(struct vmctx *const ctx __unused,
    struct passthru_softc *const sc, const char *const romfile)
{
	if (romfile == NULL) {
		return (0);
	}

	const int fd = open(romfile, O_RDONLY);
	if (fd < 0) {
		warnx("%s: can't open romfile \"%s\"", __func__, romfile);
		return (-1);
	}

	struct stat sbuf;
	if (fstat(fd, &sbuf) < 0) {
		warnx("%s: can't fstat romfile \"%s\"", __func__, romfile);
		close(fd);
		return (-1);
	}
	const uint64_t rom_size = sbuf.st_size;

	void *const rom_data = mmap(NULL, rom_size, PROT_READ, MAP_SHARED, fd,
	    0);
	if (rom_data == MAP_FAILED) {
		warnx("%s: unable to mmap romfile \"%s\" (%d)", __func__,
		    romfile, errno);
		close(fd);
		return (-1);
	}

	void *rom_addr;
	int error = pci_emul_alloc_rom(sc->psc_pi, rom_size, &rom_addr);
	if (error) {
		warnx("%s: failed to alloc rom segment", __func__);
		munmap(rom_data, rom_size);
		close(fd);
		return (error);
	}
	memcpy(rom_addr, rom_data, rom_size);
	passthru_rom_fixup_chain(rom_addr, rom_size);

	sc->psc_bar[PCI_ROM_IDX].type = PCIBAR_ROM;
	sc->psc_bar[PCI_ROM_IDX].addr = (uint64_t)rom_addr;
	sc->psc_bar[PCI_ROM_IDX].size = rom_size;

	munmap(rom_data, rom_size);
	close(fd);

 	return (0);
 }

static int
passthru_init(struct pci_devinst *pi, nvlist_t *nvl)
{
	int error, memflags, pptfd;
	struct passthru_softc *sc;
	const char *path;
	struct vmctx *ctx = pi->pi_vmctx;

	pptfd = -1;
	sc = NULL;
	error = 1;

	memflags = vm_get_memflags(ctx);
	if (!(memflags & VM_MEM_F_WIRED)) {
		warnx("passthru requires guest memory to be wired");
		goto done;
	}

	path = get_config_value_node(nvl, "path");
	if (path == NULL || passthru_dev_open(path, &pptfd) != 0) {
		warnx("invalid passthru options");
		goto done;
	}

	if (vm_assign_pptdev(ctx, pptfd) != 0) {
		warnx("PCI device at %d is not using the ppt driver", pptfd);
		goto done;
	}

	sc = calloc(1, sizeof(struct passthru_softc));

	pi->pi_arg = sc;
	sc->psc_pi = pi;
	sc->pptfd = pptfd;
	sc->psc_intx_ioctl_supported = 1;
	(void) pthread_mutex_init(&sc->psc_alias_lock, NULL);

	if ((error = vm_get_pptdev_limits(ctx, pptfd, &sc->msi_limit,
	    &sc->msix_limit)) != 0)
		goto done;

#ifndef	__FreeBSD__
	/*
	 * If this function uses legacy interrupt messages, then request one for
	 * the guest in case drivers expect to see it. Note that nothing in the
	 * hypervisor is currently wired up do deliver such an interrupt should
	 * the guest actually rely upon it.
	 */
	uint8_t intpin = passthru_read_config(sc, PCIR_INTPIN, 1);
	if (intpin > 0 && intpin < 5)
		pci_lintr_request(sc->psc_pi);
#endif

	/* initialize config space */
	if ((error = cfginit(ctx, sc)) != 0)
		goto done;

	/* initialize ROM */
	if ((error = passthru_init_rom(ctx, sc,
	    get_config_value_node(nvl, "rom"))) != 0) {
		goto done;
	}
	if (passthru_nvidia_display_fn0(sc)) {
		passthru_tu102_active_sc = sc;
	}

done:
	if (error) {
		if (sc != NULL) {
			(void) pthread_mutex_destroy(&sc->psc_alias_lock);
		}
		free(sc);
		if (pptfd != -1)
			vm_unassign_pptdev(ctx, pptfd);
	}
	return (error);
}

static int
msicap_access(struct passthru_softc *sc, int coff)
{
	int caplen;

	if (sc->psc_msi.capoff == 0)
		return (0);

	caplen = msi_caplen(sc->psc_msi.msgctrl);

	if (coff >= sc->psc_msi.capoff && coff < sc->psc_msi.capoff + caplen)
		return (1);
	else
		return (0);
}

static int
msixcap_access(struct passthru_softc *sc, int coff)
{
	if (sc->psc_msix.capoff == 0)
		return (0);

	return (coff >= sc->psc_msix.capoff &&
	        coff < sc->psc_msix.capoff + MSIX_CAPLEN);
}

static int
passthru_cfgread_default(struct passthru_softc *sc,
    struct pci_devinst *pi __unused, int coff, int bytes, uint32_t *rv)
{
	/*
	 * MSI capability is emulated.
	 */
	if (msicap_access(sc, coff) || msixcap_access(sc, coff))
		return (PE_CFGRW_DEFAULT);

	/*
	 * MSI-X is also emulated since a limit on interrupts may be imposed by
	 * the OS, altering the perceived register state.
	 */
	if (msixcap_access(sc, coff))
		return (PE_CFGRW_DEFAULT);

	/*
	 * Emulate the command register.  If a single read reads both the
	 * command and status registers, read the status register from the
	 * device's config space.
	 */
	if (coff == PCIR_COMMAND) {
		if (bytes <= 2)
			return (PE_CFGRW_DEFAULT);
		*rv = passthru_read_config(sc, PCIR_STATUS, 2) << 16 |
		    pci_get_cfgdata16(pi, PCIR_COMMAND);
		return (PE_CFGRW_DROP);
	}

	/* Everything else just read from the device's config space */
	*rv = passthru_read_config(sc, coff, bytes);

	return (PE_CFGRW_DROP);
}

int
passthru_cfgread_emulate(struct passthru_softc *sc __unused,
    struct pci_devinst *pi __unused, int coff __unused, int bytes __unused,
    uint32_t *rv __unused)
{
	return (PE_CFGRW_DEFAULT);
}

static int
passthru_cfgread(struct pci_devinst *pi, int coff, int bytes, uint32_t *rv)
{
	struct passthru_softc *sc;

	sc = pi->pi_arg;

	if (sc->psc_pcir_rhandler[coff] != NULL)
		return (sc->psc_pcir_rhandler[coff](sc, pi, coff, bytes, rv));

	return (passthru_cfgread_default(sc, pi, coff, bytes, rv));
}

static int
passthru_cfgwrite_default(struct passthru_softc *sc, struct pci_devinst *pi,
    int coff, int bytes, uint32_t val)
{
	int error, msix_table_entries, i;
	uint16_t cmd_old;
	struct vmctx *ctx = pi->pi_vmctx;

	/*
	 * MSI capability is emulated
	 */
	if (msicap_access(sc, coff)) {
		pci_emul_capwrite(pi, coff, bytes, val, sc->psc_msi.capoff,
		    PCIY_MSI);
		error = passthru_vm_setup_pptdev_msi(sc, pi, ctx,
		    pi->pi_msi.addr, pi->pi_msi.msg_data, pi->pi_msi.maxmsgnum);
		if (error != 0)
			err(1, "vm_setup_pptdev_msi");
		return (PE_CFGRW_DROP);
	}

	if (msixcap_access(sc, coff)) {
		pci_emul_capwrite(pi, coff, bytes, val, sc->psc_msix.capoff,
		    PCIY_MSIX);
		if (pi->pi_msix.enabled) {
			msix_table_entries = pi->pi_msix.table_count;
			for (i = 0; i < msix_table_entries; i++) {
				error = passthru_vm_setup_pptdev_msix(sc, pi, ctx,
				    i,
				    pi->pi_msix.table[i].addr,
				    pi->pi_msix.table[i].msg_data,
				    pi->pi_msix.table[i].vector_control);

				if (error)
					err(1, "vm_setup_pptdev_msix");
			}
		} else {
			error = vm_disable_pptdev_msix(ctx, sc->pptfd);
			if (error)
				err(1, "vm_disable_pptdev_msix");
		}
		return (PE_CFGRW_DROP);
	}

	/*
	 * The command register is emulated, but the status register
	 * is passed through.
	 */
	if (coff == PCIR_COMMAND) {
		uint16_t reqval;
		uint16_t newval;
		uint16_t host_cur;
		uint16_t host_write;
		int need_host_sync;
		int sticky_host_cmd;
		int shadow_host_cmd;

		if (bytes <= 2)
			return (PE_CFGRW_DEFAULT);

		reqval = val & 0xffff;
		newval = reqval;
		host_cur = 0;
		host_write = newval;
		need_host_sync = 1;
		sticky_host_cmd = passthru_tu102_host_cmd_sticky_quirk(pi);
		shadow_host_cmd = passthru_tu102_host_cmd_shadow_quirk(pi);

		/* Update the physical status register. */
		passthru_write_config(sc, PCIR_STATUS, 2, val >> 16);

		/* Update the virtual command register. */
		cmd_old = pci_get_cfgdata16(pi, PCIR_COMMAND);
		pci_set_cfgdata16(pi, PCIR_COMMAND, newval);
		pci_emul_cmd_changed(pi, cmd_old);

		if (shadow_host_cmd) {
			host_cur = passthru_read_config(sc, PCIR_COMMAND,
			    sizeof (uint16_t));
			host_write = host_cur;
			need_host_sync = 0;
		} else if (sticky_host_cmd) {
			host_cur = passthru_read_config(sc, PCIR_COMMAND,
			    sizeof (uint16_t));
			host_write |= host_cur & (PCIM_CMD_PORTEN |
			    PCIM_CMD_MEMEN | PCIM_CMD_BUSMASTEREN);
			if (host_write == host_cur)
				need_host_sync = 0;
		}

		if (need_host_sync)
			passthru_write_config(sc, PCIR_COMMAND, 2, host_write);

		if (sc->psc_intx_ioctl_supported &&
		    pi->pi_lintr.ioapic_irq > 0 &&
		    (passthru_env_enabled("BHYVE_PPT_ENABLE_INTX") ||
		    passthru_intx_bootstrap_quirk(pi) ||
		    passthru_intx_force_on_quirk(pi))) {
			int intx_enable;
			int need_cfg;

			intx_enable = ((passthru_intx_force_on_quirk(pi) ||
			    (!pi->pi_msi.enabled && !pi->pi_msix.enabled &&
			    ((newval & PCIM_CMD_INTxDIS) == 0)))) ? 1 : 0;
			need_cfg = (!sc->psc_intx_configured ||
			    sc->psc_intx_enabled != intx_enable ||
			    (intx_enable != 0 &&
			    sc->psc_intx_irq != pi->pi_lintr.ioapic_irq));

			if (need_cfg) {
				error = passthru_setup_intx(ctx, sc->pptfd,
				    intx_enable ? pi->pi_lintr.ioapic_irq : 0,
				    intx_enable);
				if (error == 0) {
					sc->psc_intx_configured = 1;
					sc->psc_intx_enabled = intx_enable;
					sc->psc_intx_irq = intx_enable ?
					    pi->pi_lintr.ioapic_irq : 0;
				} else if (errno == ENOTSUP) {
					sc->psc_intx_ioctl_supported = 0;
					sc->psc_intx_configured = 1;
					sc->psc_intx_enabled = 0;
					sc->psc_intx_irq = 0;
				}
			}
		}

		return (PE_CFGRW_DROP);
	}

	passthru_write_config(sc, coff, bytes, val);

	return (PE_CFGRW_DROP);
}

int
passthru_cfgwrite_emulate(struct passthru_softc *sc __unused,
    struct pci_devinst *pi __unused, int coff __unused, int bytes __unused,
    uint32_t val __unused)
{
	return (PE_CFGRW_DEFAULT);
}

static int
passthru_cfgwrite(struct pci_devinst *pi, int coff, int bytes, uint32_t val)
{
	struct passthru_softc *sc;

	sc = pi->pi_arg;

	if (sc->psc_pcir_whandler[coff] != NULL)
		return (sc->psc_pcir_whandler[coff](sc, pi, coff, bytes, val));

	return (passthru_cfgwrite_default(sc, pi, coff, bytes, val));
}

static void
passthru_write(struct pci_devinst *pi, int baridx, uint64_t offset, int size,
    uint64_t value)
{
	struct passthru_softc *sc = pi->pi_arg;
	struct vmctx *ctx = pi->pi_vmctx;

	if (baridx == pci_msix_table_bar(pi)) {
		msix_table_write(ctx, sc, offset, size, value);
	} else {
		struct ppt_bar_io pbi;

		assert(pi->pi_bar[baridx].type == PCIBAR_IO);

		pbi.pbi_bar = baridx;
		pbi.pbi_width = size;
		pbi.pbi_off = offset;
		pbi.pbi_data = value;
		(void) ioctl(sc->pptfd, PPT_BAR_WRITE, &pbi);
	}
}

static uint64_t
passthru_read(struct pci_devinst *pi, int baridx, uint64_t offset, int size)
{
	struct passthru_softc *sc = pi->pi_arg;
	uint64_t val;

	if (baridx == pci_msix_table_bar(pi)) {
		val = msix_table_read(sc, offset, size);
	} else {
		struct ppt_bar_io pbi;

		assert(pi->pi_bar[baridx].type == PCIBAR_IO);

		pbi.pbi_bar = baridx;
		pbi.pbi_width = size;
		pbi.pbi_off = offset;
		if (ioctl(sc->pptfd, PPT_BAR_READ, &pbi) == 0) {
			val = pbi.pbi_data;
		} else {
			val = 0;
		}
	}

	return (val);
}

static void
passthru_msix_addr(struct vmctx *ctx, struct pci_devinst *pi, int baridx,
		   int enabled, uint64_t address)
{
	struct passthru_softc *sc;
	size_t remaining;
	uint32_t table_size, table_offset;

	sc = pi->pi_arg;

	if (enabled && address != 0) {
		struct ppt_iommu_map map = {
			.gpa = address,
			.hpa = sc->psc_bar[baridx].addr,
			.size = sc->psc_bar[baridx].size,
			.prot = IOMMU_PROT_RW
		};

		if (ioctl(sc->pptfd, PPT_IOMMU_MAP, &map) != 0) {
			warn("PASSTHRU: MSIX PPT_IOMMU_MAP BAR%d FAILED "
			    "gpa=0x%llx hpa=0x%llx sz=0x%llx", baridx,
			    (unsigned long long)map.gpa,
			    (unsigned long long)map.hpa,
			    (unsigned long long)map.size);
		}
	}

	table_offset = rounddown2(pi->pi_msix.table_offset, 4096);
	if (table_offset > 0) {
		if (!enabled) {
			if (vm_unmap_pptdev_mmio(ctx, sc->pptfd, address,
			    table_offset) != 0)
				warnx("pci_passthru: unmap_pptdev_mmio failed");
		} else {
			if (vm_map_pptdev_mmio(ctx, sc->pptfd, address,
			    table_offset, sc->psc_bar[baridx].addr) != 0)
				warnx("pci_passthru: map_pptdev_mmio failed");
		}
	}
	table_size = pi->pi_msix.table_offset - table_offset;
	table_size += pi->pi_msix.table_count * MSIX_TABLE_ENTRY_SIZE;
	table_size = roundup2(table_size, 4096);
	remaining = pi->pi_bar[baridx].size - table_offset - table_size;
	if (remaining > 0) {
		address += table_offset + table_size;
		if (!enabled) {
			if (vm_unmap_pptdev_mmio(ctx, sc->pptfd, address,
			    remaining) != 0)
				warnx("pci_passthru: unmap_pptdev_mmio failed");
		} else {
			if (vm_map_pptdev_mmio(ctx, sc->pptfd, address,
			    remaining, sc->psc_bar[baridx].addr +
			    table_offset + table_size) != 0)
				warnx("pci_passthru: map_pptdev_mmio failed");
		}
	}
}

static int
passthru_mmio_addr(struct vmctx *ctx, struct pci_devinst *pi, int baridx,
		   int enabled, uint64_t address)
{
	struct passthru_softc *sc;

	sc = pi->pi_arg;
	if (passthru_nvidia_display_fn0(sc)) {
		passthru_tu102_trace("passthru: TU102 mmio %s bar=%d gpa=0x%lx len=0x%lx hpa=0x%lx",
		    enabled ? "map" : "unmap", baridx, (ulong_t)address,
		    (ulong_t)sc->psc_bar[baridx].size,
		    (ulong_t)sc->psc_bar[baridx].addr);
	}
	if (!enabled) {
		if (passthru_nvidia_display_fn0(sc)) {
			(void) pthread_mutex_lock(&sc->psc_alias_lock);
			passthru_tu102_drop_alias_locked(sc, baridx);
			if (baridx == 1) {
				passthru_tu102_drop_bar1_postbar3_window_locked(sc);
			}
			(void) pthread_mutex_unlock(&sc->psc_alias_lock);
		}
		if (!sc->psc_bar_mapped[baridx])
			return (1);
		if (vm_unmap_pptdev_mmio(ctx, sc->pptfd, address,
		    sc->psc_bar[baridx].size) != 0)
			warnx("pci_passthru: unmap_pptdev_mmio failed");
		sc->psc_bar_mapped[baridx] = 0;
	} else {
		if (vm_map_pptdev_mmio(ctx, sc->pptfd, address,
		    sc->psc_bar[baridx].size, sc->psc_bar[baridx].addr) != 0)
			warnx("pci_passthru: map_pptdev_mmio failed");
		else
			sc->psc_bar_mapped[baridx] = 1;
		if (passthru_nvidia_display_fn0(sc) && sc->psc_bar_mapped[baridx] &&
		    baridx == 1) {
			(void) pthread_mutex_lock(&sc->psc_alias_lock);
			(void) passthru_tu102_touch_bar1_postbar3_window_locked(sc,
			    passthru_tu102_bar1_postbar3_base(sc), 0, 0, 0, 0);
			(void) pthread_mutex_unlock(&sc->psc_alias_lock);
		}
	}

	return (!enabled || sc->psc_bar_mapped[baridx] != 0);
}

int
passthru_tu102_mmio_fault(struct vmctx *ctx, struct vm_mmio *mmio)
{
	struct passthru_softc *sc = passthru_tu102_active_sc;
	int baridx;
	int window_slot = -1;
	uint64_t offset;
	uint64_t window_gpa = 0;
	int handled = 0;
	int err = 0;
	bool bar3_tail = false;

	if (sc == NULL || sc->psc_pi == NULL || sc->psc_pi->pi_vmctx != ctx ||
	    !passthru_nvidia_display_fn0(sc) || mmio == NULL) {
		return (0);
	}
	(void) pthread_mutex_lock(&sc->psc_alias_lock);
	if (passthru_tu102_bar3_tail_alias_decode(sc, mmio->gpa, &offset)) {
		baridx = 3;
		bar3_tail = true;
	} else if (passthru_tu102_alias_decode(sc, mmio->gpa, &baridx,
	    &offset)) {
		if (baridx == 1) {
			window_gpa = mmio->gpa - offset;
		}
		if (baridx != 1 || mmio->gpa == PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA +
		    offset) {
			(void) passthru_tu102_ensure_alias_locked(sc, baridx);
		}
	} else if (passthru_tu102_bar1_postbar3_decode(sc, mmio->gpa,
	    &window_gpa, &offset)) {
		baridx = 1;
		passthru_trace_tu102_bar1_transition(sc, mmio->gpa);
		(void) passthru_tu102_touch_bar1_postbar3_window_locked(sc,
		    window_gpa, mmio->gpa, mmio->read, mmio->bytes, mmio->data);
	} else {
		(void) pthread_mutex_unlock(&sc->psc_alias_lock);
		passthru_trace_tu102_bar1_transition(sc, mmio->gpa);
		return (0);
	}
	if (baridx == 1 && window_gpa != 0 &&
	    window_gpa != PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA) {
		window_slot = passthru_tu102_bar1_window_slot_locked(sc, window_gpa);
	}

	if (mmio->read != 0) {
		uint64_t val = 0;

		if (passthru_host_bar_read(sc, baridx, offset, mmio->bytes,
		    &val) == 0) {
			mmio->data = val;
			handled = 1;
		} else {
			err = errno;
		}
	} else {
		if (passthru_host_bar_write(sc, baridx, offset, mmio->bytes,
		    mmio->data) == 0) {
			handled = 1;
		} else {
			err = errno;
		}
	}
	if (handled && bar3_tail) {
		passthru_trace_tu102_bar3_tail_mmio(sc, mmio->read != 0 ? "read" :
		    "write", mmio->gpa, offset, mmio->bytes, mmio->data);
	} else if (handled && baridx == 1) {
		if (sc->psc_bar1_alias_first_seen == 0 &&
		    window_gpa == PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA) {
			sc->psc_bar1_alias_first_seen = 1;
			passthru_tu102_trace("passthru: TU102 BAR1 alias first-hit "
			    "fault_gpa=0x%lx alias_base=0x%lx native_bar1_gpa=0x%lx",
			    (ulong_t)mmio->gpa,
			    (ulong_t)PASSTHRU_TU102_BAR1_FW_ALIAS0_GPA,
			    (ulong_t)(sc->psc_bar_gpa_valid[1] != 0 ?
			    sc->psc_bar_gpa[1] : sc->psc_pi->pi_bar[1].addr));
		}
		passthru_trace_tu102_bar1_mmio(sc, mmio->read != 0 ? "read" :
		    "write", mmio->gpa, window_gpa, mmio->bytes, mmio->data,
		    window_slot);
		if (window_slot >= 0 &&
		    sc->psc_bar1_postbar3_cache[window_slot].trap_only != 0 &&
		    offset + mmio->bytes >= 0x1000 &&
		    passthru_tu102_map_bar1_window_page_locked(sc, window_slot,
		    window_gpa)) {
			passthru_tu102_trace("passthru: TU102 postbar3-window page-map slot=%d "
			    "gpa=0x%lx after_off=0x%lx", window_slot,
			    (ulong_t)window_gpa, (ulong_t)offset);
		}
	}
	(void) pthread_mutex_unlock(&sc->psc_alias_lock);
	if (!handled && err != 0) {
		warnx("pci_passthru: TU102 alias mmio %s failed bar=%d "
		    "gpa=0x%lx off=0x%lx size=%u errno=%d",
		    mmio->read != 0 ? "read" : "write", baridx,
		    (ulong_t)mmio->gpa, (ulong_t)offset, mmio->bytes, err);
	}
	return (handled);
}

static void
passthru_addr_rom(struct pci_devinst *const pi, const int idx,
    const int enabled)
{
	const uint64_t addr = pi->pi_bar[idx].addr;
	const uint64_t size = pi->pi_bar[idx].size;

	if (!enabled) {
		if (vm_munmap_memseg(pi->pi_vmctx, addr, size) != 0) {
			errx(4, "%s: munmap_memseg @ [%016lx - %016lx] failed",
			    __func__, addr, addr + size);
		}

	} else {
		if (vm_mmap_memseg(pi->pi_vmctx, addr, VM_PCIROM,
			pi->pi_romoffset, size, PROT_READ | PROT_EXEC) != 0) {
			errx(4, "%s: mmap_memseg @ [%016lx - %016lx]  failed",
			    __func__, addr, addr + size);
		}
	}
}

static int
passthru_addr_one(struct pci_devinst *pi, int baridx, int enabled,
    uint64_t address)
{
	struct vmctx *ctx = pi->pi_vmctx;

	switch (pi->pi_bar[baridx].type) {
	case PCIBAR_IO:
		/* IO BARs are emulated */
		return (1);
	case PCIBAR_ROM:
		passthru_addr_rom(pi, baridx, enabled);
		return (1);
	case PCIBAR_MEM32:
	case PCIBAR_MEM64:
		if (baridx == pci_msix_table_bar(pi))
			passthru_msix_addr(ctx, pi, baridx, enabled, address);
		else
			return (passthru_mmio_addr(ctx, pi, baridx, enabled,
			    address));
		return (1);
	default:
		errx(4, "%s: invalid BAR type %d", __func__,
		    pi->pi_bar[baridx].type);
	}

	return (0);
}

static void
passthru_retry_pending_bars(struct pci_devinst *pi)
{
	struct passthru_softc *sc = pi->pi_arg;
	int baridx;
	int progress;

	do {
		progress = 0;
		for (baridx = 0; baridx <= PCI_BARMAX; baridx++) {
			if (pi->pi_bar[baridx].type != PCIBAR_MEM32 &&
			    pi->pi_bar[baridx].type != PCIBAR_MEM64)
				continue;
			if (baridx == pci_msix_table_bar(pi))
				continue;
			if (sc->psc_bar_gpa_valid[baridx] != 0 ||
			    pi->pi_bar[baridx].addr == 0)
				continue;
			if (passthru_addr_one(pi, baridx, 1,
			    pi->pi_bar[baridx].addr)) {
				sc->psc_bar_gpa[baridx] = pi->pi_bar[baridx].addr;
				sc->psc_bar_gpa_valid[baridx] = 1;
				progress = 1;
			}
		}
	} while (progress != 0);
}

static void
passthru_addr(struct pci_devinst *pi, int baridx,
    int enabled, uint64_t address)
{
	struct passthru_softc *sc = pi->pi_arg;
	uint64_t oldaddr;
	int success;
	uint64_t req_address = address;
	uint32_t cfg_lo;
	uint32_t cfg_hi;

	if (enabled && passthru_nvidia_display_fn0(sc)) {
		uint64_t pinned;

		switch (baridx) {
		case 0:
			pinned = 0xc0000000ULL;
			address = pinned;
			pi->pi_bar[0].addr = pinned;
			pci_set_cfgdata32(pi, PCIR_BAR(0),
			    (uint32_t)pinned | pi->pi_bar[0].lobits);
			break;
		case 1:
			pinned = 0x800000000ULL;
			address = pinned;
			pi->pi_bar[1].addr = pinned;
			pci_set_cfgdata32(pi, PCIR_BAR(1),
			    (uint32_t)pinned | pi->pi_bar[1].lobits);
			pci_set_cfgdata32(pi, PCIR_BAR(2), pinned >> 32);
			break;
		case 3:
			pinned = 0x810000000ULL;
			address = pinned;
			pi->pi_bar[3].addr = pinned;
			pci_set_cfgdata32(pi, PCIR_BAR(3),
			    (uint32_t)pinned | pi->pi_bar[3].lobits);
			pci_set_cfgdata32(pi, PCIR_BAR(4), pinned >> 32);
			break;
		default:
			break;
		}
	}

	if (passthru_nvidia_display_fn0(sc)) {
		cfg_lo = pci_get_cfgdata32(pi, PCIR_BAR(baridx));
		cfg_hi = (pi->pi_bar[baridx].type == PCIBAR_MEM64 &&
		    baridx + 1 <= PCI_BARMAX) ?
		    pci_get_cfgdata32(pi, PCIR_BAR(baridx + 1)) : 0;
		passthru_tu102_trace("passthru: TU102 baraddr enabled=%d bar=%d req_gpa=0x%lx gpa=0x%lx size=0x%lx",
		    enabled, baridx, (ulong_t)req_address, (ulong_t)address,
		    (ulong_t)sc->psc_bar[baridx].size);
		passthru_tu102_trace("passthru: TU102 barcfg bar=%d type=%d cfg_lo=0x%08x cfg_hi=0x%08x pi_addr=0x%lx valid=%u mapped=%u alias=%u",
		    baridx, pi->pi_bar[baridx].type, cfg_lo, cfg_hi,
		    (ulong_t)pi->pi_bar[baridx].addr,
		    sc->psc_bar_gpa_valid[baridx], sc->psc_bar_mapped[baridx],
		    sc->psc_bar_fw_alias_mapped[baridx]);
	}

	if (enabled && sc->psc_bar_gpa_valid[baridx] &&
	    sc->psc_bar_gpa[baridx] == address && sc->psc_bar_mapped[baridx]) {
		passthru_retry_pending_bars(pi);
		return;
	}

	if (enabled && sc->psc_bar_gpa_valid[baridx] &&
	    sc->psc_bar_gpa[baridx] != address) {
		oldaddr = sc->psc_bar_gpa[baridx];
		(void) passthru_addr_one(pi, baridx, 0, oldaddr);
		sc->psc_bar_gpa_valid[baridx] = 0;
	}

	success = passthru_addr_one(pi, baridx, enabled, address);

	if (enabled && success) {
		sc->psc_bar_gpa[baridx] = address;
		sc->psc_bar_gpa_valid[baridx] = 1;
		passthru_retry_pending_bars(pi);
	} else if (!enabled) {
		sc->psc_bar_gpa_valid[baridx] = 0;
		sc->psc_bar_mapped[baridx] = 0;
	}
}

static const struct pci_devemu passthru = {
	.pe_emu		= "passthru",
	.pe_init	= passthru_init,
	.pe_legacy_config = passthru_legacy_config,
	.pe_cfgwrite	= passthru_cfgwrite,
	.pe_cfgread	= passthru_cfgread,
	.pe_barwrite 	= passthru_write,
	.pe_barread    	= passthru_read,
	.pe_baraddr	= passthru_addr,
};
PCI_EMUL_SET(passthru);

/*
 * This isn't the right place for these functions which, on FreeBSD, can
 * read or write from arbitrary devices. They are not supported on illumos;
 * not least because bhyve is generally run in a non-global zone which doesn't
 * have access to the devinfo tree.
 */
uint32_t
pci_host_read_config(const struct pcisel *sel __unused, long reg __unused,
    int width __unused)
{
	return (-1);
}

void
pci_host_write_config(const struct pcisel *sel __unused, long reg __unused,
    int width __unused, uint32_t data __unused)
{
       errx(4, "pci_host_write_config() unimplemented on illumos");
}
