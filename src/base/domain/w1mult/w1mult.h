/*
 * Copyright (C) 2008, Strawberry Development Group.
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

#include <stdint.h>
#include <string.h>
#include <eros/Link.h>
#include <eros/Invoke.h>
#include <idl/capros/Sleep.h>
#include <idl/capros/W1Bus.h>
#include <domain/Runtime.h>

#define dbg_errors 0x1
#define dbg_search 0x2
#define dbg_doall  0x4
#define dbg_server 0x8
#define dbg_thermom 0x10

/* Following should be an OR of some of the above */
#define dbg_flags   ( 0u | dbg_errors )

#define DEBUG(x) if (dbg_##x & dbg_flags)

#define KR_OSTREAM KR_APP(0)
#define KR_SLEEP   KR_APP(1)
#define KR_TIMPROC KR_APP(2)
#define KR_W1BUS   KR_APP(3)
#define KR_SNODE   KR_APP(4)
#define KR_TIMRESUME KR_APP(5)

// The family code is the low byte of the ROM ID.
enum {
  famCode_DS18B20 = 0x28,	// temperature
  famCode_DS2409  = 0x1f,	// coupler2438
  famCode_DS2438  = 0x26,	// battery monitor
  famCode_DS2450  = 0x20,	// A-D
  famCode_DS2502  = 0x09,	// a DS9097U has one of these
  famCode_DS9490R = 0x81,	// custom DS2401 in a DS9490R
};

struct W1Device;

enum {
  branchUnknown = 0,
  branchMain = capros_W1Bus_stepCode_setPathMain,
  branchAux  = capros_W1Bus_stepCode_setPathAux
};

struct Branch {
  struct W1Device * childCouplers;
  struct W1Device * childDevices;	// other than couplers
  capros_W1Bus_stepCode whichBranch;	// branchMain or branchAux, 0 for root
  bool shorted;
  bool needsWork;
};

struct W1Device {
  uint64_t rom;
  uint64_t busyUntil;	// device is busy until this time
  struct Branch * parentBranch;	// branch this device is connected to
  Link workQueueLink;
  struct W1Device * nextChild;
  struct W1Device * nextInSamplingList;
  bool sampling;
  bool callerWaiting;	// whether snode slot has a resume key for this dev
  bool found;		// device has been found on the network
  union {	// data specific to the type of device
    struct {
      capros_W1Bus_stepCode activeBranch;	// enumerated above
      struct Branch mainBranch;
      struct Branch auxBranch;
    } coupler;
    struct {
      int16_t temperature;	// in units of 1/16 degree Celsius
      uint8_t resolution;	// 1 through 4, bits after the binary point
			// 255 means the resolution hasn't been specified yet
      bool needToWriteEEPROM;
      capros_Sleep_nanoseconds_t time;	// time at which temperature was the above
		// does this need to be in absolute real time?
    } thermom;
  } u;
};

struct w1Timer {
  capros_Sleep_nanoseconds_t expiration;
  Link link;	// link in timerHead
  // On expiration, we call function(arg)
  void * arg;
  void (*function)(void * arg);	
};

struct W1Device * BranchToCoupler(struct Branch * br);

// Bits in heartbeatDisable:
#define hbBit_DS18B20 0x01
#define hbBit_All     0x01
void DisableHeartbeat(uint32_t bit);
void EnableHeartbeat(uint32_t bit);

// Stuff for programming the 1-Wire bus:
extern unsigned char outBuf[capros_W1Bus_maxProgramSize + 1];
extern unsigned char * const outBeg;
extern unsigned char * outCursor;
extern unsigned char inBuf[capros_W1Bus_maxResultsSize];
extern struct Branch * resetBranch;
extern Message RunPgmMsg;

// Append a byte to the program.
#define wp(b) *outCursor++ = (b);

uint8_t CalcCRC8(uint8_t * data, unsigned int len);

void ClearProgram(void);
void ProgramReset(void);
void AddressDevice(struct W1Device * dev);
void ProgramMatchROM(struct W1Device * dev);
void WriteOneByte(uint8_t b);
int RunProgram(void);

static inline bool
ProgramIsClear(void)
{
  return outCursor == outBeg;
}

static inline void
CopyToProgram(void * data, unsigned int len)
{
  memcpy(outCursor, data, len);
  outCursor += len;
}

// Indicate that the program does not end with a reset.
static inline void
NotReset(void)
{
  resetBranch = NULL;
}

extern capros_Sleep_nanoseconds_t currentTime;
void RecordCurrentTime(void);
void InsertTimer(struct w1Timer * timer);

/* We will sample at least every 2**7 seconds (about 2 minutes). */
#define maxLog2Seconds 7

extern struct Branch root;

void MarkForSampling(uint32_t hbCount, Link * workQueues,
  struct W1Device * * samplingListHead);
void MarkSamplingList(struct W1Device * dev);
extern void (*DoAllWorkFunction)(struct Branch * br);
void DoAll(struct Branch * br);
extern void (*DoEachWorkFunction)(struct W1Device * dev);
void DoEach(struct Branch * br);
void EnsureBranchSmartReset(struct Branch * br);
