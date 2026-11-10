/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Jonathan Wagner.
 */

/*
 * Zone-local Linux uinput and evdev support for LX.
 *
 * The driver deliberately does not inject events into native illumos input
 * devices.  A uinput producer and all evdev consumers must belong to the same
 * LX zone.  Fixed event nodes avoid host devfs churn; inactive nodes fail open
 * with ENODEV.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/conf.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/filio.h>
#include <sys/poll.h>
#include <sys/vmem.h>
#include <sys/list.h>
#include <sys/model.h>
#include <sys/time.h>
#include <sys/proc.h>
#include <sys/zone.h>
#include <sys/brand.h>
#include <sys/lx_brand.h>
#include <sys/lx_input.h>
#include <sys/lx_netlink.h>
#include <sys/modctl.h>

#define	LX_INPUT_QUEUE_SIZE	256
#define	LX_INPUT_MAX_COPY	(1U << LX_IOC_SIZEBITS)

typedef struct lx_input_event {
	int64_t lie_sec;
	int64_t lie_usec;
	uint16_t lie_type;
	uint16_t lie_code;
	int32_t lie_value;
} lx_input_event_t;

struct lx_input_device;

typedef struct lx_input_client {
	list_node_t lic_link;
	struct lx_input_device *lic_device;
	kcondvar_t lic_cv;
	pollhead_t lic_pollhead;
	clockid_t lic_clock;
	boolean_t lic_grabbed;
	uint_t lic_head;
	uint_t lic_count;
	lx_input_event_t lic_queue[LX_INPUT_QUEUE_SIZE];
} lx_input_client_t;

typedef struct lx_input_device {
	list_node_t lid_link;
	kmutex_t lid_lock;
	list_t lid_clients;
	lx_input_config_t lid_config;
	zoneid_t lid_zoneid;
	uint_t lid_slot;
	uint_t lid_refs;
	boolean_t lid_dead;
	lx_input_client_t *lid_grabber;
	uint8_t lid_keystate[LX_INPUT_BITS(LX_KEY_MAX)];
	uint8_t lid_swstate[LX_INPUT_BITS(LX_SW_MAX)];
	uint8_t lid_ledstate[LX_INPUT_BITS(LX_LED_MAX)];
} lx_input_device_t;

typedef enum lx_input_handle_type {
	LX_IHT_UINPUT,
	LX_IHT_EVENT
} lx_input_handle_type_t;

typedef struct lx_input_handle {
	kmutex_t lih_lock;
	lx_input_handle_type_t lih_type;
	zoneid_t lih_zoneid;
	union {
		struct {
			lx_input_config_t lihu_config;
			lx_input_device_t *lihu_device;
		} lihu_uinput;
		struct {
			lx_input_client_t *lihe_client;
		} lihu_event;
	} lih_u;
} lx_input_handle_t;

#define	lih_config	lih_u.lihu_uinput.lihu_config
#define	lih_device	lih_u.lihu_uinput.lihu_device
#define	lih_client	lih_u.lihu_event.lihe_client

static dev_info_t *lx_input_devi;
static void *lx_input_softstate;
static vmem_t *lx_input_minor_ids;
static kmutex_t lx_input_lock;
static list_t lx_input_devices;

static void
lx_input_setbit(uint8_t *bits, uint_t bit)
{
	bits[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

static boolean_t
lx_input_testbit(const uint8_t *bits, uint_t bit)
{
	return ((bits[bit >> 3] & (1U << (bit & 7))) != 0);
}

static int
lx_input_config_bits(lx_input_config_t *config, uint_t type, uint8_t **bitsp,
    uint_t *maxp)
{
	switch (type) {
	case LX_EV_SYN:
		*bitsp = NULL;
		*maxp = LX_SYN_DROPPED;
		return (0);
	case LX_EV_KEY:
		*bitsp = config->lic_keybits;
		*maxp = LX_KEY_MAX;
		return (0);
	case LX_EV_REL:
		*bitsp = config->lic_relbits;
		*maxp = LX_REL_MAX;
		return (0);
	case LX_EV_ABS:
		*bitsp = config->lic_absbits;
		*maxp = LX_ABS_MAX;
		return (0);
	case LX_EV_MSC:
		*bitsp = config->lic_mscbits;
		*maxp = LX_MSC_MAX;
		return (0);
	case LX_EV_SW:
		*bitsp = config->lic_swbits;
		*maxp = LX_SW_MAX;
		return (0);
	case LX_EV_LED:
		*bitsp = config->lic_ledbits;
		*maxp = LX_LED_MAX;
		return (0);
	case LX_EV_SND:
		*bitsp = config->lic_sndbits;
		*maxp = LX_SND_MAX;
		return (0);
	case LX_EV_FF:
		*bitsp = config->lic_ffbits;
		*maxp = LX_FF_MAX;
		return (0);
	default:
		return (EINVAL);
	}
}

static lx_input_device_t *
lx_input_device_lookup(zoneid_t zoneid, uint_t slot)
{
	lx_input_device_t *device;

	ASSERT(MUTEX_HELD(&lx_input_lock));

	for (device = list_head(&lx_input_devices); device != NULL;
	    device = list_next(&lx_input_devices, device)) {
		if (device->lid_zoneid == zoneid && device->lid_slot == slot)
			return (device);
	}

	return (NULL);
}

boolean_t
lx_input_get_config(zoneid_t zoneid, uint_t slot, lx_input_config_t *config)
{
	lx_input_device_t *device;

	if (slot >= LX_INPUT_EVENT_COUNT)
		return (B_FALSE);

	mutex_enter(&lx_input_lock);
	device = lx_input_device_lookup(zoneid, slot);
	if (device == NULL) {
		mutex_exit(&lx_input_lock);
		return (B_FALSE);
	}
	mutex_enter(&device->lid_lock);
	if (config != NULL)
		*config = device->lid_config;
	mutex_exit(&device->lid_lock);
	mutex_exit(&lx_input_lock);
	return (B_TRUE);
}

static boolean_t
lx_input_uevent_action_valid(const char *action)
{
	static const char *valid[] = {
		"add", "remove", "change", "move", "online", "offline",
		"bind", "unbind"
	};
	uint_t i;

	for (i = 0; i < sizeof (valid) / sizeof (valid[0]); i++) {
		if (strcmp(action, valid[i]) == 0)
			return (B_TRUE);
	}
	return (B_FALSE);
}

int
lx_input_trigger_uevent(zoneid_t zoneid, uint_t slot, boolean_t parent,
    const char *action)
{
	if (!lx_input_uevent_action_valid(action))
		return (EINVAL);
	if (!lx_input_get_config(zoneid, slot, NULL))
		return (ENODEV);
	if (parent)
		lx_netlink_input_parent_uevent(zoneid, action, slot);
	else
		lx_netlink_input_uevent(zoneid, action, slot);
	return (0);
}

int
lx_input_trigger_uinput_uevent(zoneid_t zoneid, const char *action)
{
	if (!lx_input_uevent_action_valid(action))
		return (EINVAL);
	lx_netlink_uinput_uevent(zoneid, action);
	return (0);
}

static void
lx_input_device_free(lx_input_device_t *device)
{
	ASSERT(device->lid_dead);
	ASSERT(device->lid_refs == 0);
	ASSERT(list_is_empty(&device->lid_clients));

	list_destroy(&device->lid_clients);
	mutex_destroy(&device->lid_lock);
	kmem_free(device, sizeof (*device));
}

static boolean_t
lx_input_device_rele_locked(lx_input_device_t *device)
{
	ASSERT(MUTEX_HELD(&device->lid_lock));
	ASSERT(device->lid_refs != 0);

	device->lid_refs--;
	return (device->lid_refs == 0);
}

static void
lx_input_client_wakeup(lx_input_client_t *client, short events)
{
	cv_broadcast(&client->lic_cv);
	pollwakeup(&client->lic_pollhead, events);
}

static void
lx_input_device_destroy(lx_input_handle_t *handle)
{
	lx_input_client_t *client;
	lx_input_device_t *device = handle->lih_device;
	boolean_t free_device;
	zoneid_t zoneid;
	uint_t slot;

	if (device == NULL)
		return;

	mutex_enter(&lx_input_lock);
	mutex_enter(&device->lid_lock);

	ASSERT(!device->lid_dead);
	zoneid = device->lid_zoneid;
	slot = device->lid_slot;
	list_remove(&lx_input_devices, device);
	device->lid_dead = B_TRUE;
	handle->lih_device = NULL;

	for (client = list_head(&device->lid_clients); client != NULL;
	    client = list_next(&device->lid_clients, client)) {
		lx_input_client_wakeup(client, POLLHUP | POLLERR);
	}

	free_device = lx_input_device_rele_locked(device);
	mutex_exit(&device->lid_lock);
	mutex_exit(&lx_input_lock);

	if (free_device)
		lx_input_device_free(device);
	lx_netlink_input_uevent(zoneid, "remove", slot);
}

static int
lx_input_device_create(lx_input_handle_t *handle)
{
	lx_input_device_t *device;
	uint_t slot;

	if (handle->lih_device != NULL)
		return (EINVAL);
	if (handle->lih_config.lic_name[0] == '\0')
		return (EINVAL);

	device = kmem_zalloc(sizeof (*device), KM_SLEEP);
	mutex_init(&device->lid_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&device->lid_clients, sizeof (lx_input_client_t),
	    offsetof(lx_input_client_t, lic_link));
	device->lid_zoneid = handle->lih_zoneid;
	device->lid_refs = 1;
	device->lid_config = handle->lih_config;
	lx_input_setbit(device->lid_config.lic_evbits, LX_EV_SYN);

	mutex_enter(&lx_input_lock);
	for (slot = 0; slot < LX_INPUT_EVENT_COUNT; slot++) {
		if (lx_input_device_lookup(handle->lih_zoneid, slot) == NULL)
			break;
	}
	if (slot == LX_INPUT_EVENT_COUNT) {
		mutex_exit(&lx_input_lock);
		device->lid_dead = B_TRUE;
		device->lid_refs = 0;
		lx_input_device_free(device);
		return (ENOSPC);
	}

	device->lid_slot = slot;
	list_insert_tail(&lx_input_devices, device);
	handle->lih_device = device;
	mutex_exit(&lx_input_lock);
	lx_netlink_input_uevent(device->lid_zoneid, "add", device->lid_slot);

	return (0);
}

static void
lx_input_event_time(lx_input_client_t *client, lx_input_event_t *event)
{
	if (client->lic_clock == CLOCK_MONOTONIC) {
		hrtime_t now = gethrtime();

		event->lie_sec = now / NANOSEC;
		event->lie_usec = (now % NANOSEC) / (NANOSEC / MICROSEC);
	} else {
		timestruc_t now;

		gethrestime(&now);
		event->lie_sec = now.tv_sec;
		event->lie_usec = now.tv_nsec / (NANOSEC / MICROSEC);
	}
}

static void
lx_input_client_enqueue(lx_input_client_t *client,
    const lx_input_event_t *source)
{
	lx_input_event_t event = *source;
	uint_t tail;

	ASSERT(MUTEX_HELD(&client->lic_device->lid_lock));

	if (client->lic_count == LX_INPUT_QUEUE_SIZE) {
		client->lic_head = 0;
		client->lic_count = 0;
		bzero(&event, sizeof (event));
		event.lie_type = LX_EV_SYN;
		event.lie_code = LX_SYN_DROPPED;
		lx_input_event_time(client, &event);
		client->lic_queue[0] = event;
		client->lic_count = 1;
		event = *source;
	}

	lx_input_event_time(client, &event);
	tail = (client->lic_head + client->lic_count) % LX_INPUT_QUEUE_SIZE;
	client->lic_queue[tail] = event;
	client->lic_count++;
	lx_input_client_wakeup(client, POLLIN | POLLRDNORM);
}

static boolean_t
lx_input_event_supported(lx_input_device_t *device,
    const lx_input_event_t *event)
{
	uint8_t *bits;
	uint_t max;

	if (event->lie_type == LX_EV_SYN)
		return (event->lie_code <= LX_SYN_DROPPED);
	if (event->lie_type > LX_EV_MAX ||
	    !lx_input_testbit(device->lid_config.lic_evbits,
	    event->lie_type))
		return (B_FALSE);
	if (lx_input_config_bits(&device->lid_config, event->lie_type,
	    &bits, &max) != 0 || event->lie_code > max)
		return (B_FALSE);

	return (bits != NULL && lx_input_testbit(bits, event->lie_code));
}

static void
lx_input_device_event(lx_input_device_t *device, lx_input_event_t *event)
{
	lx_input_client_t *client;

	mutex_enter(&device->lid_lock);
	if (device->lid_dead || !lx_input_event_supported(device, event)) {
		mutex_exit(&device->lid_lock);
		return;
	}

	if (event->lie_type == LX_EV_KEY && event->lie_code <= LX_KEY_MAX) {
		if (event->lie_value != 0)
			lx_input_setbit(device->lid_keystate, event->lie_code);
		else
			device->lid_keystate[event->lie_code >> 3] &=
			    ~(1U << (event->lie_code & 7));
	} else if (event->lie_type == LX_EV_SW &&
	    event->lie_code <= LX_SW_MAX) {
		if (event->lie_value != 0)
			lx_input_setbit(device->lid_swstate, event->lie_code);
		else
			device->lid_swstate[event->lie_code >> 3] &=
			    ~(1U << (event->lie_code & 7));
	} else if (event->lie_type == LX_EV_LED &&
	    event->lie_code <= LX_LED_MAX) {
		if (event->lie_value != 0)
			lx_input_setbit(device->lid_ledstate, event->lie_code);
		else
			device->lid_ledstate[event->lie_code >> 3] &=
			    ~(1U << (event->lie_code & 7));
	} else if (event->lie_type == LX_EV_ABS &&
	    event->lie_code <= LX_ABS_MAX) {
		device->lid_config.lic_absinfo[event->lie_code].lia_value =
		    event->lie_value;
	}

	if (device->lid_grabber != NULL) {
		lx_input_client_enqueue(device->lid_grabber, event);
	} else {
		for (client = list_head(&device->lid_clients); client != NULL;
		    client = list_next(&device->lid_clients, client)) {
			lx_input_client_enqueue(client, event);
		}
	}
	mutex_exit(&device->lid_lock);
}

static int
lx_input_event_copyin(uio_t *uio, lx_input_event_t *event)
{
	if (get_udatamodel() == DATAMODEL_ILP32) {
		lx_input_event32_t event32;
		int error;

		if ((error = uiomove(&event32, sizeof (event32), UIO_WRITE,
		    uio)) != 0)
			return (error);
		event->lie_sec = event32.lie_sec;
		event->lie_usec = event32.lie_usec;
		event->lie_type = event32.lie_type;
		event->lie_code = event32.lie_code;
		event->lie_value = event32.lie_value;
	} else {
		lx_input_event64_t event64;
		int error;

		if ((error = uiomove(&event64, sizeof (event64), UIO_WRITE,
		    uio)) != 0)
			return (error);
		event->lie_sec = event64.lie_sec;
		event->lie_usec = event64.lie_usec;
		event->lie_type = event64.lie_type;
		event->lie_code = event64.lie_code;
		event->lie_value = event64.lie_value;
	}

	return (0);
}

static int
lx_input_event_copyout(uio_t *uio, const lx_input_event_t *event)
{
	if (get_udatamodel() == DATAMODEL_ILP32) {
		lx_input_event32_t event32;

		event32.lie_sec = (int32_t)event->lie_sec;
		event32.lie_usec = (int32_t)event->lie_usec;
		event32.lie_type = event->lie_type;
		event32.lie_code = event->lie_code;
		event32.lie_value = event->lie_value;
		return (uiomove(&event32, sizeof (event32), UIO_READ, uio));
	} else {
		lx_input_event64_t event64;

		event64.lie_sec = event->lie_sec;
		event64.lie_usec = event->lie_usec;
		event64.lie_type = event->lie_type;
		event64.lie_code = event->lie_code;
		event64.lie_value = event->lie_value;
		return (uiomove(&event64, sizeof (event64), UIO_READ, uio));
	}
}

static size_t
lx_input_event_size(void)
{
	return (get_udatamodel() == DATAMODEL_ILP32 ?
	    sizeof (lx_input_event32_t) : sizeof (lx_input_event64_t));
}

static int
lx_input_open(dev_t *devp, int flag __unused, int otyp, cred_t *cr)
{
	lx_input_client_t *client = NULL;
	lx_input_device_t *device;
	lx_input_handle_t *handle;
	major_t major = getemajor(*devp);
	minor_t type = getminor(*devp);
	minor_t id, clone;
	zoneid_t zoneid = crgetzoneid(cr);

	if (otyp != OTYP_CHR || curproc->p_brand != &lx_brand)
		return (ENXIO);
	if (type > LX_INPUT_MINOR_EVENT_LAST)
		return (ENXIO);

	id = (minor_t)(uintptr_t)vmem_alloc(lx_input_minor_ids, 1,
	    VM_BESTFIT | VM_SLEEP);
	clone = LX_INPUT_MINOR_CLONE(id, type);
	if (ddi_soft_state_zalloc(lx_input_softstate, clone) != DDI_SUCCESS) {
		vmem_free(lx_input_minor_ids, (void *)(uintptr_t)id, 1);
		return (ENXIO);
	}

	handle = ddi_get_soft_state(lx_input_softstate, clone);
	mutex_init(&handle->lih_lock, NULL, MUTEX_DEFAULT, NULL);
	handle->lih_zoneid = zoneid;

	if (type == LX_INPUT_MINOR_UINPUT) {
		handle->lih_type = LX_IHT_UINPUT;
	} else {
		client = kmem_zalloc(sizeof (*client), KM_SLEEP);
		cv_init(&client->lic_cv, NULL, CV_DEFAULT, NULL);
		client->lic_clock = CLOCK_REALTIME;

		mutex_enter(&lx_input_lock);
		device = lx_input_device_lookup(zoneid,
		    type - LX_INPUT_MINOR_EVENT_BASE);
		if (device == NULL) {
			mutex_exit(&lx_input_lock);
			cv_destroy(&client->lic_cv);
			kmem_free(client, sizeof (*client));
			mutex_destroy(&handle->lih_lock);
			ddi_soft_state_free(lx_input_softstate, clone);
			vmem_free(lx_input_minor_ids, (void *)(uintptr_t)id, 1);
			return (ENODEV);
		}

		mutex_enter(&device->lid_lock);
		ASSERT(!device->lid_dead);
		client->lic_device = device;
		list_insert_tail(&device->lid_clients, client);
		device->lid_refs++;
		mutex_exit(&device->lid_lock);
		mutex_exit(&lx_input_lock);

		handle->lih_type = LX_IHT_EVENT;
		handle->lih_client = client;
	}

	*devp = makedevice(major, clone);
	return (0);
}

static int
lx_input_close(dev_t dev, int flag __unused, int otyp __unused,
    cred_t *cr __unused)
{
	lx_input_handle_t *handle;
	minor_t clone = getminor(dev);
	minor_t id = clone >> LX_INPUT_MINOR_BITS;

	handle = ddi_get_soft_state(lx_input_softstate, clone);
	if (handle->lih_type == LX_IHT_UINPUT) {
		mutex_enter(&handle->lih_lock);
		lx_input_device_destroy(handle);
		mutex_exit(&handle->lih_lock);
	} else {
		lx_input_client_t *client = handle->lih_client;
		lx_input_device_t *device = client->lic_device;
		boolean_t free_device;

		mutex_enter(&device->lid_lock);
		if (device->lid_grabber == client)
			device->lid_grabber = NULL;
		list_remove(&device->lid_clients, client);
		free_device = lx_input_device_rele_locked(device);
		mutex_exit(&device->lid_lock);

		pollhead_clean(&client->lic_pollhead);
		cv_destroy(&client->lic_cv);
		kmem_free(client, sizeof (*client));
		if (free_device)
			lx_input_device_free(device);
	}

	mutex_destroy(&handle->lih_lock);
	ddi_soft_state_free(lx_input_softstate, clone);
	vmem_free(lx_input_minor_ids, (void *)(uintptr_t)id, 1);
	return (0);
}

static int
lx_input_read(dev_t dev, uio_t *uio, cred_t *cr __unused)
{
	lx_input_handle_t *handle;
	lx_input_client_t *client;
	lx_input_device_t *device;
	size_t event_size = lx_input_event_size();
	size_t initial_resid = uio->uio_resid;
	int error = 0;

	handle = ddi_get_soft_state(lx_input_softstate, getminor(dev));
	if (handle->lih_type != LX_IHT_EVENT)
		return ((uio->uio_fmode & (FNDELAY | FNONBLOCK)) != 0 ?
		    EAGAIN : ENOTSUP);
	if (uio->uio_resid < event_size)
		return (EINVAL);

	mutex_enter(&handle->lih_lock);
	client = handle->lih_client;
	device = client->lic_device;
	mutex_enter(&device->lid_lock);
	while (client->lic_count == 0 && !device->lid_dead) {
		if ((uio->uio_fmode & (FNDELAY | FNONBLOCK)) != 0) {
			mutex_exit(&device->lid_lock);
			mutex_exit(&handle->lih_lock);
			return (EAGAIN);
		}
		if (!cv_wait_sig_swap(&client->lic_cv, &device->lid_lock)) {
			mutex_exit(&device->lid_lock);
			mutex_exit(&handle->lih_lock);
			return (EINTR);
		}
	}

	while (client->lic_count != 0 && uio->uio_resid >= event_size) {
		lx_input_event_t event = client->lic_queue[client->lic_head];

		mutex_exit(&device->lid_lock);
		error = lx_input_event_copyout(uio, &event);
		mutex_enter(&device->lid_lock);
		if (error != 0)
			break;
		client->lic_head = (client->lic_head + 1) % LX_INPUT_QUEUE_SIZE;
		client->lic_count--;
	}

	if (error == 0 && uio->uio_resid == initial_resid &&
	    client->lic_count == 0 && device->lid_dead)
		error = ENODEV;
	mutex_exit(&device->lid_lock);
	mutex_exit(&handle->lih_lock);
	return (error);
}

static int
lx_input_legacy_setup(lx_input_handle_t *handle, uio_t *uio)
{
	lx_uinput_user_dev_t setup;
	uint_t i;
	int error;

	if (handle->lih_device != NULL || uio->uio_resid != sizeof (setup))
		return (EINVAL);
	if ((error = uiomove(&setup, sizeof (setup), UIO_WRITE, uio)) != 0)
		return (error);

	handle->lih_config.lic_id = setup.luud_id;
	handle->lih_config.lic_ff_effects_max = setup.luud_ff_effects_max;
	bcopy(setup.luud_name, handle->lih_config.lic_name,
	    sizeof (handle->lih_config.lic_name));
	handle->lih_config.lic_name[LX_INPUT_NAME_SIZE - 1] = '\0';
	for (i = 0; i <= LX_ABS_MAX; i++) {
		handle->lih_config.lic_absinfo[i].lia_maximum =
		    setup.luud_absmax[i];
		handle->lih_config.lic_absinfo[i].lia_minimum =
		    setup.luud_absmin[i];
		handle->lih_config.lic_absinfo[i].lia_fuzz =
		    setup.luud_absfuzz[i];
		handle->lih_config.lic_absinfo[i].lia_flat =
		    setup.luud_absflat[i];
	}
	return (0);
}

static int
lx_input_write(dev_t dev, uio_t *uio, cred_t *cr __unused)
{
	lx_input_handle_t *handle;
	lx_input_device_t *device;
	size_t event_size = lx_input_event_size();
	int error;

	handle = ddi_get_soft_state(lx_input_softstate, getminor(dev));
	if (handle->lih_type != LX_IHT_UINPUT)
		return (EINVAL);
	mutex_enter(&handle->lih_lock);
	if (handle->lih_device == NULL) {
		int error = lx_input_legacy_setup(handle, uio);

		mutex_exit(&handle->lih_lock);
		return (error);
	}
	if (uio->uio_resid == 0 || (uio->uio_resid % event_size) != 0)
		goto invalid;

	device = handle->lih_device;
	while (uio->uio_resid != 0) {
		lx_input_event_t event;

		if ((error = lx_input_event_copyin(uio, &event)) != 0)
			goto failed;
		lx_input_device_event(device, &event);
	}
	mutex_exit(&handle->lih_lock);
	return (0);

invalid:
	mutex_exit(&handle->lih_lock);
	return (EINVAL);

failed:
	mutex_exit(&handle->lih_lock);
	return (error);
}

static int
lx_input_copyout_data(intptr_t arg, int mode, const void *data,
    size_t data_size, size_t requested, int *rvalp)
{
	void *buffer;
	size_t size;

	if (requested == 0 || requested > LX_INPUT_MAX_COPY)
		return (EINVAL);
	size = MIN(data_size, requested);
	buffer = kmem_zalloc(requested, KM_SLEEP);
	bcopy(data, buffer, size);
	if (ddi_copyout(buffer, (void *)arg, requested, mode) != 0) {
		kmem_free(buffer, requested);
		return (EFAULT);
	}
	kmem_free(buffer, requested);
	*rvalp = (int)size;
	return (0);
}

static int
lx_input_copyout_string(intptr_t arg, int mode, const char *string,
    size_t requested, int *rvalp)
{
	return (lx_input_copyout_data(arg, mode, string, strlen(string) + 1,
	    requested, rvalp));
}

static int
lx_uinput_set_code(lx_input_config_t *config, uint_t nr, uint_t code)
{
	uint8_t *bits;
	uint_t max;
	uint_t type;

	switch (nr) {
	case 100:
		if (code > LX_EV_MAX)
			return (EINVAL);
		lx_input_setbit(config->lic_evbits, code);
		return (0);
	case 101:
		type = LX_EV_KEY;
		break;
	case 102:
		type = LX_EV_REL;
		break;
	case 103:
		type = LX_EV_ABS;
		break;
	case 104:
		type = LX_EV_MSC;
		break;
	case 105:
		type = LX_EV_LED;
		break;
	case 106:
		type = LX_EV_SND;
		break;
	case 107:
		type = LX_EV_FF;
		break;
	case 109:
		type = LX_EV_SW;
		break;
	case 110:
		if (code > LX_INPUT_PROP_MAX)
			return (EINVAL);
		lx_input_setbit(config->lic_propbits, code);
		return (0);
	default:
		return (ENOTTY);
	}

	if (lx_input_config_bits(config, type, &bits, &max) != 0 || code > max)
		return (EINVAL);
	lx_input_setbit(bits, code);
	lx_input_setbit(config->lic_evbits, type);
	return (0);
}

static int
lx_uinput_ioctl(lx_input_handle_t *handle, uint32_t cmd, intptr_t arg,
    int mode, int *rvalp)
{
	lx_input_config_t *config = &handle->lih_config;
	uint_t nr = LX_IOC_NR(cmd);

	if (cmd == LX_UI_DEV_CREATE)
		return (lx_input_device_create(handle));
	if (cmd == LX_UI_DEV_DESTROY) {
		if (handle->lih_device == NULL)
			return (EINVAL);
		lx_input_device_destroy(handle);
		return (0);
	}
	if (cmd == LX_UI_GET_VERSION) {
		uint32_t version = LX_UINPUT_VERSION;

		return (ddi_copyout(&version, (void *)arg, sizeof (version),
		    mode) == 0 ? 0 : EFAULT);
	}
	if (nr == 44) {
		char sysname[32];

		if (handle->lih_device == NULL)
			return (ENODEV);
		(void) snprintf(sysname, sizeof (sysname), "input%u",
		    handle->lih_device->lid_slot);
		return (lx_input_copyout_string(arg, mode, sysname,
		    LX_IOC_SIZE(cmd), rvalp));
	}
	if (handle->lih_device != NULL)
		return (EINVAL);
	if (cmd == LX_UI_DEV_SETUP) {
		lx_uinput_setup_t setup;

		if (ddi_copyin((void *)arg, &setup, sizeof (setup), mode) != 0)
			return (EFAULT);
		config->lic_id = setup.lus_id;
		config->lic_ff_effects_max = setup.lus_ff_effects_max;
		bcopy(setup.lus_name, config->lic_name,
		    sizeof (config->lic_name));
		config->lic_name[LX_INPUT_NAME_SIZE - 1] = '\0';
		return (0);
	}
	if (cmd == LX_UI_ABS_SETUP) {
		lx_uinput_abs_setup_t setup;

		if (ddi_copyin((void *)arg, &setup, sizeof (setup), mode) != 0)
			return (EFAULT);
		if (setup.luas_code > LX_ABS_MAX)
			return (EINVAL);
		config->lic_absinfo[setup.luas_code] = setup.luas_absinfo;
		lx_input_setbit(config->lic_absbits, setup.luas_code);
		lx_input_setbit(config->lic_evbits, LX_EV_ABS);
		return (0);
	}
	if (nr == 108) {
		return (copyinstr((char *)arg, config->lic_phys,
		    sizeof (config->lic_phys), NULL) == 0 ? 0 : EFAULT);
	}
	return (lx_uinput_set_code(config, nr, (uint_t)arg));
}

static int
lx_evdev_bits(lx_input_device_t *device, uint_t type, const uint8_t **bitsp,
    size_t *sizep)
{
	lx_input_config_t *config = &device->lid_config;
	uint8_t *bits;
	uint_t max;

	if (type == 0) {
		*bitsp = config->lic_evbits;
		*sizep = sizeof (config->lic_evbits);
		return (0);
	}
	if (lx_input_config_bits(config, type, &bits, &max) != 0)
		return (EINVAL);
	*bitsp = bits;
	*sizep = LX_INPUT_BITS(max);
	return (0);
}

static int
lx_evdev_ioctl(lx_input_handle_t *handle, uint32_t cmd, intptr_t arg,
    int mode, int *rvalp)
{
	lx_input_client_t *client = handle->lih_client;
	lx_input_device_t *device = client->lic_device;
	uint_t nr = LX_IOC_NR(cmd);
	size_t requested = LX_IOC_SIZE(cmd);
	int error = 0;

	mutex_enter(&device->lid_lock);
	if (device->lid_dead) {
		mutex_exit(&device->lid_lock);
		return (ENODEV);
	}

	if (cmd == LX_EVIOCGVERSION) {
		int version = LX_EV_VERSION;

		mutex_exit(&device->lid_lock);
		return (ddi_copyout(&version, (void *)arg, sizeof (version),
		    mode) == 0 ? 0 : EFAULT);
	}
	if (cmd == LX_EVIOCGID) {
		lx_input_id_t id = device->lid_config.lic_id;

		mutex_exit(&device->lid_lock);
		return (ddi_copyout(&id, (void *)arg, sizeof (id), mode) == 0 ?
		    0 : EFAULT);
	}
	if (cmd == LX_EVIOCGRAB) {
		if (arg != 0) {
			if (device->lid_grabber != NULL &&
			    device->lid_grabber != client) {
				error = EBUSY;
			} else {
				device->lid_grabber = client;
				client->lic_grabbed = B_TRUE;
			}
		} else if (device->lid_grabber == client) {
			device->lid_grabber = NULL;
			client->lic_grabbed = B_FALSE;
		}
		mutex_exit(&device->lid_lock);
		return (error);
	}
	if (cmd == LX_EVIOCSCLOCKID) {
		clockid_t clock;

		mutex_exit(&device->lid_lock);
		if (ddi_copyin((void *)arg, &clock, sizeof (clock), mode) != 0)
			return (EFAULT);
		if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
			return (EINVAL);
		mutex_enter(&device->lid_lock);
		client->lic_clock = clock;
		mutex_exit(&device->lid_lock);
		return (0);
	}

	switch (nr) {
	case 0x06:
		mutex_exit(&device->lid_lock);
		return (lx_input_copyout_string(arg, mode,
		    device->lid_config.lic_name, requested, rvalp));
	case 0x07:
		mutex_exit(&device->lid_lock);
		return (lx_input_copyout_string(arg, mode,
		    device->lid_config.lic_phys, requested, rvalp));
	case 0x08:
		mutex_exit(&device->lid_lock);
		return (lx_input_copyout_string(arg, mode,
		    device->lid_config.lic_uniq, requested, rvalp));
	case 0x09:
		mutex_exit(&device->lid_lock);
		return (lx_input_copyout_data(arg, mode,
		    device->lid_config.lic_propbits,
		    sizeof (device->lid_config.lic_propbits), requested,
		    rvalp));
	case 0x18:
		{
			uint8_t state[sizeof (device->lid_keystate)];

			bcopy(device->lid_keystate, state, sizeof (state));
			mutex_exit(&device->lid_lock);
			return (lx_input_copyout_data(arg, mode, state,
			    sizeof (state), requested, rvalp));
		}
	case 0x19:
		{
			uint8_t state[sizeof (device->lid_ledstate)];

			bcopy(device->lid_ledstate, state, sizeof (state));
			mutex_exit(&device->lid_lock);
			return (lx_input_copyout_data(arg, mode, state,
			    sizeof (state), requested, rvalp));
		}
	case 0x1b:
		{
			uint8_t state[sizeof (device->lid_swstate)];

			bcopy(device->lid_swstate, state, sizeof (state));
			mutex_exit(&device->lid_lock);
			return (lx_input_copyout_data(arg, mode, state,
			    sizeof (state), requested, rvalp));
		}
	default:
		break;
	}

	if (nr >= 0x20 && nr <= 0x20 + LX_EV_MAX) {
		const uint8_t *bits;
		size_t size;

		error = lx_evdev_bits(device, nr - 0x20, &bits, &size);
		mutex_exit(&device->lid_lock);
		if (error != 0)
			return (error);
		return (lx_input_copyout_data(arg, mode, bits, size, requested,
		    rvalp));
	}
	if (nr >= 0x40 && nr <= 0x40 + LX_ABS_MAX) {
		uint_t code = nr - 0x40;
		lx_input_absinfo_t absinfo;

		if (!lx_input_testbit(device->lid_config.lic_absbits, code)) {
			mutex_exit(&device->lid_lock);
			return (EINVAL);
		}
		absinfo = device->lid_config.lic_absinfo[code];

		mutex_exit(&device->lid_lock);
		return (ddi_copyout(&absinfo, (void *)arg, sizeof (absinfo),
		    mode) == 0 ? 0 : EFAULT);
	}

	mutex_exit(&device->lid_lock);
	return (ENOTTY);
}

static int
lx_input_ioctl(dev_t dev, int cmd, intptr_t arg, int mode,
    cred_t *cr __unused, int *rvalp)
{
	lx_input_handle_t *handle;

	handle = ddi_get_soft_state(lx_input_softstate, getminor(dev));
	if (cmd == FIONREAD) {
		int bytes;

		if (handle->lih_type != LX_IHT_EVENT)
			bytes = 0;
		else {
			lx_input_client_t *client = handle->lih_client;
			lx_input_device_t *device = client->lic_device;

			mutex_enter(&device->lid_lock);
			bytes = client->lic_count * lx_input_event_size();
			mutex_exit(&device->lid_lock);
		}
		return (ddi_copyout(&bytes, (void *)arg, sizeof (bytes),
		    mode) == 0 ? 0 : EFAULT);
	}

	if (handle->lih_type == LX_IHT_UINPUT) {
		int error;

		if (LX_IOC_TYPE(cmd) != LX_UINPUT_IOCTL_BASE)
			return (ENOTTY);
		mutex_enter(&handle->lih_lock);
		error = lx_uinput_ioctl(handle, (uint32_t)cmd, arg, mode,
		    rvalp);
		mutex_exit(&handle->lih_lock);
		return (error);
	}

	if (LX_IOC_TYPE(cmd) != LX_EVDEV_IOCTL_BASE)
		return (ENOTTY);
	return (lx_evdev_ioctl(handle, (uint32_t)cmd, arg, mode, rvalp));
}

static int
lx_input_poll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp)
{
	lx_input_handle_t *handle;
	lx_input_client_t *client;
	lx_input_device_t *device;
	short revents = 0;

	handle = ddi_get_soft_state(lx_input_softstate, getminor(dev));
	if (handle->lih_type == LX_IHT_UINPUT) {
		revents = POLLOUT | POLLWRNORM;
		*reventsp = revents & events;
		return (0);
	}

	client = handle->lih_client;
	device = client->lic_device;
	mutex_enter(&device->lid_lock);
	if (client->lic_count != 0)
		revents |= POLLIN | POLLRDNORM;
	if (device->lid_dead)
		revents |= POLLHUP | POLLERR;
	*reventsp = (revents & events) |
	    (revents & (POLLHUP | POLLERR));
	if ((*reventsp == 0 && !anyyet) || (events & POLLET) != 0)
		*phpp = &client->lic_pollhead;
	mutex_exit(&device->lid_lock);
	return (0);
}

static int
lx_input_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{
	char name[32];
	uint_t i;

	if (cmd == DDI_RESUME)
		return (DDI_SUCCESS);
	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	if (ddi_soft_state_init(&lx_input_softstate,
	    sizeof (lx_input_handle_t), 0) != 0)
		return (DDI_FAILURE);

	if (ddi_create_minor_node(devi, "lx_uinput", S_IFCHR,
	    LX_INPUT_MINOR_UINPUT, DDI_PSEUDO, 0) == DDI_FAILURE)
		goto fail;
	for (i = 0; i < LX_INPUT_EVENT_COUNT; i++) {
		(void) snprintf(name, sizeof (name), "lx_event%u", i);
		if (ddi_create_minor_node(devi, name, S_IFCHR,
		    LX_INPUT_MINOR_EVENT_BASE + i, DDI_PSEUDO, 0) ==
		    DDI_FAILURE)
			goto fail;
	}

	lx_input_minor_ids = vmem_create("lx_input_minor_ids", (void *)1,
	    (UINT32_MAX >> LX_INPUT_MINOR_BITS) - 1, 1, NULL, NULL, NULL, 0,
	    VM_SLEEP | VMC_IDENTIFIER);
	lx_input_devi = devi;
	ddi_report_dev(devi);
	return (DDI_SUCCESS);

fail:
	ddi_remove_minor_node(devi, NULL);
	ddi_soft_state_fini(&lx_input_softstate);
	return (DDI_FAILURE);
}

static int
lx_input_detach(dev_info_t *devi, ddi_detach_cmd_t cmd)
{
	if (cmd == DDI_SUSPEND)
		return (DDI_SUCCESS);
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	mutex_enter(&lx_input_lock);
	if (!list_is_empty(&lx_input_devices)) {
		mutex_exit(&lx_input_lock);
		return (DDI_FAILURE);
	}
	mutex_exit(&lx_input_lock);

	vmem_destroy(lx_input_minor_ids);
	lx_input_minor_ids = NULL;
	ddi_remove_minor_node(devi, NULL);
	ddi_soft_state_fini(&lx_input_softstate);
	lx_input_devi = NULL;
	return (DDI_SUCCESS);
}

static int
lx_input_info(dev_info_t *devi __unused, ddi_info_cmd_t cmd,
    void *arg __unused, void **result)
{
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = lx_input_devi;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static struct cb_ops lx_input_cb_ops = {
	lx_input_open,
	lx_input_close,
	nulldev,
	nulldev,
	nodev,
	lx_input_read,
	lx_input_write,
	lx_input_ioctl,
	nodev,
	nodev,
	nodev,
	lx_input_poll,
	ddi_prop_op,
	NULL,
	D_NEW | D_MP
};

static struct dev_ops lx_input_dev_ops = {
	DEVO_REV,
	0,
	lx_input_info,
	nulldev,
	nulldev,
	lx_input_attach,
	lx_input_detach,
	nodev,
	&lx_input_cb_ops,
	NULL,
	nodev,
	ddi_quiesce_not_needed
};

static struct modldrv lx_input_modldrv = {
	&mod_driverops,
	"LX virtual input",
	&lx_input_dev_ops
};

static struct modlinkage lx_input_modlinkage = {
	MODREV_1,
	(void *)&lx_input_modldrv,
	NULL
};

int
_init(void)
{
	int error;

	mutex_init(&lx_input_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&lx_input_devices, sizeof (lx_input_device_t),
	    offsetof(lx_input_device_t, lid_link));
	if ((error = mod_install(&lx_input_modlinkage)) != 0) {
		list_destroy(&lx_input_devices);
		mutex_destroy(&lx_input_lock);
	}
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&lx_input_modlinkage, modinfop));
}

int
_fini(void)
{
	int error;

	if ((error = mod_remove(&lx_input_modlinkage)) != 0)
		return (error);
	list_destroy(&lx_input_devices);
	mutex_destroy(&lx_input_lock);
	return (0);
}
