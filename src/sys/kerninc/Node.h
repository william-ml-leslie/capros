#ifndef __NODE_H__
#define __NODE_H__
/*
 * Copyright (C) 1998, 1999, Jonathan S. Shapiro.
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

/* Node.hxx: Declaration of a Node. */

#include "ObjectHeader.h"
#include "Key.h"
#include <eros/ProcessState.h>

struct DiskNodeStruct;

/*typedef struct Node Node;*/

struct Node {
  ObjectHeader node_ObjHdr;
  ObCount callCount;

  uint8_t objAge;
  uint8_t kernPin;
  
  Key slot[EROS_NODE_SIZE];
};

INLINE Node *
objH_ToNode(ObjectHeader * pObj)
{
  return (Node *)pObj;	// the ObjectHeader is the first component of Node
}

INLINE ObjectHeader *
node_ToObj(Node * pNode)
{
  return &pNode->node_ObjHdr;
}

INLINE Node *         
objH_LookupNode(OID oid)
{
  return objH_ToNode(objH_Lookup(ot_NtUnprepared, oid));
}

INLINE bool
node_IsKernelPinned(Node * thisPtr)
{
  return (thisPtr->kernPin != 0);
}

INLINE void
node_MakeDirty(Node * pNode)
{
  objH_MakeObjectDirty(node_ToObj(pNode));
  pNode->objAge = age_NewBorn;
}

INLINE Key *
node_GetKeyAtSlot(Node *thisPtr, int n)
{
  return & thisPtr->slot[n];
}

void node_SetEqualTo(Node *thisPtr, const struct DiskNodeStruct *);

bool node_Validate(Node* thisPtr);

void node_ClearHazard(Node* thisPtr, uint32_t ndx);

void node_UnprepareHazardedSlot(Node* thisPtr, uint32_t ndx);

void node_RescindHazardedSlot(Node* thisPtr, uint32_t ndx, bool mustUnprepare);

/* Prepare node under various sorts of contracts.  In unprepare, set
 * zapMe to true if the *calling* domain should be deprepared in
 * order to satisfy the request.  It shouldn't to satisfy segment
 * walks or to prepare a domain, but it should to set a key in a
 * node slot.
 */
bool node_PrepAsSegment(Node* thisPtr);
void node_PrepAsDomain(Node* thisPtr);
bool node_PrepAsDomainSubnode(Node* thisPtr, ObType, Process*);

Process *node_GetDomainContext(Node* thisPtr);


bool node_Unprepare(Node* thisPtr, bool zapMe);
    
void node_DoClearThisNode(Node* thisPtr);

INLINE void 
node_ClearThisNode(Node* thisPtr)
{
  node_MakeDirty(thisPtr);
  node_DoClearThisNode(thisPtr);
}

void node_SetSlot(Node* thisPtr, int ndx, Node* node /*@ NOT NULL @*/, uint32_t otherSlot);

#endif /* __NODE_H__ */
