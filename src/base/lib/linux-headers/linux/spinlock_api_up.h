#ifndef __LINUX_SPINLOCK_API_UP_H
#define __LINUX_SPINLOCK_API_UP_H

#ifndef __LINUX_SPINLOCK_H
# error "please don't include this file directly"
#endif

/*
 * include/linux/spinlock_api_up.h
 *
 * spinlock API implementation on UP-nondebug (inlined implementation)
 *
 * portions Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Portions Copyright (C) 2007, 2008, Strawberry Development Group.
 * Released under the General Public License (GPL).
 */
/* This material is based upon work supported by the US Defense Advanced
Research Projects Agency under Contract No. W31P4Q-07-C-0070.
Approved for public release, distribution unlimited. */

#define in_lock_functions(ADDR)		0

#define assert_spin_locked(lock)	do { (void)(lock); } while (0)

#ifdef CONFIG_SPINLOCK_USES_IRQ

/*
 * In the UP-nondebug case there's no real locking going on, so the
 * only thing we have to do is to keep the preempt counts and irq
 * flags straight, to supress compiler warnings of unused lock
 * variables, and to add the proper checker annotations:
 */
#define __LOCK(lock) \
  do { preempt_disable(); __acquire(lock); (void)(lock); } while (0)

/* In CapROS, the only way to disable preemption is to disable irq. 
So, once irq is disabled, no point in disabling preemption. */
#define __LOCK_IRQ(lock) \
  do { local_irq_disable(); __acquire(lock); (void)(lock); } while (0)

#define __LOCK_IRQSAVE(lock, flags) \
  do { local_irq_save(flags); __acquire(lock); (void)(lock); } while (0)

#define __UNLOCK(lock) \
  do { preempt_enable(); __release(lock); (void)(lock); } while (0)

#define __UNLOCK_IRQ(lock) \
  do { local_irq_enable(); __release(lock); (void)(lock); } while (0)

#define __UNLOCK_IRQRESTORE(lock, flags) \
  do { local_irq_restore(flags); __release(lock); (void)(lock); } while (0)

#else // CONFIG_SPINLOCK_USES_IRQ

#define __LOCK(lock) \
  do { mutex_lock(&(lock)->raw_lock.mutx); } while (0)

#define __LOCK_IRQ(lock) __LOCK(lock)

#define __LOCK_IRQSAVE(lock, flags) \
  do { __LOCK(lock); (void)(flags); } while (0)

#define __UNLOCK(lock) \
  do { mutex_unlock(&(lock)->raw_lock.mutx); } while (0)

#define __UNLOCK_IRQ(lock) __UNLOCK(lock)

#define __UNLOCK_IRQRESTORE(lock, flags) \
  do { __UNLOCK(lock); (void)(flags); } while (0)

#endif // CONFIG_SPINLOCK_USES_IRQ

#define _spin_lock(lock)			__LOCK(lock)
#define _spin_lock_nested(lock, subclass)	__LOCK(lock)
#define _read_lock(lock)			__LOCK(lock)
#define _write_lock(lock)			__LOCK(lock)
#define _spin_lock_bh(lock)			__LOCK_IRQ(lock)
#define _read_lock_bh(lock)			__LOCK_IRQ(lock)
#define _write_lock_bh(lock)			__LOCK_IRQ(lock)
#define _spin_lock_irq(lock)			__LOCK_IRQ(lock)
#define _read_lock_irq(lock)			__LOCK_IRQ(lock)
#define _write_lock_irq(lock)			__LOCK_IRQ(lock)
#define _spin_lock_irqsave(lock, flags)		__LOCK_IRQSAVE(lock, flags)
#define _read_lock_irqsave(lock, flags)		__LOCK_IRQSAVE(lock, flags)
#define _write_lock_irqsave(lock, flags)	__LOCK_IRQSAVE(lock, flags)
#define _spin_trylock(lock)			({ __LOCK(lock); 1; })
#define _read_trylock(lock)			({ __LOCK(lock); 1; })
#define _write_trylock(lock)			({ __LOCK(lock); 1; })
#define _spin_trylock_bh(lock)			({ __LOCK_IRQ(lock); 1; })
#define _spin_unlock(lock)			__UNLOCK(lock)
#define _read_unlock(lock)			__UNLOCK(lock)
#define _write_unlock(lock)			__UNLOCK(lock)
#define _spin_unlock_bh(lock)			__UNLOCK_IRQ(lock)
#define _write_unlock_bh(lock)			__UNLOCK_IRQ(lock)
#define _read_unlock_bh(lock)			__UNLOCK_IRQ(lock)
#define _spin_unlock_irq(lock)			__UNLOCK_IRQ(lock)
#define _read_unlock_irq(lock)			__UNLOCK_IRQ(lock)
#define _write_unlock_irq(lock)			__UNLOCK_IRQ(lock)
#define _spin_unlock_irqrestore(lock, flags)	__UNLOCK_IRQRESTORE(lock, flags)
#define _read_unlock_irqrestore(lock, flags)	__UNLOCK_IRQRESTORE(lock, flags)
#define _write_unlock_irqrestore(lock, flags)	__UNLOCK_IRQRESTORE(lock, flags)

#endif /* __LINUX_SPINLOCK_API_UP_H */
