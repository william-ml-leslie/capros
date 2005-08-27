/*
 * Copyright (C) 1998, 1999, Jonathan S. Shapiro.
 * Copyright (C) 2005, Strawberry Development Group.
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

/* Drivers for 386 protection faults */

#include <eros/target.h>
#include <kerninc/kernel.h>
#include <kerninc/Activity.h>
#include <kerninc/Debug.h>
#include <kerninc/Machine.h>
/*#include <kerninc/util.h>*/
#include <kerninc/Process.h>
#include "IDT.h"

extern void halt(char);

bool
OverflowTrap(savearea_t* sa)
{
  /* Don't back up EIP. */

  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_Overflow, sa->EIP, false);

  return false;
}

bool
BoundsFault(savearea_t* sa)
{
  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_Bounds, sa->EIP, false);

  return false;
}

bool
BadOpcode(savearea_t* sa)
{
  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_BadOpcode, sa->EIP, false);

  return false;
}

bool
InvalTSSFault(savearea_t* sa)
{
  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_InvalidTSS, sa->EIP, false);

  return false;
}

bool
CoprocErrorFault(savearea_t* sa)
{
  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_FloatingPointError, sa->EIP, false);

  return false;
}

bool
AlignCheckFault(savearea_t* sa)
{
  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_Alignment, sa->EIP, false);

  return false;
}

bool
SIMDFloatingPointFault(savearea_t* sa)
{
  if ( sa_IsKernel(sa) ) {
    debug_Backtrace(0, true);
    halt('o');
  }

  proc_SetFault(((Process*) act_CurContext()), FC_SIMDFloatingPointError, sa->EIP, false);

  return false;
}


