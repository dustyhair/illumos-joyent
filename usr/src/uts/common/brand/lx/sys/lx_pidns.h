/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _SYS_LX_PIDNS_H
#define	_SYS_LX_PIDNS_H

#include <sys/types.h>
#include <sys/cred.h>

#define	LX_PIDNS_MAX_DEPTH	32
#define	LX_PIDNS_PID_LIMIT	4194304
#define	LX_PIDNS_DEFAULT_MAX	1024

typedef struct lx_pidns lx_pidns_t;
typedef struct lx_pidns_map lx_pidns_map_t;
struct pid;

/*
 * Private PID map backend; Linux namespace flags remain disabled.
 * Creation checks CAP_SYS_ADMIN in the owner's user namespace and charges
 * its zone-wide PID namespace budget. NULL parent denotes the outer zone.
 * Each namespace owns an immutable credential and parent reference.
 * Numeric IDs are in [1, limit). The first reservation is PID1; an aborted
 * unpublished reservation permits another attempt. A published PID1 cannot
 * be replaced after shutdown or its map's last release.
 *
 * Reserve outside process locks, before native fork/LWP publication. Publish
 * only after the global lx task hash exists, without pidlock or p_lock held.
 * Publication is atomic across ancestors. A map owns its namespace chain
 * and a native PID reference; it does not keep the task alive. Lookup returns
 * a held native identity, not task existence or signal/lookup authority.
 * Release and lookup accept pidlock held, as required by native reap.
 */
extern int lx_pidns_create(cred_t *, lx_pidns_t *, uint_t, lx_pidns_t **);
extern void lx_pidns_hold(lx_pidns_t *);
extern void lx_pidns_rele(lx_pidns_t *);
extern boolean_t lx_pidns_busy(void);
extern void lx_pidns_shutdown(lx_pidns_t *);
extern int lx_pidns_reserve(lx_pidns_t *, lx_pidns_map_t **);
extern int lx_pidns_publish(lx_pidns_map_t *, pid_t);
extern void lx_pidns_map_hold(lx_pidns_map_t *);
extern void lx_pidns_map_rele(lx_pidns_map_t *);
extern pid_t lx_pidns_map_id(lx_pidns_map_t *, lx_pidns_t *);
extern pid_t lx_pidns_visible(lx_pidns_t *, pid_t);
extern struct pid *lx_pidns_lookup(lx_pidns_t *, pid_t);

#endif /* _SYS_LX_PIDNS_H */
