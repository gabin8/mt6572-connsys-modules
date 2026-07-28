/* Compat shim: legacy Android wakelock API on top of wakeup sources,
 * for the downstream conn_soc port. */
#ifndef _COMPAT_LINUX_WAKELOCK_H
#define _COMPAT_LINUX_WAKELOCK_H

#include <linux/device.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>

enum { WAKE_LOCK_SUSPEND = 0 };

struct wake_lock {
	struct wakeup_source *ws;
};

static inline void wake_lock_init(struct wake_lock *l, int type,
				  const char *name)
{
	l->ws = wakeup_source_register(NULL, name);
}

static inline void wake_lock_destroy(struct wake_lock *l)
{
	wakeup_source_unregister(l->ws);
	l->ws = NULL;
}

static inline void wake_lock(struct wake_lock *l)
{
	__pm_stay_awake(l->ws);
}

static inline void wake_lock_timeout(struct wake_lock *l, long timeout)
{
	__pm_wakeup_event(l->ws, jiffies_to_msecs(timeout));
}

static inline void wake_unlock(struct wake_lock *l)
{
	__pm_relax(l->ws);
}

static inline int wake_lock_active(struct wake_lock *l)
{
	return l->ws ? l->ws->active : 0;
}

#endif
