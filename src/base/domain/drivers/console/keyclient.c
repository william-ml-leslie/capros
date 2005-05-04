/*
 * Copyright (C) 2002, Jonathan S. Shapiro.
 *
 * This file is part of the EROS Operating System distribution.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330 Boston, MA 02111-1307, USA.
 */

/* Keyboard Client to the ps2 driver(keyb). This process gets keyboard
 * scan codes from the ps2 driver, translates them to ASCII.
 * The builder is retry-parked on a shared main & woken up when
 * a character arrives from the ps2 reader
 */
 
#include <stddef.h>
#include <eros/target.h>
#include <eros/ProcessState.h>
#include <eros/NodeKey.h>
#include <eros/KeyConst.h>
#include <eros/ProcessKey.h>
#include <eros/Invoke.h>
#include <eros/cap-instr.h>
#include <eros/i486/atomic.h>

#include <idl/eros/Number.h>
#include <idl/eros/key.h>
#include <idl/eros/Ps2.h>

#include <idl/console/keyclient.h>

#include <domain/SpaceBankKey.h>
#include <domain/ConstructorKey.h>
#include <domain/domdbg.h>
#include <domain/ProcessCreatorKey.h>
#include <domain/Runtime.h>

#include "constituents.h"
#include "keytrans.h"

#define KR_DOMCRE       3
#define KR_OSTREAM      KR_APP(0)
#define KR_PS2READER    KR_APP(2)
#define KR_MCLI_C       KR_APP(3)
#define KR_MCLI_S       KR_APP(4)
#define KR_START        KR_APP(5)
#define KR_PARENT       KR_APP(6)
#define KR_NEWPROC      KR_APP(7)
#define KR_NEWFAULT     KR_APP(8)
#define KR_SHAREDSTART  KR_APP(9)
#define KR_PARKNODE     KR_APP(10)
#define KR_PARKWRAP     KR_APP(11)

#define KR_SCRATCH      KR_APP(20)

#define NOCHAR  -10  /* Some value not corresponding to any ASCII */
 
/* function declarations */
int parkCall(Message *msg);
int wakeCall(int parkingNo);
unsigned char getChar(void);
int getstate(int scan, int *status);
int ProcessRequest();
void updateLeds(int *status);
void makeSharedProc(cap_t krNewProc,uint32_t sp);
void sharedMain();

/* Globals */
uint32_t charReceived = NOCHAR; /* Use atomic swap to protect */

int
provide_parent_key(cap_t krProc) 
{
  Message msg;
  
  msg.snd_invKey = krProc;
  msg.snd_code   = 0;
  msg.snd_key0   = KR_VOID; 
  msg.snd_key1   = KR_PARENT;
  msg.snd_key2   = KR_VOID; 
  msg.snd_rsmkey = KR_VOID;
  msg.snd_data = 0;
  msg.snd_len  = 0;
  msg.snd_w1 = 0;
  msg.snd_w2 = 0;
  msg.snd_w3 = 0;
  
  msg.rcv_key0   = KR_VOID;
  msg.rcv_key1   = KR_VOID;
  msg.rcv_key2   = KR_VOID;
  msg.rcv_rsmkey = KR_VOID;
  msg.rcv_data = 0;
  msg.rcv_limit = 0;
  msg.rcv_code = 0;
  msg.rcv_w1 = 0;
  msg.rcv_w2 = 0;
  msg.rcv_w3 = 0;
  CALL(&msg);
  
  return 0; 
}

/* We have a node from the spcbank which we have turned into
 * a "parknode". Here we park our client */
int 
parkCall(Message *msg) {
  msg->invType = IT_Retry;
  msg->snd_w1 = RETRY_SET_LIK|RETRY_SET_WAKEINFO;
  msg->snd_w2 = msg->rcv_w1; /* wakeinfo value */
  msg->snd_key0 = KR_PARKWRAP;
   
  return 1;
}

/* We wake up if any bit is set in node_wake...So all the andBits,
 * orBits & match are the same. Now the client given by the parkingNo
 * is woken up and free to retry its call */
int 
wakeCall(int parkingNo) {
  node_wake_some_no_retry(KR_PARKNODE,0,0,parkingNo);
  return 1;
}

/* Get char from the ps2 driver (ps2reader). ASCII Characters may 
 * comprise of multiple scancodes */
unsigned char
getChar(void) 
{
  static int state = 0;
  int i, prevcode, scode;
  unsigned char kcode;
  int32_t valid;
  int32_t data;
  
  kcode = 0;
  prevcode = 0;
  
  /* Call ps2reader */
  while (kcode == 0) {
    (void)eros_Ps2_getKeycode(KR_PS2READER,&data,&valid);
    
    while(data > -1) { 
      do {
	scode = scanToKey(data, &prevcode, state);
      } while (prevcode == -1);
      
      if ((updShiftState(scode, data & 0x80, &state)) == 0) {
	i = getstate(scode, &state);
	kcode = key_map.key[scode].map[i];
      }
      
      if (state & LED_UPDATE) {
	updateLeds(&state);
      }
            
      (void)eros_Ps2_getKeycode(KR_PS2READER,&data,&valid);
    }
  }
  //kprintf(KR_OSTREAM,"Returning ASCIIcode = %d",kcode);
  return kcode;
}

int
getstate(int scan, int *status) 
{
  int i, state;
  state = *status;

  i = ((state & SHIFTS) ? 1 : 0)
    | ((state & CTLS) ? 2 : 0)
    | ((state & ALTS) ? 4 : 0);

  /* only upper case on letter keys */  
  if (state & CLKED) {
    if ((scan >= 0x10) && (scan <= 0x19)) {
      if (state & SHIFTS) {
	i &= ~1;
      }
      else {
	i |= 1;
      }
    }
    else if ((scan >= 0x1E) && (scan <= 0x26)) {
      if (state & SHIFTS) {
	i &= ~1;
      }
      else {
	i |= 1;
      }
    }
    else if ((scan >= 0x2C) && (scan <= 0x32)) {
      if (state & SHIFTS) {
	i &= ~1;
      }
      else {
	i |= 1;
      }
    }
  }

  /* get keypad keys when numlocked */
  if (state & NLKED) {
    if ((scan >= 0x47) && (scan <= 0x53)) {
      if (state & SHIFTS) {
	i &= ~1;
      }
      else {
	i |= 1;
      }
    }
  }

  return i;
}

/* Update the LEDs incase of Caps, scroll,Num lock */
void
updateLeds(int *status) 
{
  int result;
  int state = *status;
    
  /* clear update flag */
  state &= ~LED_UPDATE;
  *status = state;

  /* clear unnecessary bits */
  state = state << 5;
  state = state >> 5;

  result = eros_Ps2_setLed(KR_PS2READER,state);
  
  if (result != RC_OK) {
    kprintf(KR_OSTREAM, "Problem setting LEDs.errcode = %d",result);
  }

  return;
}

/* Call our parent to notify it of new characters */
int
ProcessRequest() 
{
  unsigned char ch = getChar();
  
  ATOMIC_SWAP32(&charReceived,charReceived,ch);
  //charReceived = ch; //Do Atomically
  
  /* Wake the caller on our shared main Viz. our builder */
  wakeCall(0);
  
  return 1;
}

/* Wake up the process if we have some character else park it
 * on a descriptor to the park node */
int 
ProcessSharedMainRequest(Message *msg) 
{
  msg->snd_len = 0;
  msg->snd_key0 = KR_VOID;
  msg->snd_key1 = KR_VOID;
  msg->snd_key2 = KR_VOID;
  msg->snd_rsmkey = KR_RETURN;
  msg->snd_code = RC_OK;
  msg->snd_w1 = 0;
  msg->snd_w2 = 0;
  msg->snd_w3 = 0;
  msg->snd_invKey = KR_RETURN;
  
  switch (msg->rcv_code) {
  case OC_console_keyclient_getChar:
    {
      if(charReceived == NOCHAR)  { 
	//kprintf(KR_OSTREAM,"Parking call");
	parkCall(msg);
      }
      else  { 
	//kprintf(KR_OSTREAM,"Sending character");
	msg->snd_w1 = charReceived;
	charReceived = NOCHAR;
	ATOMIC_SWAP32(&charReceived,charReceived,NOCHAR);
      }
      
      return 1;
    }
  default:
    kprintf(KR_OSTREAM,"keyclient SharedMain default");
    break;
  }

  msg->snd_code = RC_eros_key_UnknownRequest;
  return 1;
}


void 
sharedMain() 
{
  Message msg;
  
  /* Return to null and wait for events */
  msg.snd_invKey = KR_VOID;
  msg.snd_key0   = KR_VOID;
  msg.snd_key1   = KR_VOID;
  msg.snd_key2   = KR_VOID;
  msg.snd_rsmkey = KR_RETURN;
  msg.snd_data = 0;
  msg.snd_len  = 0;
  msg.snd_code = 0;
  msg.snd_w1 = 0;
  msg.snd_w2 = 0;
  msg.snd_w3 = 0;

  msg.rcv_key0   = KR_VOID;
  msg.rcv_key1   = KR_VOID;
  msg.rcv_key2   = KR_VOID;
  msg.rcv_rsmkey = KR_RETURN;
  msg.rcv_data = 0;
  msg.rcv_limit = 0;
  msg.rcv_code = 0;
  msg.rcv_w1 = 0;
  msg.rcv_w2 = 0;
  msg.rcv_w3 = 0;
  msg.invType = IT_PReturn;
  
  do {
    INVOKECAP(&msg);
    msg.rcv_rsmkey = KR_RETURN;
    msg.invType = IT_PReturn;
  }while(ProcessSharedMainRequest(&msg));
  
  return;
}

void 
makeSharedProc(cap_t krNewProc,uint32_t sp) 
{
  uint32_t result;
  struct Registers regs;
  Message msg;
  
  result = proccre_create_process(KR_DOMCRE, KR_BANK, krNewProc);
  if (result != RC_OK) kprintf (KR_OSTREAM,"Failed to create new process");
  
  //copy address space
  process_copy(KR_SELF, ProcAddrSpace, KR_SCRATCH);
  process_swap(krNewProc, ProcAddrSpace, KR_SCRATCH, KR_VOID);
  
  //copy schedule key over
  process_copy(KR_SELF, ProcSched, KR_SCRATCH);
  process_swap(krNewProc, ProcSched, KR_SCRATCH, KR_VOID);
  process_swap_keyreg(krNewProc, KR_SCHED, KR_SCRATCH, KR_VOID);
  
  //copy spacebank key over
  process_copy_keyreg(KR_SELF,KR_BANK, KR_SCRATCH);
  process_swap_keyreg(krNewProc,KR_BANK,KR_SCRATCH, KR_VOID);
  
  //copy KR_OSTREAM key over 
  process_copy_keyreg(KR_SELF,KR_OSTREAM,KR_SCRATCH);
  process_swap_keyreg(krNewProc,KR_OSTREAM,KR_SCRATCH, KR_VOID);
  
  /* Sleazy portability trick. Rather than set all target register by
     hand (most don't actually matter), copy out our own registers and
     then clobber the ones that we actually need to change. This is
     ugly, but relatively more machine independent (cough, ahem). This
     deals conveniently with segmentation registers on machines that
     have them.  Note that if we fail to set the seg regs, then the
     kernel barfs (at the moment). */
  process_get_regs(KR_SELF, &regs);
  
  regs.CS = DOMAIN_CODE_SEG;
  regs.SS = DOMAIN_DATA_SEG;
  regs.DS = DOMAIN_DATA_SEG;
  regs.ES = DOMAIN_DATA_SEG;
  regs.FS = DOMAIN_DATA_SEG;
  regs.GS = DOMAIN_PSEUDO_SEG;
  
  regs.pc = (unsigned)&sharedMain;
  regs.nextPC = (unsigned)&sharedMain;
  
  /* ::Was 0x30000u. I have no idea on this one!!! */
  regs.sp = sp;

  regs.faultCode = 0;
  regs.faultInfo = 0;
  regs.domState = RS_Waiting;
  regs.domFlags = 0;
  regs.EFLAGS = 0x200;

  result = process_set_regs(krNewProc, &regs);
  if (result != RC_OK) 
    kprintf(KR_OSTREAM,"makeSharedProc,setting regs...[FAILED]");
  
  result = process_make_fault_key(krNewProc, KR_NEWFAULT);
  if (result != RC_OK) 
    kprintf(KR_OSTREAM,"makeSharedProc Making fault key..[FAILED]");
  
  /* We buy a node from the spacebank to make a "park-node" on which 
   * clients can be redirected to wait. We can then selectively wake 
   * up these parked clients when the appropriate trigger fires */
  result = spcbank_buy_nodes(KR_BANK, 1, KR_PARKNODE, KR_VOID, KR_VOID);
  if(result != RC_OK)  kdprintf(KR_OSTREAM,"kclient Buying node ... [FAILED]");
  
  result = node_make_wrapper_key(KR_PARKNODE,0,0,KR_PARKWRAP);
  if(result != RC_OK)
    kdprintf(KR_OSTREAM,"Kclient Making wrapper key ... [FAILED]");
 
  {
    eros_Number_value nkv;
    nkv.value[0] = WRAPPER_BLOCKED;
    nkv.value[1] = 0;
    nkv.value[2] = 0;
    
    node_write_number(KR_PARKNODE, WrapperFormat, &nkv);
  }
  
  process_make_start_key(krNewProc,0,KR_SCRATCH);
  process_swap_keyreg(krNewProc,KR_SHAREDSTART,KR_SCRATCH, KR_VOID);
  process_swap_keyreg(KR_SELF,KR_SHAREDSTART,KR_SCRATCH, KR_VOID);
  node_swap(KR_PARKNODE, WrapperKeeper, KR_SHAREDSTART, KR_VOID);
  
  //copy KR_PARKWRAP over
  process_copy_keyreg(KR_SELF, KR_PARKWRAP, KR_SCRATCH);
  process_swap_keyreg(krNewProc, KR_PARKWRAP, KR_SCRATCH, KR_VOID);

  //copy KR_PARKNODE over
  process_copy_keyreg(KR_SELF, KR_PARKNODE, KR_SCRATCH);
  process_swap_keyreg(krNewProc, KR_PARKNODE, KR_SCRATCH, KR_VOID);
  
  //Send to the fault key to get the process started 
  msg.snd_invKey = KR_NEWFAULT;
  result = SEND(&msg);
  if(result != RC_OK) kprintf(KR_OSTREAM,"MakeSharedPRoc SEND ... [FAILED]");

  return;
}

int
main(void) 
{
  Message msg;

  node_extended_copy(KR_CONSTIT, KC_OSTREAM, KR_OSTREAM);
  
  COPY_KEYREG(KR_ARG(0),KR_PS2READER);
  
  (void)makeSharedProc(KR_NEWPROC,0x20000u);
  
  /* We are done with all the initial setup and will now return
   * the shared start key. */
  msg.snd_invKey = KR_RETURN;
  msg.snd_key0   = KR_SHAREDSTART;
  msg.snd_key1   = KR_VOID;
  msg.snd_key2   = KR_VOID;
  msg.snd_rsmkey = KR_RETURN;
  msg.snd_data = 0;
  msg.snd_len  = 0;
  msg.snd_code = 0;
  msg.snd_w1 = 0;
  msg.snd_w2 = 0;
  msg.snd_w3 = 0;

  msg.rcv_key0   = KR_PARENT;
  msg.rcv_key1   = KR_VOID;
  msg.rcv_key2   = KR_VOID;
  msg.rcv_rsmkey = KR_RETURN;
  msg.rcv_data = 0;
  msg.rcv_limit = 0;
  msg.rcv_code = 0;
  msg.rcv_w1 = 0;
  msg.rcv_w2 = 0;
  msg.rcv_w3 = 0;

  /* Send back to our parent to get it running. Don't RETURN as
   * we need to run too */
  SEND(&msg);
  
  /* Infinitely loop getting char from ps2reader and 
   * passing it to (queuing it up at) the parent's */
  do {
  }while(ProcessRequest());
  
  return 0;
}
