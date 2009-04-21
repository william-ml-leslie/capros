/*
 * Copyright (C) 1998, 1999, 2001, Jonathan S. Shapiro.
 * Copyright (C) 2007, 2008, 2009, Strawberry Development Group.
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

/* VCSK -- Virtual copy space keeper/Zero space keeper (they are
   actually the same!)

   When a fault occurs on a frozen VCS, the fault is simply punted to
   the domain keeper (by returning RC_capros_key_RequestError, which
   causes a process fault).

   When a fault occurs on a non-frozen VCS, the action taken depends
   on the fault type:

      Read Fault   --  space is expanded appropriately using GPT
                       capabilities into the primordial zero space.

      Write fault  --  all immutable GPTs on the path from root to
                       offset are replaced with mutable GPTs until
		       an immutable page is found.  A new page is
		       acquired from a space bank and the content of
		       the old page is copied into the new page.

The original segment to be copied must be an "original key".
An "original key" must be either
1. a read-only page key, or
2. A sensory (weak, nocall, and RO restrictions) non-opaque key to a GPT
   whose slots are all either
  (a) original keys with BLSS one smaller than this key (no levels skipped),
   or (b) void.

A key to a VCS is an opaque key to a GPT.
(The key may have other restrictions such as RO.)
The GPT has a keeper that is a start key to a process (one process per VCS).
The keyInfo of the start key is 0 if the segment is not frozen,
nonzero if it is frozen (not implemented yet). 

Slot 0 of the top-level GPT has a key to a subtree. No other slots are used.

A subtree key is either a page key or a non-opaque GPT key. 
Void keys may also be present?
A page key is either original,
  or RW having been copied from the original segment or the zero segment. 
A GPT key is either original,
  or non-sensory (no restrictions) having been copied
    from the original segment or the zero segment.

What about extending with zero pages?

The only l2v values used are those produced by the procedure BlssToL2v.
L2v goes down by EROS_NODE_LGSIZE at each level - no levels are skipped.

   You might think that this ought to be built using background
   windows -- I certainly did.  Unfortunately, the background window
   approach has an undesirable consequence -- the translation of pages
   that have NOT been copied requires traversing the entire background
   segment, a traversal that is a function of the tree depth.

   Norm points out that it is necessary to have a distinct VCSK process for
   each active kept segment, to avoid the situation in which a single
   VCSK might become inoperative due to blocking on a dismounted page.


   VCSK doesn't quite start up as you might expect.  Generally, it is
   the product of a factory, and returns a segment key as its
   "yield"  The special case is that the zero segment keeper (which is
   the very first, primordial keeper) does not do this, but
   initializes directly into "frozen" mode.  It recognizes this case
   by the fact that a pre-known constituent slot holds something other
   than void.
   (The above appears to be wrong.)
   */

#include <stddef.h>
#include <stdbool.h>
#include <eros/target.h>
#include <eros/Invoke.h>
#include <idl/capros/Page.h>
#include <idl/capros/GPT.h>
#include <eros/StdKeyType.h>
#include <eros/cap-instr.h>

#include <idl/capros/key.h>
#include <idl/capros/SpaceBank.h>
#include <idl/capros/Node.h>
#include <idl/capros/Process.h>

#include <domain/Runtime.h>
#include <domain/domdbg.h>
#include <domain/assert.h>
#include <domain/ProtoSpaceDS.h>

#include "constituents.h"

#define dbg_init    0x1
#define dbg_invalid 0x2
#define dbg_access  0x4
#define dbg_op      0x8
#define dbg_destroy 0x10
#define dbg_returns 0x20

/* Following should be an OR of some of the above */
#define dbg_flags   ( 0u )

#define DEBUG(x) if (dbg_##x & dbg_flags)

#define KR_INITSEG  KR_APP(0)	/* used in primordia init */
#define KR_SCRATCH  KR_APP(1)
#define KR_SCRATCH2 KR_APP(2)

#define KR_ZINDEX   KR_APP(3)	/* index of primordial zero space */ 
#define KR_OSTREAM  KR_APP(4)
#define KR_L1_NODE  KR_APP(5)		/* last L1 node we COW'd a page into */
#define KR_PG_CACHE KR_APP(6)		/* really KR_APP(6, 7, 8) */

#define KR_SEGMENT KR_ARG(0)	// a non-opaque key to the segment

/* POLICY NOTE:

   The original design was intended to grow by one page when
   zero-extending the segment.  Newer operating systems frequently
   grow by more than this.  The following tunable may be set to
   anything in the range 1..EROS_NODE_SIZE to control how many pages
   the segment grows by. Note that this could (and should) be made
   into a user-controllable variable -- I just haven't bothered
   yet. */

#define ZERO_EXTEND_BY 2

/* Our start key is never invoked directly, because we never give it out.

   If this was a pass-through invocation via a red space key, these
   hold what the user passed except that KR_SEGMENT has been replaced
   by a non-opaque key to the segment GPT.

   If this was a kernel-generated fault invocation,
   KR_SEGMENT holds a non-opaque key to the segment GPT,
   and KR_RETURN holds a fault key. */

/* IMPORTANT optimization:

   VCSK serves to implement both demand-copy and demand-zero
   segments.  Of the cycles spent invoking capabilities, it proves
   that about 45% of them are spent in capros_Page_clone, and another 45% in
   range key calls (done by the space bank).  The only call to
   capros_Page_clone is here.  It is unavoidable when we are actually doing a
   virtual copy, but very much avoidable if we are doing demand-zero
   extension on an empty or short segment -- the page we get from the
   space bank is already zeroed.

   We therefore remember in the VCSK state the offset of the *end* of
   the last copied or original page.  Anything past this is known to be zero.
   We take advantage of this to know when the capros_Page_clone() operation
   can be skipped.  The variable is copied_limit.

   When a VCS is frozen, we stash this number in a number key in the
   red segment node, and reload it from there when the factory creates
   a new child node.

   At the moment I have not implemented this, because the VCSK code is
   also trying to be useful in providing demand extension of existing
   segments. There are a couple of initialization cases to worry about
   -- all straightforward -- but I haven't dealt with those yet. None
   of these are issues in the performance path.  */

typedef struct {
  bool was_access;
  uint64_t last_offset;		/* last offset we modified */

	/* No pages at or above the offset in copied_limit are either
	copied or original. */
  uint64_t copied_limit;
  int frozen;
  uint32_t npage;
} state;

void Sepuku(result_t);
void DestroySegment(state *);

static unsigned int
L2vToBlss(unsigned int l2v)
{
  return (l2v - EROS_PAGE_ADDR_BITS) / EROS_NODE_LGSIZE + 1 + EROS_PAGE_BLSS;
}

static unsigned int
BlssToL2v(unsigned int blss)
{
  // assert(blss > 0);
  return (blss -1 - EROS_PAGE_BLSS) * EROS_NODE_LGSIZE + EROS_PAGE_ADDR_BITS;
}

uint32_t
BiasedLSS(uint64_t offset)
{
  /* Shouldn't this be using fcs()? */
  uint32_t bits = 0;
  uint32_t w0 = (uint32_t) offset;
  
  static unsigned hexbits[16] = {0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4 };

  /* Run a decision tree: */
  if (offset >= 0x100000000ull) {
    bits += 32;
    w0 = (offset >> 32);
  }
	
  if (w0 >= 0x10000ull) {
    bits += 16;
    w0 >>= 16;
  }
  if (w0 >= 0x100u) {
    bits += 8;
    w0 >>= 8;
  }
  if (w0 >= 0x10u) {
    bits += 4;
    w0 >>= 4;
  }

  /* Table lookup for the last part: */
  bits += hexbits[w0];
  
  if (bits < EROS_PAGE_ADDR_BITS)
    return EROS_PAGE_BLSS;

  bits -= EROS_PAGE_ADDR_BITS;

  bits += (EROS_NODE_LGSIZE - 1);
  bits /= EROS_NODE_LGSIZE;
  bits += EROS_PAGE_BLSS;
  
  return bits;
}
  
uint64_t
BlssMask(uint32_t blss)
{
  if (blss <= EROS_PAGE_BLSS)
    return 0ull;

  uint32_t bits_to_shift = BlssToL2v(blss);

  if (bits_to_shift >= UINT64_BITS)
    return -1ull;	/* all 1's */
  else return (1ull << bits_to_shift) -1ull;
}

uint32_t
BlssSlotNdx(uint64_t offset, uint32_t blss)
{
  if (blss <= EROS_PAGE_BLSS)
    return 0;

  uint32_t bits_to_shift = BlssToL2v(blss);
  if (bits_to_shift >= UINT64_BITS)
    return 0;

  return offset >> bits_to_shift;
}

enum {
  type_GPT,
  type_Page,
  type_Unknown,
} GetKeyType(cap_t cap)
{
  capros_key_type kt;
  result_t result = capros_key_getType(cap, &kt);
  if (result == RC_OK) {
    if (kt == IKT_capros_GPT)
      return type_GPT;
    if (kt == IKT_capros_Page)
      return type_Page;
  }
  return type_Unknown;
}

// krMem must be a GPT.
uint8_t
GetGPTBlss(uint32_t krMem)
{
  uint8_t l2v;
  uint32_t result;

  result  = capros_GPT_getL2v(krMem, &l2v);
  if (result != RC_OK) {
    kprintf(KR_OSTREAM, "Error geting GPT l2v!\n");
    return EROS_PAGE_BLSS;
  }
  return L2vToBlss(l2v);
}

uint8_t
GetBlss(cap_t krMem)
{
  if (GetKeyType(krMem) != type_GPT)
    return EROS_PAGE_BLSS;	// assume it is a page key
  return GetGPTBlss(krMem);
}

bool
CapIsReadOnly(cap_t cap)
{
  uint32_t perms;
  result_t result = capros_Memory_getRestrictions(cap, &perms);
  return (result != RC_OK || (perms & capros_Memory_readOnly));
}

cap_t
AllocPage(state * pState, result_t * resultp)
{
  result_t result;
#if 1
  if (pState->npage == 0) {
    result = capros_SpaceBank_alloc3(KR_BANK,
          capros_Range_otPage
          | ((capros_Range_otPage | (capros_Range_otPage << 8)) << 8),
			            KR_PG_CACHE + 0,
			            KR_PG_CACHE + 1,
			            KR_PG_CACHE + 2);
    if (result == RC_OK) {
      pState->npage = 3;
    } else {
      // We could try allocating 2 pages here,
      // but that does not seem like a worthwhile optimization,
      // since there is a good chance it won't succeed.
      result = capros_SpaceBank_alloc1(KR_BANK,
                                       capros_Range_otPage,
  			               KR_PG_CACHE + 0 );
      if (result == RC_OK) {
        pState->npage = 1;
      } else {
        DEBUG(returns) kprintf(KR_OSTREAM, "AllocPage failed\n");
        *resultp = result;
        return KR_VOID;
      }
    }
  }
  return KR_PG_CACHE + --pState->npage;
#else
  result = capros_SpaceBank_alloc1(KR_BANK, capros_Range_otPage, KR_TEMP0);
  if (result == RC_OK)
    return KR_TEMP0;
  else {
    DEBUG(returns) kprintf(KR_OSTREAM, "AllocPage failed\n");
    *resultp = result;
    return KR_VOID;
  }
#endif
}

void
InsertNewPage(state * pState, uint64_t orig_offset)
{
  pState->was_access = true;
  pState->last_offset = orig_offset;
  
  /* We assume at this point that the page in question no longer
     contains zeros, so the first zero offset must be above this
     point. */
  if (orig_offset >= pState->copied_limit)
    pState->copied_limit = orig_offset + EROS_PAGE_SIZE;
}

uint32_t
HandleSegmentFault(Message *pMsg, state *pState)
{
  uint32_t slot = 0;
  uint64_t offset = ((uint64_t) pMsg->rcv_w3) << 32
                    | (uint64_t) (pMsg->rcv_w2 & - EROS_PAGE_SIZE);
  uint32_t offsetBlss = BiasedLSS(offset);

  switch (pMsg->rcv_w1) {
  case capros_Process_FC_InvalidAddr:
    {
      /* capros_Process_FC_InvalidAddr does not copy any pages.
	 It therefore cannot alter the value of copied_limit.  */
      
      result_t result;
      
      pState->was_access = false;
      
      DEBUG(invalid)
	kdprintf(KR_OSTREAM, "FC_SegInvalidAddr at %#llx\n", offset);
      
      /* fetch out the subseg key */
      capros_GPT_getSlot(KR_SEGMENT, 0, KR_SCRATCH);

      /* find out its BLSS: */
      uint8_t subsegBlss = GetBlss(KR_SCRATCH);
      uint32_t segBlss = subsegBlss + 1;
      
      DEBUG(invalid) kdprintf(KR_OSTREAM, "FC_SegInvalidAddr: segBlss %d offsetBlss %d\n", segBlss, offsetBlss);

      if (subsegBlss < offsetBlss) {
        // Need to make the tree taller.
	while (subsegBlss < offsetBlss) {
	  DEBUG(invalid) kdprintf(KR_OSTREAM, "  Growing: subsegblss %d offsetblss %d\n",
				  subsegBlss, offsetBlss);
      
	  /* Buy a new GPT to expand with: */
          result = capros_SpaceBank_alloc1(KR_BANK,
                                           capros_Range_otGPT, KR_TEMP0);
	  if (result != RC_OK)
	    return result;
      
	  /* Make that node have BLSS == subsegBlss+1: */
          capros_GPT_setL2v(KR_TEMP0, BlssToL2v(subsegBlss+1));

	  /* Insert the old subseg into slot 0: */
	  capros_GPT_setSlot(KR_TEMP0, 0, KR_SCRATCH);

#if 1
	  /* Populate slots 1 and higher with suitable primordial zero
	     subsegments. */
          /* This isn't necessary; leaving the slots void will result in
          faulting them to zero as they are referenced.
          However, it's probably faster to populate them now. */
          capros_Node_getSlot(KR_ZINDEX, subsegBlss, KR_SCRATCH);
	  {	 
	    int i;
	    for (i = 1; i < EROS_NODE_SIZE; i++)
	      capros_GPT_setSlot(KR_TEMP0, i, KR_SCRATCH);
	  }
#endif

	  COPY_KEYREG(KR_TEMP0, KR_SCRATCH);

	  subsegBlss++;
	}

	/* Finally, insert the new subseg into the original GPT */
	capros_GPT_setSlot(KR_SEGMENT, 0, KR_SCRATCH);

        /* Note: there is a window between setting slot 0 of the
           original GPT, and setting its blss. That's not a problem;
           you just won't be able to access the taller portion until
           the blss is set. (The taller portion is void anyway.) */

	segBlss = subsegBlss + 1;
        capros_GPT_setL2v(KR_SEGMENT, BlssToL2v(segBlss));

        /* This might have been only a read reference, in which case
        populating with primordial zeroes was enough. 
        If it is a write (which is likely, actually),
        the client will fault again with capros_Process_FC_AccessViolation.
        We mustn't fall through to the code below, because it expects
        to find an invalid key. */
        DEBUG(returns)
          kdprintf(KR_OSTREAM, 
                   "Returning from capros_Process_FC_InvalidAddr after growing"
                   " managed segment to blss=%d\n", subsegBlss);
        return RC_OK;
      }

      /* Segment is now tall enough, but some portion was unpopulated. */

      DEBUG(invalid) kprintf(KR_OSTREAM,
        "Invalid address -- falling through to COW logic for znode\n"
        "  traverse slot 0, blss %d, offset %#llx\n",
			     0, segBlss, offset);

      /* Traverse downward to the invalid key.  No need to check
	 writability.  Had that been the problem we would have gotten
	 an access violation instead.

	 This logic will work fine as long as the whole segment tree
	 is non-opaque GPTs and pages, but will fail miserably if
         an opaque subsegment is plugged in here somewhere. */

      while (1) {
        switch (GetKeyType(KR_SCRATCH)) {
        case type_Page:
          // Since we got an Invalid fault, we shouldn't succeed in walking
          // the tree all the way to a page.
          assert(false);

        case type_GPT:
          assert(GetGPTBlss(KR_SCRATCH) == subsegBlss);
          COPY_KEYREG(KR_SCRATCH, KR_SEGMENT);
          slot = BlssSlotNdx(offset, subsegBlss);
          offset &= BlssMask(subsegBlss);

          DEBUG(invalid) kprintf(KR_OSTREAM,
          "  traverse slot %d, blss %d, offset %#llx\n",
			       slot, subsegBlss, offset);

          subsegBlss--;
          capros_GPT_getSlot(KR_SEGMENT, slot, KR_SCRATCH);
          continue;

        case type_Unknown:	// should be Void
          break;
        }
        break;
      }

      /* Key in KR_SCRATCH is the invalid key.  Key in
	 KR_SEGMENT is its parent. */

      /* Replace the offending subsegment with a primordial zero segment of
	   suitable size: */
      capros_Node_getSlot(KR_ZINDEX, subsegBlss, KR_SCRATCH);
	  
      capros_GPT_setSlot(KR_SEGMENT, slot, KR_SCRATCH);

      DEBUG(returns)
	kdprintf(KR_OSTREAM,
		 "Returning from capros_Process_FC_InvalidAddr after populating"
		 " zero subsegment\n");
      return RC_OK;
    }
    
  case capros_Process_FC_AccessViolation:
    {
      uint32_t segBlss = EROS_PAGE_BLSS + 1; /* until otherwise proven */
      /* Subseg is read-only */

      uint8_t subsegBlss;
      result_t result;
      uint64_t orig_offset = offset;
      bool needTraverse = true;

      DEBUG(access)
	kprintf(KR_OSTREAM, "capros_Process_FC_AccessViolation at 0x%llx\n",
		 offset);
      
#if KR_L1_NODE != KR_SEGMENT
      /* If the last thing we got hit with was an access fault, and
	 the present fault would land within the same L1 node, there
	 is no need to re-execute the traversal; simply reuse the
	 result from the last traversal:

	 It is fairly obvious that this is safe if the access pattern
	 is of the form

	     0b=========00001xxxx
	     0b=========00002xxxx

	 It is also safe if the access pattern was:
	 
	     ???

	 Because the second fault will result in an invalid address
	 before generating the access violation, which will cause the
	 segment to grow and invalidate the walk cache.

	 The tricky one is if the pattern is the other way:

	     0b=========00001xxxx
	     0b=========00000xxxx

         In this case, we will have done tree expansion in the first
	 path and have cached the right information.
	 
	 This hack only possible with 32-slot nodes. (Why??) */
      
      if (pState->was_access) {
	uint64_t xoff = offset ^ pState->last_offset;
	xoff >>= (EROS_PAGE_ADDR_BITS + EROS_NODE_LGSIZE);
	if (xoff == 0) {
	  needTraverse = false;
	  slot = (offset >> EROS_PAGE_ADDR_BITS) & EROS_NODE_SLOT_MASK;
	  subsegBlss = 2;
	  capros_GPT_getSlot(KR_L1_NODE, slot, KR_SCRATCH);

	  DEBUG(access)
	    kprintf(KR_OSTREAM, "Re-traverse suppressed at slot %d!\n", slot);
	}
      }
#endif
      
      if (needTraverse) {
	pState->was_access = false; /* we COULD fail! */
	
	/* fetch out the subseg key */
	capros_GPT_getSlot(KR_SEGMENT, slot, KR_SCRATCH);

	/* find out its BLSS: */
        subsegBlss = GetBlss(KR_SCRATCH);
	segBlss = subsegBlss + 1;
      
	DEBUG(access) kprintf(KR_OSTREAM,
                        "  traverse slot %d, blss %d, offset 0x%llx\n",
			slot, segBlss, offset);

	while (subsegBlss > EROS_PAGE_BLSS) {
	  DEBUG(access) kprintf(KR_OSTREAM, "  Walking down: subsegBlss %d\n",
				 subsegBlss);

          assert(GetKeyType(KR_SCRATCH) == type_GPT);
          if (CapIsReadOnly(KR_SCRATCH)) {
	    DEBUG(access) kprintf(KR_OSTREAM, "  subsegBlss %d unwritable.\n",
				   subsegBlss);
	    break;
	  }

          // Traverse downward past this writable GPT.
	  COPY_KEYREG(KR_SCRATCH, KR_SEGMENT);
	  slot = BlssSlotNdx(offset, subsegBlss);
	  offset &= BlssMask(subsegBlss);

	  DEBUG(access) kprintf(KR_OSTREAM,
                          "  traverse slot %d, blss %d, offset 0x%llx\n",
			  slot, subsegBlss, offset);

	  subsegBlss--;
	  capros_GPT_getSlot(KR_SEGMENT, slot, KR_SCRATCH);
	}

        assert(subsegBlss > EROS_PAGE_BLSS
               || (GetKeyType(KR_SCRATCH) == type_Page
                   && CapIsReadOnly(KR_SCRATCH) ) );
      
	/* Have now hit a read-only subtree, which we need to traverse,
	   turning the R/O GPTs into R/W GPTs. */

	while (subsegBlss > EROS_PAGE_BLSS) {
	  DEBUG(access) kprintf(KR_OSTREAM,
                          "  Walking down: COW subsegBlss %d\n", subsegBlss);

	  /* Buy a new read-write GPT: */
          result = capros_SpaceBank_alloc1(KR_BANK,
                                           capros_Range_otGPT, KR_TEMP0);
	  if (result != RC_OK)
	    return result;
      
	  /* Copy the slots and l2v from the old subseg into the new: */
	  capros_GPT_clone(KR_TEMP0, KR_SCRATCH);

	  /* Replace the old subseg with the new */
	  capros_GPT_setSlot(KR_SEGMENT, slot, KR_TEMP0);

	  COPY_KEYREG(KR_TEMP0, KR_SEGMENT);

	  /* Now traverse downward in the tree: */
	  slot = BlssSlotNdx(offset, subsegBlss);
	  offset &= BlssMask(subsegBlss);

	  DEBUG(access) kprintf(KR_OSTREAM,
                          "  traverse slot %d, blss %d, offset 0x%llx\n",
			  slot, subsegBlss, offset);

	  subsegBlss--;
	  capros_GPT_getSlot(KR_SEGMENT, slot, KR_SCRATCH);
	}

#if KR_L1_NODE != KR_SEGMENT
	/* KR_SEGMENT now points to the parent of the page key we are
	   COWing. If we are doing the L1 node memorization, record the
	   last manipulated L1 node here. */
	COPY_KEYREG(KR_SEGMENT, KR_L1_NODE);
#endif
      }

      /* Now KR_SCRATCH names a page and KR_L1_NODE is its containing
	 node, and KR_L1_NODE[slot] == KR_SCRATCH. */

      DEBUG(access) kprintf(KR_OSTREAM, "  Walking down: COW"
			     " orig_offset %#llx copied limit %#llx\n",
			     orig_offset, pState->copied_limit);
      
      if (orig_offset >= pState->copied_limit) {
	uint32_t count;
	
	/* Insert new zero pages at this location. As an optimization,
	   insert ZERO_EXTEND_BY pages at a time. */
	for (count = 0;
             (slot < EROS_NODE_SIZE) && (count < ZERO_EXTEND_BY);
             count++, slot++, orig_offset += EROS_PAGE_SIZE) {
	  cap_t kr = AllocPage(pState, &result);

	  if (kr == KR_VOID) {
	    if (count)
	      break;
	    else
	      return result;	// failed to get any pages
	  }

	  capros_GPT_setSlot(KR_L1_NODE, slot, kr);
          InsertNewPage(pState, orig_offset);
	}
	DEBUG(returns)
	  kdprintf(KR_OSTREAM,
		   "Returning from capros_Process_FC_AccessViolation\n"
                   " after appending %d empty pages to %#llx\n",
                   count, orig_offset);
      } else {
	cap_t kr = AllocPage(pState, &result);
	if (kr == KR_VOID)
	  return result;

	capros_Page_clone(kr, KR_SCRATCH);

	/* Replace the old page with the new */
	capros_GPT_setSlot(KR_L1_NODE, slot, kr);
	InsertNewPage(pState, orig_offset);

	DEBUG(returns)
	  kdprintf(KR_OSTREAM,
		   "Returning from capros_Process_FC_AccessViolation after duplicating"
		   " page at %#llx\n", orig_offset);

      }
      return RC_OK;
    }

  default:
    return pMsg->rcv_w1;	/* fault code */
  }
}

static int
ProcessRequest(Message *argmsg, state *pState)
{
  uint32_t result = RC_OK;
  
  switch(argmsg->rcv_code) {
  case OC_capros_key_getType:			/* check alleged keytype */
    {
      argmsg->snd_w1 = AKT_VcskSeg;
      break;
    }

  case OC_capros_Memory_fault:
    if (pState->frozen) {
      result = RC_capros_key_RequestError;
      break;
    }
    
    result = HandleSegmentFault(argmsg, pState);
    break;

  case OC_capros_Memory_reduce:
    {
      capros_Memory_reduce(KR_SEGMENT, capros_Memory_opaque | argmsg->rcv_w1,
                           KR_SEGMENT);
      argmsg->snd_key0 = KR_SEGMENT;
      result = RC_OK;
      break;
    }

#if 0
  case OC_Vcsk_Truncate:
  case OC_Vcsk_Pack:
    if (pState->frozen) {
      result = RC_capros_key_RequestError;
      break;
    }
#endif
    
  case OC_capros_key_destroy:
    {
      DestroySegment(pState);

      Sepuku(RC_OK);
      break;
    }
    
  default:
    result = RC_capros_key_UnknownRequest;
    break;
  }
  
  argmsg->snd_code = result;
  return 1;
}

void
Sepuku(result_t retCode)
{
  capros_Node_getSlot(KR_CONSTIT, KC_PROTOSPC, KR_TEMP0);

  /* Invoke the protospace to destroy us and return. */
  protospace_destroy_small(KR_TEMP0, retCode);
}

/* Tree destruction is a little tricky.  We know the height of the
   tree, but we have no simple way to build a stack of node pointers
   as we traverse it.

   The following algorithm is pathetically brute force.
   */
int
ReturnWritableSubtree(uint32_t krTree)
{
  uint32_t perms = 0;
  uint32_t kt;
  uint32_t result = capros_key_getType(krTree, &kt);
  
  for (;;) {
    if (result != RC_OK)	// must be void
      /* Segment has been fully demolished. */
      return 0;

    if (kt == IKT_capros_Page || kt == IKT_capros_GPT) {
      if (CapIsReadOnly(krTree))
        return 0;
    } else {
      if (perms & capros_Memory_readOnly)
        /* This case can occur if the entire segment is both tall and
           unmodified. */
        return 0;
    }

    if (kt == IKT_capros_Page) {
      capros_SpaceBank_free1(KR_BANK, krTree);
      return 1;			/* more to do */
    }
    else if (kt == IKT_capros_GPT) {
      int i;
      for (i = 0; i < EROS_NODE_SIZE; i++) {
	uint32_t sub_kt;
	uint32_t result;
	uint32_t subPerms = 0;

	capros_GPT_getSlot(krTree, i, KR_SCRATCH2);
      
	result = capros_key_getType(KR_SCRATCH2, &sub_kt);

        // FIXME: this is clearly wrong!
	if (kt == IKT_capros_Page || kt == IKT_capros_GPT)
          result = capros_Memory_getRestrictions(krTree, &subPerms);

	if ((subPerms & capros_Memory_readOnly) == 0) {
	  /* do nothing */
	}
	else if (sub_kt == IKT_capros_Page) {
	  capros_SpaceBank_free1(KR_BANK, KR_SCRATCH2);
	}
	else if (sub_kt == IKT_capros_GPT) {
	  COPY_KEYREG(KR_SCRATCH2, KR_SCRATCH);
	  kt = sub_kt;
	  break;
	}
      }

      if (i == EROS_NODE_SIZE) {
	/* If we get here, this node has no children.  Return it, but
	   also return 1 because it may have a parent node. */
	capros_SpaceBank_free1(KR_BANK, KR_SCRATCH);
	return 1;
      }
    }
    else {
      kdprintf(KR_OSTREAM, "Returning a key with kt=0x%x\n", kt);
    }
  }
}

void
DestroySegment(state *mystate)
{
  for(;;) {
    capros_GPT_getSlot(KR_SEGMENT, 0, KR_SCRATCH);

    if (!ReturnWritableSubtree(KR_SCRATCH))
      break;
  }

  DEBUG(destroy) kdprintf(KR_OSTREAM, "Destroying red seg root\n");
  capros_SpaceBank_free1(KR_BANK, KR_SEGMENT);
}

int
main(void)
{
  Message msg;

  state Mystate, * mystate = &Mystate;

  result_t result;
  
  mystate->was_access = false;
  mystate->npage = 0;
  
  capros_Node_getSlot(KR_CONSTIT, KC_OSTREAM, KR_OSTREAM);
  capros_Node_getSlot(KR_CONSTIT, KC_ZINDEX, KR_ZINDEX);
  capros_Node_getSlot(KR_CONSTIT, KC_FROZEN_SEG, KR_TEMP2);

  DEBUG(init) kdprintf(KR_OSTREAM, "Fetch BLSS of frozen seg\n");
  /* find out BLSS of frozen segment: */
  /* FIXME: need to validate KR_TEMP2 - it must be an "original key". */
  uint8_t segBlss = GetBlss(KR_TEMP2);

  DEBUG(init) kdprintf(KR_OSTREAM, "BLSS of frozen seg was %d\n", segBlss);

  segBlss += 1;
  
  mystate->copied_limit = 1ull << BlssToL2v(segBlss);
    
  DEBUG(init) kdprintf(KR_OSTREAM, "Buy new GPT\n");
  
  result = capros_SpaceBank_alloc1(KR_BANK, capros_Range_otGPT, KR_SEGMENT);
  if (result != RC_OK)	// failed to allocate a GPT
    Sepuku(result);	// return failure to caller

  DEBUG(init) kdprintf(KR_OSTREAM, "Initialize it\n");

  result = capros_GPT_setL2v(KR_SEGMENT, BlssToL2v(segBlss));
  assert(result == RC_OK);

  /* write the immutable seg to slot 0 */
  result = capros_GPT_setSlot(KR_SEGMENT, 0, KR_TEMP2);

  /* Make a start key to ourself and set as keeper. */
  capros_Process_makeStartKey(KR_SELF, 0, KR_TEMP0);
  result = capros_GPT_setKeeper(KR_SEGMENT, KR_TEMP0);

  result = capros_Memory_reduce(KR_SEGMENT, capros_Memory_opaque, KR_TEMP0);

  DEBUG(init) kdprintf(KR_OSTREAM, "Initialized VCSK\n");
    
  msg.snd_invKey = KR_RETURN;
  msg.snd_key0 = KR_TEMP0;	/* first return: seg key */
  msg.snd_key1 = KR_VOID;
  msg.snd_key2 = KR_VOID;
  msg.snd_rsmkey = KR_VOID;
  msg.snd_data = 0;
  msg.snd_len = 0;
  msg.snd_code = 0;
  msg.snd_w1 = 0;
  msg.snd_w2 = 0;
  msg.snd_w3 = 0;

  msg.rcv_key0 = KR_SEGMENT;
  msg.rcv_key1 = KR_VOID;
  msg.rcv_key2 = KR_VOID;
  msg.rcv_rsmkey = KR_RETURN;
  msg.rcv_limit = 0;
  msg.rcv_code = 0;
  msg.rcv_w1 = 0;
  msg.rcv_w2 = 0;
  msg.rcv_w3 = 0;

  do {
    RETURN(&msg);
    mystate->frozen = msg.rcv_keyInfo;
    msg.snd_key0 = KR_VOID;	/* until otherwise proven */
  } while ( ProcessRequest(&msg, mystate) );

  return 0;
}
