/*
 * Copyright (C) 2001, Jonathan S. Shapiro.
 * Copyright (C) 2005, 2006, 2007, 2008, Strawberry Development Group.
 *
 * This file is part of the CapROS Operating System.
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
/* This material is based upon work supported by the US Defense Advanced
Research Projects Agency under Contract No. W31P4Q-07-C-0070.
Approved for public release, distribution unlimited. */

/* ObjectRange driver for preloaded ram ranges */

#include <string.h>
#include <idl/capros/Range.h>
#include <kerninc/kernel.h>
#include <kerninc/Activity.h>
#include <kerninc/Machine.h>
#include <kerninc/util.h>
#include <kerninc/ObjectCache.h>
#include <kerninc/ObjectSource.h>
#include <kerninc/Node.h>
#include <eros/Device.h>
#include <disk/PagePot.h>
#include <disk/DiskNodeStruct.h>
#include <disk/NPODescr.h>
#include <arch-kerninc/Page-inline.h>
#include <arch-kerninc/kern-target.h>

#define dbg_fetch	0x1
#define dbg_init	0x2

/* Following should be an OR of some of the above */
#define dbg_flags   ( 0u )

#define DEBUG(x) if (dbg_##x & dbg_flags)

void
AssertPageIsFree(PageHeader * pageH)
{
  switch (pageH_GetObType(pageH)) {
  case ot_PtFreeFrame:
  case ot_PtSecondary:	/* This is part of a free block. */
    break;

  default:
    /* If this happens, we need to figure out a way to prevent
    the preloaded data from getting allocated in initialization. */
    assert(!"Preloaded data was overwritten!");
  }
}

void
preload_Init(void)
{
  int i, j, k;

  struct NPObjectsDescriptor * npod = NPObDescr;
  kpa_t pagePA = VTOP((kva_t)NPObDescr);
  // Make sure the preloaded data hasn't been inadvertently allocated.

  uint32_t nf = 0;	// number of frames in the preload images

  // npod->numPreloadImages should be 1 or 2, but we don't need to check that.
  for (k = npod->numPreloadImages; k > 0; k--) {
    uint32_t thisFrames = 1 + npod->numFrames;	// including the header frame
    nf += thisFrames;
    npod = (struct NPObjectsDescriptor *)
           ((char *)npod + thisFrames * EROS_PAGE_SIZE);
  }

  PageHeader * pageH = objC_PhysPageToObHdr(pagePA);
  for (i = 0; i < nf; i++) {
    AssertPageIsFree(pageH++);
  }
  // Whew, preloaded data is safe.

  // Load all the preload images.
  npod = NPObDescr;	// start at beginning again
  for (k = npod->numPreloadImages; k > 0; k--) {
    pagePA += EROS_PAGE_SIZE;	// skip frame containing NPObjectsDescriptor
    OID oid = npod->OIDBase;

    // Preload the initialized nodes.
    j = 0;	// number of nodes loaded
    while (j < npod->numNodes) {	// load node frames
      // Load a frame of nodes.
      DiskNodeStruct * dn = KPAtoP(DiskNodeStruct *, pagePA);
      for (i = 0; i < DISK_NODES_PER_PAGE; i++) {
        Node * pNode = objC_GrabNodeFrame();

        node_SetEqualTo(pNode, dn + i);
        pNode->objAge = age_NewBorn;
        objH_InitObj(node_ToObj(pNode), oid + i, 0, 0, capros_Range_otNode);

        if (++j >= npod->numNodes)
          break;
      }
      oid += FrameToOID(1);
      pagePA += EROS_PAGE_SIZE;
    }
    // Leave the npod frame and the node frames free.

    // Preload the initialized pages.
    // Just leave them where they are and do the bookkeeping.
    for (i = 0; i < npod->numNonzeroPages; i++) {
      pageH = objC_PhysPageToObHdr(pagePA);

      // If it's part of a free block, split it up:
      while (pageH_GetObType(pageH) != ot_PtFreeFrame
             || pageH->kt_u.free.log2Pages != 0) {
        physMem_SplitContainingFreeBlock(pageH);
      }
      // Unlink from free list:
      link_Unlink(&pageH->kt_u.free.freeLink);
      pageH_ToObj(pageH)->obType = ot_PtNewAlloc;

      objC_GrabThisPageFrame(pageH);
      pageH_MDInitDataPage(pageH);
      pageH->objAge = age_NewBorn;
      objH_InitObj(pageH_ToObj(pageH), oid, 0, 0, capros_Range_otPage);

      oid += FrameToOID(1);
      pagePA += EROS_PAGE_SIZE;
    }

    printf("Preloaded %d nodes and %d pages at OID %#llx\n",
           npod->numNodes, npod->numNonzeroPages, npod->OIDBase);

    // Go on to the next preload image:
    uint32_t thisFrames = 1 + npod->numFrames;	// including the header frame
    npod = (struct NPObjectsDescriptor *)
           ((char *)npod + thisFrames * EROS_PAGE_SIZE);
  }
}

/* May Yield. */
static ObjectLocator
PreloadObSource_GetObjectType(ObjectRange * rng, OID oid)
{
  ObjectLocator objLoc;

  assert(rng->start <= oid && oid < rng->end);

  /* All the initialized preloaded objects were set up in the ObCache
  by preload_Init. Therefore this must be an uninitialized object. */

  objLoc.locType = objLoc_Preload;
  objLoc.objType = capros_Range_otNone;
  objLoc.u.preload.range = rng;
  return objLoc;
}

static struct Counts
PreloadObSource_GetObjectCounts(ObjectRange * rng, OID oid,
  ObjectLocator * pObjLoc)
{
  assert(rng->start <= oid && oid < rng->end);

  struct Counts counts = {0, 0};	// FIXME
  return counts;
}

static ObjectHeader *
PreloadObSource_GetObject(ObjectRange * rng, OID oid,
  const ObjectLocator * pObjLoc)
{
  ObjectHeader * pObj;

  assert(rng->start <= oid && oid < rng->end);

  switch (pObjLoc->objType) {
  default: ;
    assert(false);

  case capros_Range_otPage:
  {
    PageHeader * pageH = objC_GrabPageFrame();
    pObj = pageH_ToObj(pageH);

    void * dest = (void *) pageH_GetPageVAddr(pageH);
    kzero(dest, EROS_PAGE_SIZE);

    pageH_MDInitDataPage(pageH);
    pageH->objAge = age_NewBorn;
    break;
  }

  case capros_Range_otNode:
  {
    Node * pNode = objC_GrabNodeFrame();
    pObj = node_ToObj(pNode);

    pNode->nodeData = 0;

    uint32_t ndx;
    for (ndx = 0; ndx < EROS_NODE_SIZE; ndx++) {
      assert (keyBits_IsUnprepared(&pNode->slot[ndx]));
      /* not hazarded because newly loaded node */
      keyBits_InitToVoid(&pNode->slot[ndx]);
    }

    pNode->objAge = age_NewBorn;
  }
  }

  // FIXME: set count right.
  objH_InitObj(pObj, oid, 0, 0, pObjLoc->objType);

  return pObj;
}

static bool
PreloadObSource_WriteBack(ObjectRange * rng, ObjectHeader *obHdr, bool b)
{
  fatal("PreloadObSource::Write() unimplemented\n");

  return false;
}

static const ObjectSource PreloadObSource = {
  .name = "preload",
  .objS_GetObjectType = &PreloadObSource_GetObjectType,
  .objS_GetObjectCounts = &PreloadObSource_GetObjectCounts,
  .objS_GetObject = &PreloadObSource_GetObject,
  .objS_WriteBack = &PreloadObSource_WriteBack
};

void
PreloadObSource_Init(void)
{
  unsigned i;
  ObjectRange rng;

  // Set up the preloaded objects.
  struct NPObjectsDescriptor * npod = NPObDescr;	// local copy
  for (i = npod->numPreloadImages; i > 0; i--) {
    OID oid = npod->OIDBase;

    /* For the case of persistent objects (oid == FIRST_PERSISTENT_OID),
    using PreloadObSource is a temporary expedient until we get 
    paging working.
    When changing this, make sure zero pages get initialized somehow. */

    /* code for initializing PreloadObSource */
    rng.start = oid;
    rng.end = oid + FrameToOID(npod->numFramesInRange);
    rng.source = &PreloadObSource;

    objC_AddRange(&rng);

    uint32_t thisFrames = 1 + npod->numFrames;  // including the header frame
    npod = (struct NPObjectsDescriptor *)
           ((char *)npod + thisFrames * EROS_PAGE_SIZE);
  }
}
