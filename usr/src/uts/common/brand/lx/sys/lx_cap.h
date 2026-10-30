/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _SYS_LX_CAP_H
#define	_SYS_LX_CAP_H
#include <sys/cred.h>
#include <lx_userns_model.h>

extern boolean_t lx_cap_initial(const cred_t *, uint_t);
extern void lx_cap_native_get(const cred_t *, lx_caps_t *);
extern int lx_cap_native_update(cred_t *, const lx_caps_t *);

#endif /* _SYS_LX_CAP_H */
