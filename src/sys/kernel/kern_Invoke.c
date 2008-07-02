/*
 * Copyright (C) 1998, 1999, 2001, Jonathan S. Shapiro.
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

/* Driver for key invocation */

#include <string.h>
#include <kerninc/kernel.h>
#include <kerninc/Activity.h>
#include <kerninc/Debug.h>
#include <kerninc/Check.h>
#include <kerninc/Invocation.h>
#include <kerninc/GPT.h>
#include <kerninc/IRQ.h>
#include <arch-kerninc/Process-inline.h>
#include <arch-kerninc/PTE.h>
#ifdef OPTION_DDB
#include <eros/StdKeyType.h>
#endif

#include <kerninc/Machine.h>
#include <kerninc/KernStats.h>
#include <eros/KeyConst.h>	// segment and wrapper defs
#include <kerninc/Key-inline.h>
#include <kerninc/Node-inline.h>

#include <disk/Forwarder.h>
#include <idl/capros/Forwarder.h>
#include <idl/capros/GPT.h>

// #define GATEDEBUG 5
// #define KPRDEBUG
 

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
  switch (inv.invKeyType) {
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
 *    3. Determine who we will be exiting to (the returnee aka invokee) --
 *       this is often the invoker.  It can also be the domain named by the
 *       invoked gate key.  It can also be the domain named by the key
 *       in slot RESUME_SLOT if this is a kernel key (YUCK).  It can also be
 *       NULL if the key in slot RESUME_SLOT proves not to be a resume key.
 * 
 *       Invoked      Invocation    Slot RESUME_SLOT   Invokee
 *       Key Type     Type          Key Type
 * 
 *       Gate Key     ANY           *                  Named by gate key
 *       Kernel Key   CALL          *                  Same as invoker
 *       Kernel Key   SEND,RETURN   Resume             Named by resume key
 *       Kernel Key   SEND,RETURN   !Resume            NONE
 * 
 *       Note that the case of CALL on a kernel key is in some sense
 *       the same as SEND/RETURN on a kernel key -- the resume key was
 *       generated by the call.
 * 
 *    4. Construct a receive buffer mapping, splicing in the
 *       kernel-reserved void page where no mapping is present.
 *
 *    5. Other tests specific to the invoked key. 
 * 
 *    6. Determine if a new Activity is to be created.
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
bool ReturneeSetUp = false;
bool traceInvs = false;
#endif

/* May Yield. */
void 
inv_Commit(Invocation * thisPtr)
{
  assert(!InvocationCommitted);	// can only commit once
  assert(act_CurContext()->runState == RS_Running);
  assert(ReturneeSetUp);
  assert(! allocatedActivity);

  // Do we need to decommit this invocation?
  if (MapsWereInvalidated())
    inv_RetryInvocation(thisPtr);	// Yields

  /* Revise the invoker state. */
  Process * invoker = act_CurContext();

  switch (inv.invType) {
    // Note, prompt types have been converted to nonprompt by this time.
  case IT_Return:
    invoker->runState = RS_Available;
#if 0
    dprintf(false, "Wake up the waiters sleeping on dr=0x%08x\n", invoker);
#endif
    sq_WakeAll(&invoker->stallQ, false);
    goto inactivate;

  case IT_Send:
    /* If this is a send, we will need a new Activity for the invokee.
       Allocate it now before the invocation is committed.
       Allocation failure is not currently handled, but when it is,
       we could Yield here before the invocation is committed. */

    allocatedActivity = act_AllocActivity();
    allocatedActivity->state = act_Ready;
#ifdef GATEDEBUG
    dprintf(GATEDEBUG>2, "Built new activity for Send\n");
#endif
    // invoker remains RS_Running
    proc_AdvancePostInvocationPC(invoker);
    break;

  case IT_Call:
  case IT_KeeperCall:
    invoker->runState = RS_Waiting;

    if (invoker != inv.invokee) {
      // Call of a gate key, or call makeResumeKey(self)
inactivate: ;
      /* The invoker is losing its Activity.
      Save it for use by the invokee. */
      Activity * t = invoker->curActivity;

      assert(t == act_Current());
      assert(t->context == invoker);

      proc_Deactivate(invoker);
#ifndef NDEBUG
      /* Note, at this point we have: */
      assert(act_Current() == t);
      assert(proc_curProcess == invoker);
      // but:
      assert(invoker->curActivity == NULL);
      t->context = NULL;	// to guard against (mis)use
#endif
      allocatedActivity = t;
    }
    break;

  default: assert(false);
  }
  
#ifndef NDEBUG
  InvocationCommitted = true;
#endif
}

bool 
inv_IsInvocationKey(Invocation* thisPtr, const Key* pKey)
{
  /* because this gets set up early in the keeper invocation
   * path, /inv.key/ may not be set yet, so check this early.
   */
  if (pKey == &thisPtr->keeperArg)
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
  inv.invokee = 0;	// to catch bugs
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
  if (thisPtr->flags & INV_KEEPERARG)
    key_NH_SetToVoid(&thisPtr->keeperArg);
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
  assert(act_CurContext()->runState == RS_Running);

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

/* Kernel keys call this to set up the returnee/invokee. */
// May Yield.
void
inv_GetReturnee(Invocation * inv)
{
  if (invType_IsCall(inv->invType)) {
    // FIXME: this may not be a good idea for IT_KeeperCall.
    /* Call on non-gate key always returns to caller and requires no
     * resume key.
     */
    inv->invokee = act_CurContext();
    // inv->entry.key[3] = &key_VoidKey;
  }
  else {
    /* Kernel key invoked with RETURN or SEND.  Key in slot 3 must be a
     * resume key, else we will not return to anyone.  */

    Key * rk /*@ not null @*/ = inv->entry.key[3];
    key_Prepare(rk);

    /* Kernel keys do a prompt return, so the key in
     * slot four must be a resume key to a party in the right state.
     */

    assert(keyBits_IsHazard(rk) == false);
    
    if (keyBits_IsPreparedResumeKey(rk)) {
      Process * p = rk->u.gk.pContext;
      if (! proc_IsRunnable(p)) {
        proc_DoPrepare(p);
        if (! proc_IsWellFormed(p)) {
#ifdef GATEDEBUG
          dprintf(GATEDEBUG>2, "Invokee malformed\n");
#endif
          goto noInvokee;
        }
        assert(proc_IsRunnable(p));
      }
      inv->invokee = p;
      assert(p->runState == RS_Waiting);
    }
    else
noInvokee:
      inv->invokee = 0;
  }
  
#ifdef GATEDEBUG
  if (inv->invokee)
    dprintf(GATEDEBUG>2, "Invokee now valid\n");
  else
    dprintf(GATEDEBUG>2, "Invokee doesn't exist\n");
#endif

  inv_SetupExitBlock(inv);

#ifndef NDEBUG
  ReturneeSetUp = true;
#endif
}

void
ReturnMessage(Invocation * inv)
{
  assert (InvocationCommitted);

  Process * invokee = inv->invokee;

  /* Check the sanity of the receiving process in various ways: */
  
  if (invokee) {
    if (proc_IsNotRunnable(invokee)) {
      if (invokee->procRoot == 0) {
	goto bad_invokee;
      }
      
      proc_DoPrepare(invokee);

      if (proc_IsNotRunnable(invokee)) {
	goto bad_invokee;
      }
    }
    /* Invokee is okay. */

    /* If we are returning to ourselves, the resume key was never
     * generated.
     */
    if (invokee != proc_curProcess) {
      /* keyR_ZapResumeKeys isn't always needed; may be faster to check
      first if it is necessary: */
      if (inv->invKeyType != KKT_Start) {
        keyR_ZapResumeKeys(&invokee->keyRing);
        node_BumpCallCount(invokee->procRoot);
      }
      act_AssignTo(allocatedActivity, invokee);

      if (inv->invType == IT_Send) {
        act_Wakeup(allocatedActivity);
#ifdef GATEDEBUG
        dprintf(GATEDEBUG>2, "Woke up forkee\n");
#endif
      }
    }
    invokee->runState = RS_Running;

    /* Deliver the result: */

    if (proc_IsExpectingMsg(invokee)) {
      proc_DeliverResult(invokee, inv);
      proc_AdvancePostInvocationPC(invokee);
    }

#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
    if (dbg_wild_ptr)
      check_Consistency("DoKeyInvocation() after DeliverResult()");
#endif
  }
  else {
 bad_invokee:
    act_DeleteActivity(allocatedActivity);
  }
}

void
inv_SetupExitBlock(Invocation * inv)
{
  assert(inv->invokee == 0 || proc_IsRunnable(inv->invokee));
  
  /* It does not matter if the invokee fails to prepare!
   * Note that we may be calling this with 'this == 0'
   */
  proc_SetupExitBlock(inv->invokee, inv);

#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Populated exit block\n");
#endif
}

void
inv_InvokeGateOrVoid(Invocation * inv, Key * invKey)
{
  // Prepare it now, in case a gate key becomes void.
  key_Prepare(invKey);	/* may yield */

  /* We require the target key to be a gate key to avoid unlimited recursion. */
  if (keyBits_IsGateKey(invKey) ) {
    /* Not hazarded because invocation key */
    inv->key = invKey;
    inv->invKeyType = keyBits_GetType(inv->key);
    GateKey(inv);
  }
  else {
    // Target is not a gate key - treat as void. 
    VoidKey(inv);
  }
}

extern uint64_t rdtsc();

#ifdef OPTION_KERN_TIMING_STATS
uint64_t top_time;
#ifdef OPTION_KERN_EVENT_TRACING
uint64_t top_cnt0;
uint64_t top_cnt1;
#endif
#endif

void
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
  ReturneeSetUp = false;
#endif
}

/* May Yield. */
void 
proc_DoKeyInvocation(Process* thisPtr)
{
  key_Prepare(inv.key);
#ifndef invKeyType
  inv.invKeyType = keyBits_GetType(inv.key);
#endif

#ifndef NDEBUG
  if (traceInvs) {
    /* logging mach_TicksToNanoseconds(sysT_Now()) is of little use,
    because the time to print the message dominates. */
    printf("Ivk proc=%#x ", thisPtr);
    if (thisPtr->procRoot &&
        keyBits_IsType(&thisPtr->procRoot->slot[ProcSymSpace], KKT_Number)) {
      void db_eros_print_number_as_string(Key* k);
      db_eros_print_number_as_string(&thisPtr->procRoot->slot[ProcSymSpace]);
    }
    printf(" invSlot=%d oc=%#x\n",
           inv.key - &thisPtr->keyReg[0], inv.entry.code);
  }
#endif

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

  assert(keyBits_IsPrepared(inv.key));

  /* capros_Process_PF_ExpectingMessage isn't significant while RS_Running,
  so it is OK to change it before the invocation is committed.
  Need to set it now in case the invoker is also the invokee. */
  if (inv.invType == IT_KeeperCall)
    thisPtr->processFlags &= ~capros_Process_PF_ExpectingMessage;
  else
    thisPtr->processFlags |= capros_Process_PF_ExpectingMessage;

#if defined(OPTION_DDB) && !defined(NDEBUG)
  bool invoked_gate_key = keyBits_IsGateKey(inv.key);
  
#if defined(DBG_WILD_PTR) || defined(TESTING_INVOCATION)
  if (dbg_wild_ptr)
    check_Consistency("DoKeyInvocation() before invoking handler");
#endif

  if ( DDB_STOP(all) ||
       (DDB_STOP(gate) && invoked_gate_key) ||
       (DDB_STOP(pflag) && 
	( (thisPtr->kernelFlags & KF_DDBINV) ||
	  (inv.invokee && inv.invokee->kernelFlags & KF_DDBINV) ))
       )
    dprintf(true, "About to invoke key handler (inv.ty=%d) ic=%d\n",
		    inv.invKeyType, KernStats.nInvoke);
#endif

#if defined(OPTION_KERN_TIMING_STATS)
  uint64_t pre_handler = rdtsc();
#endif

  /* The key handler must do the following:
  0. Prepare the returnee.
  1. Complete the dry run, faulting in objects, preparing them, and
     Yielding if necessary.
  2. Commit the invocation. 
  3. Perform the (side) effects of the object. 
  4. Return any keys to the invokee. (I want to change this: it should return to the caller of proc_KeyDispatch the number of keys returned; this code will then
return void keys in the rest, instead of pre-initializing inv.exit.key[n].)
  5. Return any string to the invokee, setting inv.sentLen.
  6. Set any return words in inv.
  7. Set keyData if any in inv.
   */
#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Before proc_KeyDispatch\n");
#endif

  proc_KeyDispatch(&inv);

#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "After proc_KeyDispatch\n");
#endif

#ifndef NDEBUG
  allocatedActivity = 0;
  InvocationCommitted = false;    
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
    check_Consistency("DoKeyInvocation() after invoking handler");
#endif
  
#if defined(OPTION_DDB) && !defined(NDEBUG)
  if ( DDB_STOP(all) ||
       ( DDB_STOP(gate) && invoked_gate_key ) ||
       ( DDB_STOP(return) && (inv.invType == IT_Return)) ||
       (DDB_STOP(pflag) && 
	( (thisPtr->kernelFlags & KF_DDBINV) ||
	  (inv.invokee && inv.invokee->kernelFlags & KF_DDBINV) )) ||
       ( DDB_STOP(keyerr) &&
	 !invoked_gate_key &&
	 (inv.invKeyType != KKT_Void) &&
	 (inv.exit.code != RC_OK) ) )
    dprintf(true, "Before DeliverResult() (invokee=0x%08x)\n",
		    inv.invokee); 
#endif
  
  /* Clean up the invocation block: */
  inv_Cleanup(&inv);

#ifdef GATEDEBUG
  dprintf(GATEDEBUG>2, "Cleaned up invocation\n");
#endif

#ifdef DBG_WILD_PTR
  {
    extern void ValidateAllActivitys();
    ValidateAllActivitys();
  }
#endif
  
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

    proc_DoKeyInvocation(thisPtr);

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
    act_DeleteCurrent();
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
