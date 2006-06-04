/*
 * Copyright (C) 1998, 1999, 2001, Jonathan S. Shapiro.
 * Copyright (C) 2006, Strawberry Development Group.
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

#include <string.h>
#include <kerninc/kernel.h>
#include <kerninc/Node.h>
#include <kerninc/util.h>
#include <kerninc/Check.h>
#include <kerninc/Process.h>
#include <kerninc/ObjectHeader.h>
#include <kerninc/Activity.h>
#include <kerninc/Depend.h>
#include <kerninc/Invocation.h>
#include <kerninc/IRQ.h>
#include <kerninc/ObjectCache.h>

#define PREPDEBUG

#include <eros/Invoke.h>

#include <disk/DiskNodeStruct.h>

/* replacement for overloaded operator = in Node */
void 
node_SetEqualTo(Node *thisPtr, const DiskNodeStruct *other)
{
  uint32_t i = 0;

  assert (keyR_IsEmpty(&thisPtr->node_ObjHdr.keyRing));
  assert (thisPtr->node_ObjHdr.obType == ot_NtUnprepared);

  /* The invocation does not need to be committed for this one. */
  
  thisPtr->node_ObjHdr.oid = other->oid;
  thisPtr->node_ObjHdr.allocCount = other->allocCount;
  thisPtr->callCount = other->callCount;

  for (i = 0; i < EROS_NODE_SIZE; i++) {
    assert(keyBits_IsHazard(&thisPtr->slot[i]) == false);
    assert( keyBits_IsUnprepared(&thisPtr->slot[i]) );
  }

  memcpy(&thisPtr->slot[0], &other->slot[0], 
	EROS_NODE_SIZE * sizeof(thisPtr->slot[0]));
}
