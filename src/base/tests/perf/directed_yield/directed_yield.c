/*
 * Copyright (C) 1998, 1999, Jonathan S. Shapiro.
 *
 * This file is part of the EROS Operating System.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


#include <eros/target.h>
#include <eros/Invoke.h>
#include <eros/SleepKey.h>
#include <domain/domdbg.h>
#include <eros/SysTraceKey.h>

#define KR_VOID 0

#define KR_SLEEP 9
#define KR_OSTREAM 10
#define KR_SYSTRACE 11

const uint32_t __rt_stack_pages = 0;
const uint32_t __rt_stack_pointer = 0x21000;

#define ITERATIONS 1000000

int
main()
{
  int i;

  sl_sleep(KR_SLEEP, 4000);

  kprintf(KR_OSTREAM, "Beginning %d calls to directed yield\n",
	  ITERATIONS);

  systrace_start(KR_SYSTRACE, SysTrace_Mode_Instrs);

  for (i = 0; i < ITERATIONS; i++) {
    __asm__ __volatile__("int $0x30");

  systrace_stop(KR_SYSTRACE);

  systrace_start(KR_SYSTRACE, SysTrace_Mode_Imiss);

  for (i = 0; i < ITERATIONS; i++)
    __asm__ __volatile__("int $0x30");

  systrace_stop(KR_SYSTRACE);

  systrace_start(KR_SYSTRACE, SysTrace_Mode_Dmiss);

  for (i = 0; i < ITERATIONS; i++)
    __asm__ __volatile__("int $0x30");

  systrace_stop(KR_SYSTRACE);

  systrace_start(KR_SYSTRACE, SysTrace_Mode_Cycles);

  for (i = 0; i < ITERATIONS; i++)
    __asm__ __volatile__("int $0x30");

  systrace_stop(KR_SYSTRACE);

  systrace_start(KR_SYSTRACE, SysTrace_Mode_Branches);

  for (i = 0; i < ITERATIONS; i++)
    __asm__ __volatile__("int $0x30");

  systrace_stop(KR_SYSTRACE);

  systrace_start(KR_SYSTRACE, SysTrace_Mode_BrTaken);

  for (i = 0; i < ITERATIONS; i++)
    __asm__ __volatile__("int $0x30");

  systrace_stop(KR_SYSTRACE);

  kprintf(KR_OSTREAM, "Done\n");

  return 0;
}
