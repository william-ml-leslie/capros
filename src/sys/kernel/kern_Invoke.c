/*
 * Copyright (C) 1998, 1999, 2001, Jonathan S. Shapiro.
 * Copyright (C) 2005, 2006, 2007, Strawberry Development Group.
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

/* Driver for key invocation */

#include <string.h>
#include <kerninc/kernel.h>
#include <kerninc/Activity.h>
#include <kerninc/Debug.h>
#include <kerninc/Check.h>
#include <kerninc/Invocation.h>
#include <kerninc/IRQ.h>
#include <arch-kerninc/Process-inline.h>
#ifdef OPTION_DDB
#include <eros/StdKeyType.h>
#endif

#include <kerninc/Machine.h>
#include <kerninc/KernStats.h>
#include <eros/KeyConst.h>	// segment and wrapper defs

#include <disk/Forwarder.h>
#include <idl/capros/Forwarder.h>

/* #define GATEDEBUG 5
 * #define KPRDEBUG
 */

#ifdef GATEDEBUG
#include <eros/ProcessKey.h>
#endif

/* #define TESTING_INVOCATION */

#ifdef TESTING_INVOCATION
const dbg_wild_ptr = 1;
#endif

#ifdef OPTION_DDB
uint32_t ddb_inv_flags = 0;
#define DDB_STOP(x) (ddb_inv_flags & DDB_INV_##x)
#else
#define DDB_STOP(x) 0
#endif

#define __EROS_PRIMARY_KEYDEF(x, isValid, bindTo) void bindTo##Key (Invocation* msg);
#include <eros/StdKeyType.h>

Activity * activityToRelease = 0;

#if 0
static void
UnknownKey(Invocation* inv /*@ not null @*/)
{
  key_Print(inv->key);
  fatal("Invocation of unknown primary key type 0x%x\n",
        keyBits_GetType(inv->key));
}
#endif

void
UnimplementedKey(Invocation* inv /*@ not null @*/)
{
  fatal("Invocation of unimplemented primary key type 0x%x\n",
        keyBits_GetType(inv->key));
}

#ifndef OPTION_KBD
#define KeyboardKey VoidKey
#endif

INLINE void 
proc_KeyDispatch(Invocation *pInv)
{
  switch(inv.invKeyType) {
#define __EROS_PRIMARY_KEYDEF(kn, isValid, bindTo) case KKT_##kn: bindTo##Key(pInv); break;
#include <eros/StdKeyType.h>
    default:
      UnimplementedKey(pInv);
      break;
  }
}

#ifdef OPTION_KERN_TIMING_STATS
// The +1 below is for the internal type IT_KeeperCall.
uint64_t inv_KeyHandlerCycles[PRIMARY_KEY_TYPES][IT_NUM_INVTYPES+1];
uint64_t inv_KeyHandlerCounts[PRIMARY_KEY_TYPES][IT_NUM_INVTYPES+1];

void 
inv_ZeroStats()
{
  for (int i = 0; i < PRIMARY_KEY_TYPES; i++) {
    inv_KeyHandlerCycles[i][IT_Return] = 0;
    inv_KeyHandlerCycles[i][IT_PReturn] = 0;
    inv_KeyHandlerCycles[i][IT_Call] = 0;
    inv_KeyHandlerCycles[i][IT_PCall] = 0;
    inv_KeyHandlerCycles[i][IT_Send] = 0;
    inv_KeyHandlerCycles[i][IT_PSend] = 0;
    inv_KeyHandlerCycles[i][IT_KeeperCall] = 0;
    inv_KeyHandlerCounts[i][IT_Return] = 0;
    inv_KeyHandlerCounts[i][IT_PReturn] = 0;
    inv_KeyHandlerCounts[i][IT_Call] = 0;
    inv_KeyHandlerCounts[i][IT_PCall] = 0;
    inv_KeyHandlerCounts[i][IT_Send] = 0;
    inv_KeyHandlerCounts[i][IT_PSend] = 0;
    inv_KeyHandlerCounts[i][IT_KeeperCall] = 0;
  }
}
#endif

void 
inv_BootInit()
{
#if defined(OPTION_KERN_TIMING_STATS)
  inv_ZeroStats();
#endif
}

/* KEY INVOCATION
 * 
 * Logic in Principle: Entry, Action, Exit
 * 
 * Entry: 
 * 
 *    1. Validate that the invocation is well-formed, and that the
 *       data argument if any is present and validly mapped.  
 * 
 *    2. Copy all of the information associated with the
 *       invocation to a place of safety against the possibility that
 *       entry and exit arguments overlap.
 * 
 *    3. Determine who we will be exiting to (the invokee) -- this is
 *       often the invoker.  It can also be the domain named by the
 *       invoked gate key.  It can also be the domain named by the key
 *       in slot 4 if this is a kernel key (YUCK).  It can also be
 *       NULL if the key in slot 4 proves not to be a resume key.
 * 
 *       Invoked      Invocation    Slot 4     Invokee
 *       Key Type     Type          Key Type
 * 
 *       Gate Key     ANY           *          Named by gate key
 *       Kernel Key   CALL          *          Same as invoker
 *       Kernel Key   SEND,RETURN   Resume     Named by resume key
 *       Kernel Key   SEND,RETURN   !Resume    NONE
 * 
 *       Note that the case of CALL on a kernel key is in some sense
 *       the same as SEND/RETURN on a kernel key -- the resume key was
 *       generated by the call.  I may in fact implement the first
 *       version this way.
 * 
 *    4. Determine if the invokee will receive a resume key to the
 *       invoker.  (yes if the invocation is a CALL).
 * 
 *    5. Determine if a resume key will be consumed by the invocation
 *       (yes if the invokee was named by a resume key, including the
 *       case of CALL on kernel key).
 * 
 *    6. Determine if the invokee is in the proper state:
 * 
 *       Invoked      Invocation    Invokee
 *       Key Type     Type          State
 * 
 *       Start Key    ANY           AVAILABLE
 *       Resume Key   ANY           WAITING
 *       Kernel Key   CALL          RUNNING (really waiting per note above)
 *       Kernel Key   SEND,RETURN   WAITING
 * 
 *    7. Determine if a new thread is to be created.
 * 
 *    8. Construct a receive buffer mapping, splicing in the
 *       kernel-reserved void page where no mapping is present.
 * 
 *    NONE OF THE ABOVE CAN MODIFY THE PROGRAM as the action may put
 *    the program to sleep, causing us to need to restart the entire
 *    invocation.
 * 
 * Action:
 *    Whatever the key is specified to do.  This may require stalling
 *    the application.  The interesting case is the gate key
 *    implementation.  Given that the above actions have already been
 *    performed, the gate key action
 * 
 * Exit:
 *    If a resume key is to be copied
 *      If already consumed copy a null key
 *      else construct resume key
 *    Migrate the outbound thread to the invokee.
 */

Invocation inv;
  
#ifndef NDEBUG
bool InvocationCommitted = false;
#endif

/* May Yield. */
void 
inv_Commit(Invocation* thisPtr)
{
  inv_MaybeDecommit(thisPtr);
  
#ifndef NDEBUG
  InvocationCommitted = true;
#endif

  /* Revise the invoker runState. */
  Process * invoker = act_CurContext();

  switch (inv.invType) {
    // Note, prompt types have been converted to nonprompt by this time.
  case IT_Return:
    invoker->runState = RS_Available;
#if 0
    dprintf(false, "Wake up the waiters sleeping on dr=0x%08x\n", invoker);
#endif
    sq_WakeAll(&invoker->stallQ, false);
    break;

  case IT_Send:
    // invoker remains RS_Running
    break;

  case IT_Call:
  case IT_KeeperCall:
    invoker->runState = RS_Waiting;
    break;

  default: assert(false);
  }
}

bool 
inv_IsInvocationKey(Invocation* thisPtr, const Key* pKey)
{
  /* because this gets set up early in the keeper invocation
   * path, /inv.key/ may not be set yet, so check this early.
   */
  if (pKey == &thisPtr->redNodeKey)
    return true;
  
  if (thisPtr->key == 0)
    return false;
  
  if (pKey == thisPtr->key)
    return true;
  
  if (pKey == &thisPtr->scratchKey)
    return true;
  
  return false;
}

/* Some fields in Invocation are assumed to be initialized at the
   beginning of an invocation. Initialize them here, and also in inv_Cleanup. */
void
inv_InitInv(Invocation *thisPtr)
{
#ifndef NDEBUG
  InvocationCommitted = false;
#endif

  thisPtr->flags = 0;
}

void 
inv_Cleanup(Invocation* thisPtr)
{
  thisPtr->key = 0;
  
#ifndef NDEBUG
  InvocationCommitted = false;
#endif
  
  /* exit register values are set up in PopulateExitBlock, where they
   * are only done on one path.  Don't change them here.
   */
  
#ifndef NDEBUG
  /* Leaving the key pointers dangling causes no harm */
  thisPtr->entry.key[0] = 0;
  thisPtr->entry.key[1] = 0;
  thisPtr->entry.key[2] = 0;
  thisPtr->entry.key[3] = 0;
#endif

  /* NH_Unprepare is expensive.  Avoid it if possible: */
  if (thisPtr->flags & INV_SCRATCHKEY)
    key_NH_SetToVoid(&thisPtr->scratchKey);
  if (thisPtr->flags & INV_REDNODEKEY)
    key_NH_SetToVoid(&thisPtr->redNodeKey);
#if 0
  if (flags & INV_RESUMEKEY)
    resumeKey.NH_VoidKey();
#endif
#if 0
  if (thisPtr->flags & INV_EXITKEY0)
    exit.key[0].NH_VoidKey();
  if (flags & INV_EXITKEY3)
    exit.key[3].INH_VoidKey();
  if (flags & INV_EXITKEYOTHER) {
    exit.key[1].NH_VoidKey();
    exit.key[2].NH_VoidKey();
  }
#endif
  
  thisPtr->flags = 0;
}

/* Yields, does not return. */
void 
inv_RetryInvocation(Invocation* thisPtr)
{
  inv_Cleanup(thisPtr);
  act_CurContext()->runState = RS_Running;	// FIXME: still needed?

  act_Yield();
  /* Returning all the way to user mode and taking an invocation exception
  into the kernel again is very expensive on IA-32. 
  We could optimize out the change to and from user mode, if we can
  prove the process will just reexecute its invocation. 
  Don't forget to do what this path does, such as calling UpdateTLB. */
}

#ifndef NDEBUG
bool 
inv_IsCorrupted(Invocation* thisPtr)
{
  int i = 0;
  for (i = 0; i < 4; i++) {
    if (thisPtr->entry.key[i])
      return true;
#if 0
    if (exit.key[i].IsPrepared())
      return true;
#endif
  }
  return false;
}
#endif

/* Copy at most COUNT bytes out to the process.  If the process
 * receive length is less than COUNT, silently truncate the outgoing
 * bytes.  Rewrite the process receive count field to indicate the
 * number of bytes actually transferred.
 */
void
inv_CopyOut(Invocation* thisPtr, uint32_t len, void *data)
{
  assert(InvocationCommitted);
  // sentLen should have been set up by a call to proc_SetupExitString.
  assert(thisPtr->sentLen == len);

  if (thisPtr->exit.rcvLen < len)
    len = thisPtr->exit.rcvLen;	// take minimum

  if (len) {
    memcpy(thisPtr->exit.data, data, len);

#ifdef OPTION_KERN_STATS
    KernStats.bytesMoved += len;
#endif
  }
}

/* Copy at most COUNT bytes in from the process.  If the process
 * send length is less than COUNT, copy the number of bytes in the
 * send buffer.  Return the number of bytes transferred.
 */
uint32_t 
inv_CopyIn(Invocation* thisPtr, uint32_t len, void *data)
{
  assert(InvocationCommitted);
  
  if (thisPtr->entry.len < len)
    len = thisPtr->entry.len;
  
  if (thisPtr->entry.len)
    memcpy(data, thisPtr->entry.data, len);

  return len;
}

/* Fabricate a prepared resume key. */
void 
proc_BuildResumeKey(Process* thisPtr,	/* resume key to this process */
  Key* resumeKey /*@ not null @*/)	/* must be not hazarded */
{
#ifndef NDEBUG
  if (!keyR_IsValid(&thisPtr->keyRing, thisPtr))
    fatal("Key ring screwed up\n");
#endif

  key_NH_Unchain(resumeKey);
  
  keyBits_InitType(resumeKey, KKT_Resume);
  keyBits_SetPrepared(resumeKey);
  resumeKey->u.gk.pContext = thisPtr;
  link_Init(&resumeKey->u.gk.kr);
  link_insertBefore(&thisPtr->keyRing, &resumeKey->u.gk.kr);

#if 0
  printf("Dumping key ring (context=0x%08x, resumeKey=0x%08x):\n"
		 "kr.prev=0x%08x kr.next=0x%08x\n",
		 this, &resumeKey, kr.prev, kr.next);
  KeyRing      *pkr=kr.prev;
  int		count=0;
  while (pkr!=&kr)
    {
      printf("pkr->prev=0x%08x pkr->next=0x%08x\n",
		     pkr->prev, pkr->next);
      ((Key *)pkr)->Print();
      pkr=pkr->prev;
      count++;
    }
  if (count>1)
    dprintf(true, "Multiple keys in key ring\n");
#endif

#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Crafted resume key\n");
#endif
}

#ifdef __cplusplus
extern "C" {
  uint64_t rdtsc();
};
#else
extern uint64_t rdtsc();
#endif

#ifdef OPTION_KERN_TIMING_STATS
uint64_t top_time;
#ifdef OPTION_KERN_EVENT_TRACING
uint64_t top_cnt0;
uint64_t top_cnt1;
#endif
#endif

static void
BeginInvocation(void)
{
#ifdef OPTION_KERN_TIMING_STATS
  top_time = rdtsc();
#ifdef OPTION_KERN_EVENT_TRACING
  top_cnt0 = mach_ReadCounter(0);
  top_cnt1 = mach_ReadCounter(1);
#endif
#endif

  KernStats.nInvoke++;

#ifndef NDEBUG
  InvocationCommitted = false;
#endif
}

/* This is a carefully coded routine, probably less than clear.  The
 * version that handles all of the necessary cases is
 * DoGeneralKeyInvocation; this version is trying to cherry pick the
 * high flyers.
 * 
 * The common cases are, in approximate order:
 * 
 *  1. CALL   on a gate key
 *  2. RETURN on a gate key    (someday: via returner)
 *  3. CALL   on a kernel key
 *  4. Anything else
 * 
 * This version also assumes that the string arguments are valid, and
 * does only the fast version of the string test.
 */

/* May Yield. */
void 
proc_DoKeyInvocation(Process* thisPtr)
{
  BeginInvocation();
  
  objH_BeginTransaction();

  /* Roll back the invocation PC in case we need to restart this operation */
  proc_AdjustInvocationPC(thisPtr);

  inv.suppressXfer = false;
  
  proc_SetupEntryBlock(thisPtr, &inv);

  key_Prepare(inv.key);
#ifndef invKeyType
  inv.invKeyType = keyBits_GetType(inv.key);
#endif

#ifdef GATEDEBUG
  printf("Ivk proc=0x%08x ", thisPtr);
  if (thisPtr->procRoot &&
      keyBits_IsType(&thisPtr->procRoot->slot[ProcSymSpace], KKT_Number)) {
    void db_eros_print_number_as_string(Key* k);
    db_eros_print_number_as_string(&thisPtr->procRoot->slot[ProcSymSpace]);
  }
  printf(" invSlot=%d oc=%d\n",
         inv.key - &thisPtr->keyReg[0], inv.entry.code);
#endif

#ifdef OPTION_DDB
  if ( ddb_inv_flags )
    goto general_path;
#endif

  if (invType_IsPrompt(inv.invType) && inv.invKeyType != KKT_Resume)
    goto general_path;
  inv.invType &= ~IT_PReturn;	// clear low bit, convert to nonprompt type

  /* Send is a pain in the butt.  Fuck 'em if they can't take a joke. */
  if (inv.invType == IT_Send)
    goto general_path;

  /* If it's a segment key, it might have a keeper.  Take the long way: */
  if (inv.invKeyType == KKT_Segment || inv.invKeyType == KKT_GPT)
    goto general_path;

  /* If it's a wrapper key, take the long way: */
  if (inv.invKeyType == KKT_Wrapper || inv.invKeyType == KKT_Forwarder)
    goto general_path;

  if (keyBits_IsGateKey(inv.key)) {
    inv.invokee = inv.key->u.gk.pContext;
    if (proc_IsWellFormed(inv.invokee) == false)
      goto general_path;

    /* If this is a resume key, we know the process is in the
     * RS_Waiting state.  If it is a start key, the process might be
     * in any state.  Do not solve the problem here to avoid loss of
     * I-cache locality in this case; we are going to sleep anyway.
     */
    if (inv.invKeyType == KKT_Start && inv.invokee->runState != RS_Available)
      goto general_path;
  }
  else {
    if (inv.invType != IT_Call)
      goto general_path;
    
    inv.invokee = thisPtr;
  }

  if (proc_IsRunnable(inv.invokee) == false)
    goto general_path;

  /* PF_ExpectingMsg isn't significant while RS_Running, so it is OK
  to change it before the invocation is committed. */
  assert(inv.invType != IT_KeeperCall);
  thisPtr->processFlags |= PF_ExpectingMsg;
  
  /* It does not matter if the invokee fails to prepare!
   */
  proc_SetupExitBlock(inv.invokee, &inv);
#ifdef GATEDEBUG
  dprintf(true, "Fast path, set up exit block\n");
#endif

  {
#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
    if (dbg_wild_ptr)
      check_Consistency("DoKeyInvocation() before invoking handler\n");
#endif

#if defined(OPTION_KERN_TIMING_STATS)
    uint64_t pre_handler = rdtsc();
#endif
    proc_KeyDispatch(&inv);

#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "fast path, after key dispatch\n");
#endif
  
#if defined(OPTION_KERN_TIMING_STATS)
    {
      extern uint32_t inv_delta_reset;
      if (inv_delta_reset == 0) {
	extern uint64_t inv_handler_cy;
	uint64_t post_handler = rdtsc();
	inv_handler_cy += (post_handler - pre_handler);
	inv_KeyHandlerCycles[inv.invKeyType][inv.invType] +=
	  (post_handler - pre_handler);
	inv_KeyHandlerCounts[inv.invKeyType][inv.invType] ++;
      }
    }  
#endif

#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
    if (dbg_wild_ptr)
      check_Consistency("DoKeyInvocation() after invoking handler\n");
#endif

    assert (InvocationCommitted);

    assert(act_Current()->context == thisPtr);

#ifndef NDEBUG
    InvocationCommitted = false;    
#endif
  }

  /* Now for the tricky part.  It's possible that the proces did an
   * invocation whose effect was to blow the invokee apart.  I know of
   * no way to speed these checks:
   */

  if (proc_IsNotRunnable(inv.invokee)) {
    if (inv.invokee->procRoot == 0)
      goto invokee_died;

    proc_Prepare(inv.invokee);

    if (proc_IsNotRunnable(inv.invokee))
      goto invokee_died;
  }

  inv.invokee->runState = RS_Running;

  /* 
     If this was a kernel key invocation in the fast path, we never
     bothered to actually set them waiting, but they were logically in
     the waiting state nonetheless.

     This is the C fast path, so we don't need to worry about the SEND
     case.
  */
  if (proc_IsExpectingMsg(inv.invokee)) {
    if (!inv.suppressXfer) {
      proc_DeliverResult(inv.invokee, &inv);
    }
    // else this was done by GateKey

    proc_AdvancePostInvocationPC(inv.invokee);
  }
#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
  if (dbg_wild_ptr)
    check_Consistency("DoKeyInvocation() after DeliverResult()\n");
#endif

  if (inv.invokee != thisPtr) {
    /* Following is only safe because we handle non-call on primary
     * key in the general path.
     */
    if (inv.invKeyType == KKT_Resume)
      keyR_ZapResumeKeys(&inv.invokee->keyRing);

    act_MigrateTo(act_Current(), inv.invokee);
#ifdef OPTION_DDB
    /*if (inv.invokee->priority == pr_Never)*/
    if (inv.invokee->readyQ == &prioQueues[pr_Never])
      dprintf(true, "Thread now in ctxt 0x%08x w/ bad schedule\n", 
              inv.invokee);
#endif
  }
  
  inv_Cleanup(&inv);
  inv.invokee = 0;

#ifdef OPTION_KERN_TIMING_STATS
  {
    extern uint32_t inv_delta_reset;

    if (inv_delta_reset == 0) {
      extern uint64_t inv_delta_cy;

      uint64_t bot_time = rdtsc();
#ifdef OPTION_KERN_EVENT_TRACING
      extern uint64_t inv_delta_cnt0;
      extern uint64_t inv_delta_cnt1;

      uint64_t bot_cnt0 = mach_ReadCounter(0);
      uint64_t bot_cnt1 = mach_ReadCounter(1);
      inv_delta_cnt0 += (bot_cnt0 - top_cnt0);
      inv_delta_cnt1 += (bot_cnt1 - top_cnt1);
#endif
      inv_delta_cy += (bot_time - top_time);
    }
    else
      inv_delta_reset = 0;
  }
#endif

#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
  if (dbg_wild_ptr)
    check_Consistency("bottom DoKeyInvocation()");
#endif
  
  return;

 general_path:
#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "taking slow path\n");
#endif
  
  /* This path has its own, entirely separate recovery logic.... */
  proc_DoGeneralKeyInvocation(thisPtr);
  return;
  
 invokee_died:
#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "invokee died\n");
#endif
  
  inv.invokee = 0;
  inv_Cleanup(&inv);
  act_MigrateTo(act_Current(), 0);
}

/* On entry, inv entry block has been set up. */
/* May Yield. */
void
proc_DoGeneralKeyInvocation(Process* thisPtr)
{
  activityToRelease = 0;

  assert(keyBits_IsPrepared(inv.key));
  
  /* If this is a prompt invocation, it MUST be done on a resume key.
     If it isn't a resume key, treat it like a void key. */
  if (invType_IsPrompt(inv.invType)) {
    inv.invType &= ~IT_PReturn;	// clear low bit, convert to nonprompt type
    if (inv.invKeyType != KKT_Resume) {
      /* dprintf(true, "Prompt invocation on non-resume key!\n"); */
      inv.key = &key_VoidKey;
#ifndef invKeyType
      inv.invKeyType = KKT_Void;
#endif
    }
  }

  /* There are two cases where the actual invocation may proceed on a
   * key other than the invoked key:
   * 
   *   Invocation of kept red segment key proceeds as invocation on
   *     the keeper, AND observes the slot 2 convention of the format
   *     key!!!   Because this must overwrite slot 2, it must occur
   *     following the entry block preparation.
   * 
   *   Gate key to malformed domain proceeds as invocation on void.
   * 
   * The red seg test is done first because the extracted gate key (if
   * any) needs to pass the well-formed test too.
   */
  
  if (inv.invKeyType == KKT_Forwarder
      && (inv.key->keyData & capros_Forwarder_opaque) ) {
    Node * wrapperNode = (Node *) key_GetObjectPtr(inv.key);

    if (keyBits_IsGateKey(&wrapperNode->slot[ForwarderTargetSlot]) ) {
      if (wrapperNode->nodeData & ForwarderBlocked) {
	act_SleepOn(ObjectStallQueueFromObHdr(&wrapperNode->node_ObjHdr));
	act_Yield();
      }

      if (inv.key->keyData & capros_Forwarder_sendCap) {
	/* Not hazarded because invocation key */
	key_NH_Set(&inv.scratchKey, inv.key);
	keyBits_SetType(&inv.scratchKey, KKT_Forwarder);
        inv.scratchKey.keyData = 0;	// not capros_Forwarder_opaque
	inv.entry.key[2] = &inv.scratchKey;
	inv.flags |= INV_SCRATCHKEY;
      }

      if (inv.key->keyData & capros_Forwarder_sendWord) {
        Key * dataKey = &wrapperNode->slot[ForwarderDataSlot];
        assert(keyBits_IsType(dataKey, KKT_Number));
	inv.entry.w3 = dataKey->u.nk.value[0];
      }

      /* Not hazarded because invocation key */
      inv.key = &(wrapperNode->slot[ForwarderTargetSlot]);
      inv.invKeyType = keyBits_GetType(inv.key);
    } else {
      // Target is not a gate key - treat as void. 
      keyBits_InitToVoid(&inv.scratchKey);
      inv.key = &inv.scratchKey;
      inv.invKeyType = keyBits_GetType(inv.key);
      // Don't set INV_SCRATCHKEY; no need to clean up
    }

    key_Prepare(inv.key);	/* MAY YIELD!!! */
  }
  else if ( inv.invKeyType == KKT_Wrapper
       && keyBits_IsReadOnly(inv.key) == false
       && keyBits_IsNoCall(inv.key) == false
       && keyBits_IsWeak(inv.key) == false ) {

    /* The original plan here was to hide all of the sanity checking
     * for the wrapper node in the PrepAsWrapper() logic. That doesn't
     * work out -- it causes the wrapper node to need deprepare as a
     * unit when slots are altered, with the unfortunate consequence
     * that perfectly good mapping tables can get discarded. It is
     * therefore better to check the necessary constraints here.
     */
    Node *wrapperNode = (Node *) key_GetObjectPtr(inv.key);
    Key* fmtKey /*@ not null @*/ = &wrapperNode->slot[WrapperFormat];

    if (keyBits_IsType(fmtKey, KKT_Number)
	&& keyBits_IsGateKey(&wrapperNode->slot[WrapperKeeper]) ) {
      // FIXME: check WRAPPER_KEEPER
      /* Unlike the older red segment logic, the format key has
       * preassigned slots for the keeper, address space, and so
       * forth.
       */

      if (fmtKey->u.nk.value[0] & WRAPPER_BLOCKED) {
	keyBits_SetWrHazard(fmtKey);

	act_SleepOn(ObjectStallQueueFromObHdr(&wrapperNode->node_ObjHdr));
	act_Yield();
      }

      if (fmtKey->u.nk.value[0] & WRAPPER_SEND_NODE) {
	/* Not hazarded because invocation key */
	key_NH_Set(&inv.scratchKey, inv.key);
	keyBits_SetType(&inv.scratchKey, KKT_Node);
	inv.entry.key[2] = &inv.scratchKey;
	inv.flags |= INV_SCRATCHKEY;
      }

      if (fmtKey->u.nk.value[0] & WRAPPER_SEND_WORD)
	inv.entry.w1 = fmtKey->u.nk.value[1];

      /* Not hazarded because invocation key */
      inv.key = &(wrapperNode->slot[WrapperKeeper]);
#ifndef invKeyType
      inv.invKeyType = keyBits_GetType(inv.key);
#endif

      key_Prepare(inv.key);	/* MAY YIELD!!! */
    }
  } 
  
  assert(keyBits_IsPrepared(inv.key));

  inv.invokee = 0;		/* until proven otherwise */
  
  /* Right now a corner case here is buggered because we have not yet
   * updated the caller's runstate according to the call type.  As a
   * result, a return on a start key to yourself won't work in this
   * code.
   */
  
  if ( keyBits_IsGateKey(inv.key) ) {
    /* Make a local copy (subvert alias analysis pessimism) */
    Process * p = inv.key->u.gk.pContext;
    inv.invokee = p;
    proc_Prepare(p);		/* may yield */

    if (proc_IsWellFormed(p) == false) {
      /* Not hazarded because invocation key */
      /* Pretend we invoked a void key. */
      inv.key = &key_VoidKey;
#ifndef invKeyType
      inv.invKeyType = KKT_Void;
#endif
      inv.invokee = thisPtr;	// Huh? FIXME
      inv.entry.key[RESUME_SLOT] = &key_VoidKey;	// Huh? FIXME
#ifndef NDEBUG
      printf("Jumpee malformed\n");
#endif
    }

#ifndef NDEBUG
    else if (inv.invKeyType == KKT_Resume &&
	     p->runState != RS_Waiting) {
      fatal("Resume key to wrong-state context\n");
    }
#endif
    else if (inv.invKeyType == KKT_Start && p->runState != RS_Available) {
#ifdef GATEDEBUG
      dprintf(GATEDEBUG>2, "Start key, not Available\n");
#endif
  
      act_SleepOn(&p->stallQ);
      act_Yield();
    }
#if 0
    else if ( inv.invType == IT_Call ) {
      BuildResumeKey(inv.resumeKey);
      inv.entry.key[RESUME_SLOT] = &inv.resumeKey;
      inv.flags |= INV_RESUMEKEY;
    }
#endif
  }
  else if (invType_IsCall(inv.invType)) {
    // FIXME: this may not be a good idea for IT_KeeperCall.
    /* Call on non-gate key always returns to caller and requires no
     * resume key.
     *
     * ISSUE: This will not look right to DDB, but it's correct. When
     * you next look at puzzling DDB state and wonder why the resume
     * key never got generated, it is because I did not want to deal
     * with the necessary key destruction overhead. The register
     * renaming key fabrication strategy should regularize this rather
     * nicely when we get there.
     */
    inv.invokee = thisPtr;
    inv.entry.key[3] = &key_VoidKey;
  }
  else {
    /* Kernel key invoked with RETURN or SEND.  Key in slot 3 must be a
     * resume key, and if so the process must be waiting, else we will
     * not return to anyone.
     */

    Key* rk /*@ not null @*/ = inv.entry.key[3];
    key_Prepare(rk);

    /* Kernel keys do a prompt return, so the key in
     * slot four must be a resume key to a party in the right state.
     */

    assert(keyBits_IsHazard(rk) == false);
    
    if (keyBits_IsPreparedResumeKey(rk)) {
      Process *p = rk->u.gk.pContext;
      proc_Prepare(p);

      assert(p->runState == RS_Waiting);
      inv.invokee = p;
    }
    else
      assert (inv.invokee == 0);
  }
  
#ifdef GATEDEBUG
  if (inv.invokee)
    dprintf(GATEDEBUG>2, "Invokee exists\n");
  else
    dprintf(GATEDEBUG>2, "Invokee doesn't exist\n");
#endif

  /* PF_ExpectingMsg isn't significant while RS_Running, so it is OK
  to change it before the invocation is committed.
  Need to set it now in case the invoker is also the invokee. */
  if (inv.invType == IT_KeeperCall)
    thisPtr->processFlags &= ~PF_ExpectingMsg;
  else
    thisPtr->processFlags |= PF_ExpectingMsg;

  /********************************************************************
   * It is still possible that the invoker will block while
   * some part of the invokee gets paged in or while waiting for an
   * available Activity structure.  The latter is a problem, and needs
   * to be dealt with.  Note that the finiteness of the activity pool
   * isn't the source of the problem -- the real problem is that we
   * might not be able to fit all of the running domains in the swap
   * area. Eventually we shall need to implement a decongester to deal
   * with this, but that can wait.
   *********************************************************************/

  if (inv.invokee && proc_IsWellFormed(inv.invokee) == false) {
#ifdef GATEDEBUG
    dprintf(GATEDEBUG>2, "Invokee malformed\n");
#endif

    inv.invokee = 0;
  }

  assert(keyBits_IsPrepared(inv.key));
  
#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Invokee now valid\n");
#endif

  assert(inv.invokee == 0 || proc_IsRunnable(inv.invokee));
  
  /* It does not matter if the invokee fails to prepare!
   * Note that we may be calling this with 'this == 0'
   */
  proc_SetupExitBlock(inv.invokee, &inv);

#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Populated exit block\n");
#endif
  
  /* Identify the activity that will migrate to the recipient.  Normally
   * it's the current activity.  If this is a SEND invocation, it's a
   * new activity.  Populate this activity consistent with the invokee.
   */
  
  Activity * activityToMigrate = act_Current();
  

  if (inv.invType == IT_Send) {
    Key* rk /*@ not null @*/ = inv.entry.key[3];

    /* If this is a send, and we are either (a) invoking a gate key,
       or (b) invoking a kernel key and passing a resume key to
       someone else, then we will need a new activity.

       That covers most cases. For real fun, consider a send on a
       domain key saying 'start this process' with resume key to third
       party in slot 3 -- we will (someday) handle that in the
       ProcessKey handler as a special case prior to the
       COMMIT_POINT() logic. */

    if ( keyBits_IsGateKey(inv.key) || keyBits_IsType(rk, KKT_Resume) ) {
  
      activityToMigrate = (Activity *) act_AllocActivity();
  
      activityToRelease = activityToMigrate;
      activityToMigrate->state = act_Ready;
#ifdef GATEDEBUG
      dprintf(GATEDEBUG>2, "Built new activity for fork\n");
#endif
    }
    else
      activityToMigrate = 0;
  }
  
#if defined(OPTION_DDB) && !defined(NDEBUG)
  bool invoked_gate_key = keyBits_IsGateKey(inv.key);
  
#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
  if (dbg_wild_ptr)
    check_Consistency("DoKeyInvocation() before invoking handler\n");
#endif

  if ( DDB_STOP(all) ||
       (DDB_STOP(gate) && invoked_gate_key) ||
       (DDB_STOP(keeper)
        && ! proc_IsExpectingMsg(inv.invokee)) ||
       (DDB_STOP(pflag) && 
	( (thisPtr->processFlags & PF_DDBINV) ||
	  (inv.invokee && inv.invokee->processFlags & PF_DDBINV) ))
       )
    dprintf(true, "About to invoke key handler (inv.ty=%d) ic=%d\n",
		    inv.invKeyType, KernStats.nInvoke);
#endif

#if defined(OPTION_KERN_TIMING_STATS)
  uint64_t pre_handler = rdtsc();
#endif

  /* The key handler must do the following:
  1. Complete the dry run, faulting in objects, preparing them, and
     Yielding if necessary.
  2. Commit the invocation. 
  3. Perform the (side) effects of the object. 
  4. Return 4 keys to the invokee. (I want to change this: it should return to the caller of proc_KeyDispatch the number of keys returned; this code will then
return void keys in the rest)
  5. Return any string to the invokee, setting inv.sentLen.
  6. Set any return words in inv.
  7. Set keyData if any in inv.
   */
#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Before proc_KeyDispatch\n");
#endif

  proc_KeyDispatch(&inv);
  
#if defined(OPTION_KERN_TIMING_STATS)
  {
    extern uint32_t inv_delta_reset;
    if (inv_delta_reset == 0) {
      extern uint64_t inv_handler_cy;
      uint64_t post_handler = rdtsc();
      inv_handler_cy += (post_handler - pre_handler);
      inv_KeyHandlerCycles[inv.invKeyType][inv.invType] +=
	(post_handler - pre_handler);
      inv_KeyHandlerCounts[inv.invKeyType][inv.invType] ++;
    }
  }  
#endif

#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
  if (dbg_wild_ptr)
    check_Consistency("DoKeyInvocation() after invoking handler\n");
#endif

  assert (InvocationCommitted);

  assert(act_Current()->context == thisPtr);

#ifndef NDEBUG
  InvocationCommitted = false;    
#endif
  
#if defined(OPTION_DDB) && !defined(NDEBUG)
  if ( DDB_STOP(all) ||
       ( DDB_STOP(gate) && invoked_gate_key ) ||
       (DDB_STOP(keeper)
        && ! proc_IsExpectingMsg(inv.invokee)) ||
       ( DDB_STOP(return) && (inv.invType == IT_Return)) ||
       (DDB_STOP(pflag) && 
	( (thisPtr->processFlags & PF_DDBINV) ||
	  (inv.invokee && inv.invokee->processFlags & PF_DDBINV) )) ||
       ( DDB_STOP(keyerr) &&
	 !invoked_gate_key &&
	 (inv.invKeyType != KKT_Void) &&
	 (inv.exit.code != RC_OK) ) )
    dprintf(true, "Before DeliverResult() (invokee=0x%08x)\n",
		    inv.invokee); 
#endif
  
  /* Check the sanity of the receiving process in various ways: */
  
  if (inv.invokee) {
    if (proc_IsNotRunnable(inv.invokee)) {
      if (inv.invokee->procRoot == 0) {
	inv.invokee = 0;
	goto bad_invokee;
      }
      
      proc_Prepare(inv.invokee);

      if (proc_IsNotRunnable(inv.invokee)) {
	inv.invokee = 0;
	goto bad_invokee;
      }
    }

    /* Invokee is okay.  Deliver the result: */
    inv.invokee->runState = RS_Running;

    /* If this was a kernel key invocation in the fast path, we never
       bothered to actually set them waiting, but they were logically
       in the waiting state nonetheless. */

    if (proc_IsExpectingMsg(inv.invokee)) {
      if (!inv.suppressXfer) {
        proc_DeliverResult(inv.invokee, &inv);
      }
      // else this was done by GateKey

      proc_AdvancePostInvocationPC(inv.invokee);
    }

#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
    if (dbg_wild_ptr)
      check_Consistency("DoKeyInvocation() after DeliverResult()\n");
#endif

    /* If we are returning to ourselves, the resume key was never
     * generated.
     */
    if (inv.invokee != thisPtr)
      keyR_ZapResumeKeys(&inv.invokee->keyRing);
  }
 bad_invokee:
  
  if (inv.invType == IT_Send) {
    /* dprintf(false, "Advancing SENDer PC in slow path\n"); */
    proc_AdvancePostInvocationPC(thisPtr);
  }

  /* ONCE DELIVERRESULT IS CALLED, NONE OF THE INPUT CAPABILITIES
     REMAINS ALIVE!!! */
  
  /* Clean up the invocation block: */
  inv_Cleanup(&inv);
#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Cleaned up invocation\n");
#endif
  
  if (activityToMigrate) {
    act_MigrateTo(activityToMigrate, inv.invokee);

#ifdef OPTION_DDB
    if (inv.invokee && inv.invokee->readyQ == &prioQueues[pr_Never])
      dprintf(true, "Activity now in ctxt 0x%08x w/ bad schedule\n", 
		      inv.invokee);
#endif

    if (inv.invType == IT_Send && inv.invokee) {
      act_Wakeup(activityToMigrate);
#ifdef GATEDEBUG
      dprintf(GATEDEBUG>2, "Woke up forkee\n");
#endif
    }
  }
  
#ifdef DBG_WILD_PTR
  {
    extern void ValidateAllActivityies();
    ValidateAllActivities();
  }
#endif

  inv.invokee = 0;
  activityToRelease = 0;
  
#ifdef OPTION_KERN_TIMING_STATS
  {
    extern uint32_t inv_delta_reset;

    if (inv_delta_reset == 0) {
      extern uint64_t inv_delta_cy;

      uint64_t bot_time = rdtsc();
#ifdef OPTION_KERN_EVENT_TRACING
      extern uint64_t inv_delta_cnt0;
      extern uint64_t inv_delta_cnt1;

      uint64_t bot_cnt0 = mach_ReadCounter(0);
      uint64_t bot_cnt1 = mach_ReadCounter(1);
      inv_delta_cnt0 += (bot_cnt0 - top_cnt0);
      inv_delta_cnt1 += (bot_cnt1 - top_cnt1);
#endif
      inv_delta_cy += (bot_time - top_time);
    }
    else
      inv_delta_reset = 0;
  }
#endif
}

/* May Yield. */
void
proc_InvokeMyKeeper(Process* thisPtr, uint32_t oc,
                    uint32_t warg1,
                    uint32_t warg2,
                    uint32_t warg3,
                    Key *keeperKey,
                    Key* keyArg2, uint8_t *data, uint32_t len)
{
  BeginInvocation();
  KernStats.nInvKpr++;
  
  /* Do not call BeginTransaction here -- the only way we can be here
   * is if we are already in some transaction, and it's okay to let
   * that transaction prevail.
   */

  // Set up the entry block.

  /* Not hazarded because invocation key */
  inv.key = keeperKey;
  inv.invType = IT_KeeperCall;
  
  inv.entry.code = oc;
  inv.entry.w1 = warg1;
  inv.entry.w2 = warg2;
  inv.entry.w3 = warg3;

  inv.entry.key[0] = &key_VoidKey;
  inv.entry.key[1] = &key_VoidKey;
  inv.entry.key[2] = keyArg2;
  inv.entry.key[3] = &key_VoidKey;

  inv.entry.len = len;
  inv.sentLen = 0;
  
  key_Prepare(keeperKey);
#ifndef invKeyType
  inv.invKeyType = keyBits_GetType(keeperKey);
#endif

  inv.entry.data = data;
  
  if ( DDB_STOP(keeper) )
    dprintf(true, "About to invoke keeper, key=0x%08x\n", keeperKey);

#ifdef KPRDEBUG
  dprintf(false, "Enter InvokeMyKeeper\n");
#endif
  
  // We should allow this also for a wrapper of a gate key.
  if (keyBits_IsGateKey(keeperKey)) {
#ifdef KPRDEBUG
    dprintf(true, "Kpr key is gate key\n");
#endif

    proc_DoGeneralKeyInvocation(thisPtr);	/* For better performance,
	we could instead call the guts of proc_DoKeyInvocation. */
  } else {	// keeper is not a gate key
#ifdef KPRDEBUG
    dprintf(true, "Kpr key is NOT a gate key!\n");
#endif

#ifndef NDEBUG
    OID oid = act_CurContext()->procRoot->node_ObjHdr.oid;
    printf("No keeper for OID 0x%08x%08x, FC %d FINFO 0x%08x\n",
		   (uint32_t) (oid >> 32),
                   (uint32_t) oid,
		   ((Process *)act_CurContext())->faultCode, 
		   ((Process *)act_CurContext())->faultInfo);
    dprintf(true,"Dead context was 0x%08x\n", act_CurContext());
#endif
    /* Just retire the activity, leaving the domain in the running
     * state.
     */
    act_MigrateTo(act_Current(), 0);
#if 0
    act_Current()->SleepOn(procRoot->ObjectStallQueue());
    act_Yield();
#endif
  }

#ifdef OPTION_KERN_TIMING_STATS
  {
    extern uint64_t kpr_delta_cy;

    uint64_t bot_time = rdtsc();
#ifdef OPTION_KERN_EVENT_TRACING
    extern uint64_t kpr_delta_cnt0;
    extern uint64_t kpr_delta_cnt1;
    
    uint64_t bot_cnt0 = mach_ReadCounter(0);
    uint64_t bot_cnt1 = mach_ReadCounter(1);
    kpr_delta_cnt0 += (bot_cnt0 - top_cnt0);
    kpr_delta_cnt1 += (bot_cnt1 - top_cnt1);
#endif
    kpr_delta_cy += (bot_time - top_time);
  }
#endif
}
