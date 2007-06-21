/*
 * Copyright (C) 1998, 1999, 2001, Jonathan S. Shapiro.
 * Copyright (C) 2007, Strawberry Development Group.
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

#include <assert.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <eros/target.h>
#include <erosimg/Intern.h>
#include <erosimg/ExecArch.h>
#include <erosimg/ErosImage.h>
#include <erosimg/Parse.h>
#include <erosimg/App.h>
#include <erosimg/DiskKey.h>

#include <eros/KeyConst.h>
#include <eros/ProcessKey.h>
#include <eros/ProcessState.h>
#include <disk/DiskLSS.h>

#include <idl/eros/Range.h>

/* Following is included as a special case so that AddProcess()
   register conventions stay in sync with the expectations of the
   runtime libraries. */
#include "../../../lib/domain/include/domain/Runtime.h"

#ifndef max
#define max(x,y) ( ((x) > (y)) ? (x) : (y) )
#endif

#define PAGE_ALLOC_QUANTA 16
#define NODE_ALLOC_QUANTA 16
#define DIR_ALLOC_QUANTA 16
#define THREAD_ALLOC_QUANTA 16

#ifdef __cplusplus
extern "C" {
#endif
  extern void PrintDiskKey(KeyBits);
#ifdef __cplusplus
}
#endif

int
ei_GetAnyBlss(const ErosImage * ei, KeyBits key)
{
  if (keyBits_IsVoidKey(&key)) return EROS_PAGE_BLSS;
  return keyBits_GetBlss(&key);
}

void
erosheader_init(ErosHeader *eh)
{
  /* First initialize the domain file image header: */
  memset(eh->signature, 0, 8);
  strcpy(eh->signature, "ErosImg");
  eh->imageByteSex = 0;

  eh->version = EROS_IMAGE_VERSION;
  eh->architecture = ExecArch_unknown;

  eh->nDirEnt = 0;
  eh->nStartups = 0;
  eh->nPages = 0;
  eh->nZeroPages = 0;
  eh->nNodes = 0;
  eh->strSize = 0;

  eh->dirOffset = sizeof(ErosHeader);
  eh->startupsOffset = sizeof(ErosHeader);
  eh->pageOffset = sizeof(ErosHeader);
  eh->nodeOffset = sizeof(ErosHeader);
  eh->strTableOffset = sizeof(eh->strTableOffset);
}

ErosImage *
ei_create()
{
  KeyBits key;
  ErosImage *ei = (ErosImage *) malloc(sizeof(ErosImage));

  erosheader_init(&ei->hdr);
  ei->pool = strpool_create();
  
  ei->pageImages = 0;
  ei->nodeImages = 0;
  ei->dir = 0;
  ei->startupsDir = 0;

  ei->maxPage = 0;
  ei->maxNode = 0;
  ei->maxDir = 0;
  ei->maxStartups = 0;

  key = ei_AddNode(ei, false);
  ei_AddDirEnt(ei, "volsize", key);

  key = ei_AddNode(ei, false);
  ei_AddDirEnt(ei, "threadlist", key);

  return ei;
}

void
ei_destroy(ErosImage *ei)
{
  strpool_destroy(ei->pool);
  free(ei->pageImages);
  free(ei->nodeImages);
  free(ei->dir);
  free(ei->startupsDir);
}

const char *
ei_GetString(const ErosImage *ei, int ndx)
{
  return intern(strpool_Get(ei->pool, ndx));
}

static void
ei_GrowStartupsTable(ErosImage *ei, uint32_t newMax)
{
  if (ei->maxStartups < newMax) {
    EiDirent *newThreadDir;

    ei->maxStartups = newMax;
    ei->maxStartups += (THREAD_ALLOC_QUANTA - 1);
    ei->maxStartups -= (ei->maxStartups % THREAD_ALLOC_QUANTA);

    newThreadDir = 
      (EiDirent *)malloc(sizeof(EiDirent) * ei->maxStartups);
    if (ei->startupsDir)
      memcpy(newThreadDir, ei->startupsDir, ei->hdr.nStartups * sizeof(EiDirent)); 

    free(ei->startupsDir);
    ei->startupsDir = newThreadDir;
  }
}

void
ei_GrowDirTable(ErosImage *ei, uint32_t newMax)
{
  if (ei->maxDir < newMax) {
    EiDirent *newDir;

    ei->maxDir = newMax;
    ei->maxDir += (DIR_ALLOC_QUANTA - 1);
    ei->maxDir -= (ei->maxDir % DIR_ALLOC_QUANTA);

    newDir = 
      (EiDirent *)malloc(sizeof(EiDirent) * ei->maxDir);

    if (ei->dir)
      memcpy(newDir, ei->dir, ei->hdr.nDirEnt * sizeof(EiDirent)); 

    free(ei->dir);
    ei->dir = newDir;
  }
}

void
ei_GrowNodeTable(ErosImage *ei, uint32_t newMax)
{
  if (ei->maxNode < newMax) {
    DiskNodeStruct *newNodeImages;
    unsigned i;
    ei->maxNode = newMax;
    ei->maxNode += (NODE_ALLOC_QUANTA - 1);
    ei->maxNode -= (ei->maxNode % NODE_ALLOC_QUANTA);

    newNodeImages = 
      (DiskNodeStruct *) malloc(sizeof(DiskNodeStruct) * ei->maxNode);

    for (i = 0; i < ei->maxNode; i++)
      init_DiskNode(&newNodeImages[i]);

    if (ei->nodeImages)
      memcpy(newNodeImages, ei->nodeImages, ei->hdr.nNodes * sizeof(DiskNodeStruct)); 

    free(ei->nodeImages);
    ei->nodeImages = newNodeImages;
  }
}

void
ei_GrowPageTable(ErosImage *ei, uint32_t newMax)
{
  if (ei->maxPage < newMax) {
    uint8_t *newPageImages;

    ei->maxPage = newMax;
    ei->maxPage += (PAGE_ALLOC_QUANTA - 1);
    ei->maxPage -= (ei->maxPage % PAGE_ALLOC_QUANTA);

    newPageImages = malloc(ei->maxPage * EROS_PAGE_SIZE);
    if (ei->pageImages)
      memcpy(newPageImages, ei->pageImages, ei->hdr.nPages * EROS_PAGE_SIZE); 

    free(ei->pageImages);
    ei->pageImages = newPageImages;
  }
}

bool
ei_AddStartup(ErosImage *ei, const char *name, KeyBits key)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);
  KeyBits threadChain;
  keyBits_InitToVoid(&threadChain);

  if (keyBits_IsType(&key, KKT_Process) || keyBits_IsType(&key, KKT_Node)) {
    if (ei_GetProcessState(ei, key) == RS_Waiting) {
      diag_printf("Attempt to re-start process that is already started.\n");
      return false;
    }

    ei_SetProcessState(ei, key, RS_Waiting);
  }
  else if (keyBits_IsType(&key, KKT_Start)) {
    /* Arrangements should already have been made to get the target
       process running, in which case it will be in the waiting state: */
    if (ei_GetProcessState(ei, key) != RS_Waiting) {
      diag_printf("Attempt to invoke constructor that has not been started.\n");
      return false;
    }
  }
  else {
    diag_printf("Bad key type to AddThead()\n");
    return false;
  }

  for (i = 0; i < ei->hdr.nStartups; i++)
    if (ei->startupsDir[i].name == nameNdx)
      diag_fatal(5, "Duplicate name \"%s\" added to image file\n", name);
  
  if (ei->hdr.nStartups >= ei->maxStartups)
    ei_GrowStartupsTable(ei, ei->maxStartups + THREAD_ALLOC_QUANTA);

  ei->startupsDir[ei->hdr.nStartups].key = key;
  ei->startupsDir[ei->hdr.nStartups].name = nameNdx;

  assert ( ei_GetDirEnt(ei, "threadlist", &threadChain) );
  
  ei_AppendToChain(ei, &threadChain, key);

  ei->hdr.nStartups++;

  return true;
}

void
ei_AddDirEnt(ErosImage *ei, const char *name, KeyBits key)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);

  assert ( name != 0 );
  
  for (i = 0; i < ei->hdr.nDirEnt; i++)
    if (ei->dir[i].name == nameNdx)
      diag_fatal(5, "Duplicate name \"%s\" added to image file\n", name);
  
  if (ei->hdr.nDirEnt >= ei->maxDir)
    ei_GrowDirTable(ei, ei->maxDir + DIR_ALLOC_QUANTA);

  ei->dir[ei->hdr.nDirEnt].key = key;
  ei->dir[ei->hdr.nDirEnt].name = nameNdx;
  ei->hdr.nDirEnt++;
}

void
ei_AssignDirEnt(ErosImage *ei, const char *name, KeyBits key)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);

  for (i = 0; i < ei->hdr.nDirEnt; i++)
    if (ei->dir[i].name == nameNdx) {
      ei->dir[i].key = key;
      return;
    }
  
  if (ei->hdr.nDirEnt >= ei->maxDir)
    ei_GrowDirTable(ei, ei->maxDir + DIR_ALLOC_QUANTA);

  ei->dir[ei->hdr.nDirEnt].key = key;
  ei->dir[ei->hdr.nDirEnt].name = nameNdx;
  ei->hdr.nDirEnt++;
}

bool
ei_DelDirEnt(ErosImage *ei, const char *name)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);

  for (i = 0; i < ei->hdr.nDirEnt; i++) {
    if (ei->dir[i].name == nameNdx) {
      uint32_t ent;
      for (ent = i; ent < (ei->hdr.nDirEnt - 1); ent++)
	ei->dir[ent] = ei->dir[ent+1];
      ei->hdr.nDirEnt--;
      return true;
    }
  }

  return false;
}

bool
ei_GetDirEnt(ErosImage *ei, const char *name, KeyBits *key)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);

  for (i = 0; i < ei->hdr.nDirEnt; i++) {
    if (ei->dir[i].name == nameNdx) {
      *key = ei->dir[i].key;
      return true;
    }
  }

  return false;
}

bool
ei_GetStartupEnt(ErosImage *ei, const char *name, KeyBits *key)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);

  for (i = 0; i < ei->hdr.nStartups; i++) {
    if (ei->startupsDir[i].name == nameNdx) {
      *key = ei->startupsDir[i].key;
      return true;
    }
  }

  return false;
}

void
ei_SetDirEnt(ErosImage *ei, const char *name, KeyBits key)
{
  uint32_t i;
  uint32_t nameNdx = strpool_Add(ei->pool, name);

  for (i = 0; i < ei->hdr.nDirEnt; i++) {
    if (ei->dir[i].name == nameNdx) {
      ei->dir[i].key = key;
      return;
    }
  }

  diag_fatal(1, "No directory entry for \"%s\"\n", name);
}

KeyBits
ei_AddZeroDataPage(ErosImage *ei, bool readOnly)
{
  OID oid = ei->hdr.nZeroPages++;
  KeyBits key;

  init_DataPageKey(&key, oid, readOnly);
  keyBits_SetPrepared(&key);	// prepared bit means it's a zero page

  return key;
}

KeyBits
ei_AddDataPage(ErosImage *ei, const uint8_t *buf, bool readOnly)
{
  if (ei->hdr.nPages >= ei->maxPage)
    ei_GrowPageTable(ei, ei->maxPage + PAGE_ALLOC_QUANTA);

  memcpy(&ei->pageImages[ei->hdr.nPages * EROS_PAGE_SIZE], buf, EROS_PAGE_SIZE);
  
  {
    KeyBits key;
    OID  oid = ei->hdr.nPages++;

    init_DataPageKey(&key, oid, readOnly);
    return key;
  }
}

void
ei_GetDataPageContent(const ErosImage *ei, uint32_t ndx, uint8_t* buf)
{
  if (ndx >= ei->hdr.nPages)
    diag_fatal(5, "Not that many pages in image file\n");
  
  memcpy(buf, &ei->pageImages[ndx*EROS_PAGE_SIZE], EROS_PAGE_SIZE);
}

void
ei_GetNodeContent(const ErosImage *ei, uint32_t ndx, DiskNodeStruct *pNode)
{
  if (ndx >= ei->hdr.nNodes)
    diag_fatal(5, "Not that many nodes in image file\n");
  
  memcpy(pNode, &ei->nodeImages[ndx], sizeof(DiskNodeStruct));
}

KeyBits
ei_AddNode(ErosImage *ei, bool readOnly)
{
  DiskNodeStruct *pNode;
  OID oid;
  unsigned i;

  if (ei->hdr.nNodes >= ei->maxNode)
    ei_GrowNodeTable(ei, ei->maxNode + NODE_ALLOC_QUANTA);

  pNode = &ei->nodeImages[ei->hdr.nNodes];
  oid = ei->hdr.nNodes++;
  pNode->allocCount = 0;
  pNode->callCount = 0;
  pNode->oid = oid;

  for (i = 0; i < EROS_NODE_SIZE; i++)
    keyBits_InitToVoid(&pNode->slot[i]);

  {
    KeyBits key;
    init_NodeKey(&key, oid, readOnly);

    return key;
  }
}

KeyBits
ei_AddChain(ErosImage *ei)
{
  return ei_AddNode(ei, false);
}

void
ei_AppendToChain(ErosImage *ei, KeyBits *chain, KeyBits k)
{
  unsigned ndx = 0;

  for (;;) {
    KeyBits slot = ei_GetNodeSlot(ei, *chain, EROS_NODE_SIZE - 1);
    if (keyBits_IsVoidKey(&slot))
      break;
    *chain = slot;
  }
  
  for (ndx = 0; ndx < EROS_NODE_SIZE; ndx++) {
    KeyBits slot = ei_GetNodeSlot(ei, *chain, ndx);
    if (keyBits_IsVoidKey(&slot))
      break;
  }

  if (ndx == EROS_NODE_SIZE - 1) {
    KeyBits newNode = ei_AddNode(ei, false);
    ei_SetNodeSlot(ei, *chain, EROS_NODE_SIZE - 1, newNode);

    *chain = newNode;
    ndx = 0;
  }

  ei_SetNodeSlot(ei, *chain, ndx, k);
}

#if 0
void
ei_Import(ErosImage *to, const ErosImage *from);
{
  /* Step 1: Grow the various tables to hold the new entries: */

  GrowDirTable(nDirEnt + image.nDirEnt);
  GrowStartupsTable(nStartups + image.nStartups);
  GrowPageTable(nPages + image.nPages);
  GrowNodeTable(nNodes + image.nNodes-1);

  /* Step 2: Append the other image's pages and nodes WITHOUT bumping
   * our counts:
   */
  
  memcpy(&pageImages[nPages], image.pageImages,
	 image.nPages * EROS_PAGE_SIZE);
  /* Skip the first node, which is the volsize node: */
  memcpy(&nodeImages[nNodes], image.nodeImages + 1,
	 (image.nNodes - 1) * sizeof(DiskNode));

  /* Step 3: Relocate the newly added nodes and directory entries: */
  for (uint32_t ndx = 0; ndx < image.nNodes; ndx++) {
    DiskNode& node = nodeImages[ndx + nNodes];

    for (uint32_t slot = 0; slot < EROS_NODE_SIZE; slot++) {
      KeyBits& key = node[slot];
      
      /* If the key's OID needs to be adjusted, do so: */
      if (key.IsType(KKT_Page) && key.IsPrepared() == false)
	key.unprep.oid += nPages;
      else if (key.IsType(KKT_Page) && key.IsPrepared())
	key.unprep.oid += nZeroPages;
      else if (key.IsNodeKeyType())
	key.unprep.oid += (nNodes - 1);
    }
  }

  /* Step 4: add the foreign directory table entries: */
  for (uint32_t ndx = 0; ndx < image.nDirEnt; ndx++) {
    EiDirent& dirEnt = image.dir[ndx];
    KeyBits key = dirEnt.key;

    /* Check the string table entry: */
    InternedString name = image.GetString(dirEnt.name);
    InternedString vs("volsize");
    
    if (name == vs)
      continue;
    
    /* If the key's OID needs to be adjusted, do so: */
    if (key.IsType(KKT_Page) && key.IsPrepared() == false)
      key.unprep.oid += nPages;
    else if (key.IsType(KKT_Page) && key.IsPrepared())
      key.unprep.oid += nZeroPages;
    else if (key.IsNodeKeyType())
      key.unprep.oid += (nNodes - 1);

    /* Add the resulting entry to our directory using the AddDirEnt
     * routine in order to check for collisions.
     */
    AddDirEnt(name, key);
  }

  /* Step 5: add the foreign thread table entries: */
  for (uint32_t ndx = 0; ndx < image.nStartups; ndx++) {
    EiDirent& threadEnt = image.startupsDir[ndx];
    KeyBits key = threadEnt.key;
    InternedString name = image.GetString(threadEnt.name);

    /* If the key's OID needs to be adjusted, do so: */
    if (key.IsType(KKT_Page) && key.IsPrepared() == false)
      key.unprep.oid += nPages;
    else if (key.IsType(KKT_Page) && key.IsPrepared())
      key.unprep.oid += nZeroPages;
    else if (key.IsNodeKeyType())
      key.unprep.oid += (nNodes - 1);

    /* Add the resulting entry to our directory using the AddDirEnt
     * routine in order to check for collisions.
     */
    AddStartup(name, key);
  }

  /* Note that any strings in the foreign image that were really
   * referenced were copied somewhere in the nonsense above!
   */
  
  /* Step 5: Update the count fields that are not already up to date: */
  nPages += image.nPages;
  nZeroPages += image.nZeroPages;
  nNodes += image.nNodes;
}
#endif

KeyBits
ei_GetNodeSlot(const ErosImage *ei, KeyBits nodeKey, uint32_t slot)
{
  uint32_t ndx;

  if (slot >= EROS_NODE_SIZE)
    diag_fatal(5,"Slot value too high\n");
  
  if (keyBits_IsNodeKeyType(&nodeKey) == false)
    diag_fatal(5,"GetNodeSlot expects node key!\n");

  ndx = nodeKey.u.unprep.oid;
  return ei->nodeImages[ndx].slot[slot];
}

KeyBits
ei_GetNodeSlotFromIndex(const ErosImage *ei, uint32_t nodeNdx, uint32_t slot)
{
  if (slot >= EROS_NODE_SIZE)
    diag_fatal(5,"Slot value too high\n");
  
  return ei->nodeImages[nodeNdx].slot[slot];
}

void
ei_SetNodeSlot(ErosImage *ei, KeyBits nodeKey, uint32_t slot,
	       KeyBits key)
{
  uint32_t ndx;

  if (slot >= EROS_NODE_SIZE)
    diag_fatal(5,"Slot value too high\n");
  
  if (keyBits_IsNodeKeyType(&nodeKey) == false)
    diag_fatal(5,"GetNodeSlot expects node key!\n");

  ndx = nodeKey.u.unprep.oid;
  ei->nodeImages[ndx].slot[slot] = key;
}

void
ei_SetArchitecture(ErosImage *ei, enum ExecArchitecture arch)
{
  const ExecArchInfo *ai = ExecArch_GetArchInfo(arch);
  ei->hdr.imageByteSex = ai->byteSex;
  ei->hdr.architecture = arch;
}

void
ei_ValidateImage(ErosImage *ei, const char* target)
{
  DiskNodeStruct *pNode;

  if (ei->hdr.nDirEnt == 0)
    diag_fatal(2, "No directory entries in \"%s\"!\n", target);

  if (!ei->nodeImages)
    diag_fatal(2, "Missing volsize node!\n", target);

  pNode = &ei->nodeImages[0];
  init_SmallNumberKey(&pNode->slot[eros_Range_otNode], ei->hdr.nNodes);
  init_SmallNumberKey(&pNode->slot[eros_Range_otPage], ei->hdr.nPages + ei->hdr.nZeroPages);
}

void
ei_WriteToFile(ErosImage *ei, const char *target)
{
  int tfd;
  int sz;

  ei_ValidateImage(ei, target);
  
  tfd = open(target, O_RDWR|O_TRUNC);
  if (tfd < 0 && errno == ENOENT) {
    tfd = open(target, O_RDWR|O_CREAT, 0666);
    app_AddTarget(target);
  }
  
  if (tfd < 0)
    diag_fatal(2, "Unable to open target file \"%s\"\n", target);

  /* Pin down what all the offsets will be.  We will write the pieces
   * in the following order:
   * 
   *    image header
   * 	image directory
   *    page content images
   *    node content images
   *    string table
   * 
   */

  ei->hdr.dirOffset = sizeof(ErosHeader);
  ei->hdr.startupsOffset = ei->hdr.dirOffset;
  ei->hdr.startupsOffset += ei->hdr.nDirEnt * sizeof(EiDirent);
  ei->hdr.pageOffset = ei->hdr.startupsOffset;
  ei->hdr.pageOffset += ei->hdr.nStartups * sizeof(EiDirent);
  ei->hdr.nodeOffset = ei->hdr.pageOffset;
  ei->hdr.nodeOffset += ei->hdr.nPages * EROS_PAGE_SIZE;
  ei->hdr.strTableOffset = ei->hdr.nodeOffset;
  ei->hdr.strTableOffset += ei->hdr.nNodes * sizeof(DiskNodeStruct);
  ei->hdr.strSize = strpool_Size(ei->pool);

  sz = sizeof(ErosHeader);
  if (write(tfd, &ei->hdr, sz) != sz)
    diag_fatal(3, "Unable to write image file header\n");

  sz = ei->hdr.nDirEnt * sizeof(EiDirent);
  if (write(tfd, ei->dir, sz) != sz)
    diag_fatal(3, "Unable to write image directory\n");

  sz = ei->hdr.nStartups * sizeof(EiDirent);
  if (write(tfd, ei->startupsDir, sz) != sz)
    diag_fatal(3, "Unable to write image thread list\n");

  sz = ei->hdr.nPages * EROS_PAGE_SIZE;
  if (write(tfd, ei->pageImages, sz) != sz)
    diag_fatal(3, "Unable to write page images\n");

  sz = ei->hdr.nNodes * sizeof(DiskNodeStruct);
  if (write(tfd, ei->nodeImages, sz) != sz)
    diag_fatal(3, "Unable to write node images\n");

  if (strpool_WriteToFile(ei->pool, tfd) != (int) ei->hdr.strSize)
    diag_fatal(3, "Unable to write string pool\n");

  close(tfd);
}

void
ei_ReadFromFile(ErosImage *ei, const char *source)
{
  int sfd;
  struct stat statbuf;
  int expect;
  int sz;

  ei->hdr.nNodes = 0;
  ei->hdr.nPages = 0;
  ei->hdr.nZeroPages = 0;
  free(ei->pageImages);
  ei->pageImages = 0;
  free(ei->nodeImages);
  ei->nodeImages = 0;
  ei->hdr.nDirEnt = 0;
  free(ei->dir);
  ei->hdr.nStartups = 0;
  free(ei->startupsDir);
  
  sfd = open(source, O_RDONLY);
  if (sfd < 0)
    diag_fatal(2, "Unable to open domain file \"%s\"\n", source);

  if (fstat(sfd, &statbuf) < 0)
    diag_fatal(2, "Can't stat domain file \"%s\"\n", source);
    
  /* Step 1: read the image file header: */
  
  if (read(sfd, &ei->hdr, sizeof(ErosHeader)) != sizeof(ErosHeader))
    diag_fatal(2, "Cannot read image header from \"%s\"\n", source);

  /* Step 1b: verify that the actual length of the file matches our
   * expectations:
   */

  if (ei->hdr.version != EROS_IMAGE_VERSION)
    diag_fatal(2, "Image file version in \"%s\" is obsolete\n", source);
    
  expect = sizeof(ErosHeader);
  expect += ei->hdr.nDirEnt * sizeof(EiDirent);
  expect += ei->hdr.nStartups * sizeof(EiDirent);
  expect += ei->hdr.nPages * EROS_PAGE_SIZE;
  expect += ei->hdr.nNodes * sizeof(DiskNodeStruct);
  expect += ei->hdr.strSize;

  if (expect != statbuf.st_size)
    diag_fatal(2, "Domain image file \"%s\" is malsized.\n", source);

  /* Step 2: read image directory: */

  ei->maxDir = ei->hdr.nDirEnt;
  if (ei->maxDir % DIR_ALLOC_QUANTA) {
    ei->maxDir -= (ei->maxDir % DIR_ALLOC_QUANTA);
    ei->maxDir += DIR_ALLOC_QUANTA;
  }

  ei->dir = (EiDirent *)malloc(sizeof(EiDirent) * ei->maxDir);
  sz = ei->hdr.nDirEnt * sizeof(EiDirent);
  
  if (lseek(sfd, ei->hdr.dirOffset, SEEK_SET) < 0)
    diag_fatal(2, "Cannot seek to image directory in \"%s\"\n", source);

  if (read(sfd, ei->dir, sz) != sz)
    diag_fatal(2, "Cannot read image directory from \"%s\"\n", source);
  
  /* Step 3: read thread directory: */

  ei->maxStartups = ei->hdr.nStartups;
  if (ei->maxStartups % THREAD_ALLOC_QUANTA) {
    ei->maxStartups -= (ei->maxStartups % THREAD_ALLOC_QUANTA);
    ei->maxStartups += THREAD_ALLOC_QUANTA;
  }

  ei->startupsDir = (EiDirent *)malloc(sizeof(EiDirent) * ei->maxStartups);
  sz = ei->hdr.nStartups * sizeof(EiDirent);
  
  if (lseek(sfd, ei->hdr.startupsOffset, SEEK_SET) < 0)
    diag_fatal(2, "Cannot seek to thread list in \"%s\"\n", source);

  if (read(sfd, ei->startupsDir, sz) != sz)
    diag_fatal(2, "Cannot read thread list from \"%s\"\n", source);
  
  /* Step 4: read page images: */

  ei->maxPage = ei->hdr.nPages;
  if (ei->maxPage % PAGE_ALLOC_QUANTA) {
    ei->maxPage -= (ei->maxPage % PAGE_ALLOC_QUANTA);
    ei->maxPage += PAGE_ALLOC_QUANTA;
  }

  ei->pageImages = malloc(ei->maxPage * EROS_PAGE_SIZE);
  sz = ei->hdr.nPages * EROS_PAGE_SIZE;
  
  if (lseek(sfd, ei->hdr.pageOffset, SEEK_SET) < 0)
    diag_fatal(2, "Cannot seek to page images in \"%s\"\n", source);

  if (read(sfd, ei->pageImages, sz) != sz)
    diag_fatal(2, "Cannot read page images from \"%s\"\n", source);
  
  /* Step 5: read cap page images: DELETED */

  /* Step 6: read node images: */

  ei->maxNode = ei->hdr.nNodes;
  if (ei->maxNode % NODE_ALLOC_QUANTA) {
    ei->maxNode -= (ei->maxNode % NODE_ALLOC_QUANTA);
    ei->maxNode += NODE_ALLOC_QUANTA;
  }

  {
    unsigned i;
    ei->nodeImages = (DiskNodeStruct *) malloc(sizeof(DiskNodeStruct) * ei->maxNode);

    for (i = 0; i < ei->maxNode; i++)
      init_DiskNode(&ei->nodeImages[i]);
  }
  sz = ei->hdr.nNodes * sizeof(DiskNodeStruct);
  
  if (lseek(sfd, ei->hdr.nodeOffset, SEEK_SET) < 0)
    diag_fatal(2, "Cannot seek to node images in \"%s\"\n", source);

  if (read(sfd, ei->nodeImages, sz) != sz)
    diag_fatal(2, "Cannot read node images from \"%s\"\n", source);
  
  /* Step 7: read string pool: */

  if (lseek(sfd, ei->hdr.strTableOffset, SEEK_SET) < 0)
    diag_fatal(2, "Cannot seek to string table in \"%s\"\n", source);

  if (strpool_ReadFromFile(ei->pool, sfd, ei->hdr.strSize) == false)
    diag_fatal(2, "Cannot load string table from \"%s\"\n", source);

  close(sfd);
}

/* Given a segmode key that is the root of a segment tree, add the
 * specified page key at the specified offset in that segment,
 * inserting any needed nodes along the way.
 * 
 * DO NOT traverse a red segment boundary to do so - if you need to
 * add something to the contained segment, keep a handle to it and add
 * it directly.
 * 
 * Returns a key to the new segment root.  If the segment has grown,
 * this may not be a key to the same node as the original segment key.
 * 
 * The red segment expansion code found here assumes that it will not
 * encounter an oversize subsegment.  I'll have to get that right
 * (whatever that means) in the fault handling domain, but it doesn't
 * seem necessary here.  You can still install an oversized subsegment
 * if you do it with a SEGMENT key as opposed to a node key -- the
 * insertion code will not cross a segment boundary, because segments
 * are supposed to be opaque.
 * 
 */
static KeyBits
ei_DoAddSubsegToBlackSegment(ErosImage *ei, KeyBits segRoot,
			   uint64_t segOffset,
			   KeyBits segKey,
			   uint64_t path,
			   bool expandRedSegment)
{
  uint32_t rootBLSS = ei_GetAnyBlss(ei, segRoot);
  uint32_t segBLSS = ei_GetAnyBlss(ei, segKey);
  uint32_t segOffsetBLSS;
  if (segOffset == 0) {
    segOffsetBLSS = segBLSS;
  } else {
    if (segOffset & lss_Mask(segBLSS)) {
      diag_fatal(4, "AddPageToSegment: seg cannot be aligned to offset\n");
    }
    segOffsetBLSS = lss_BiasedLSS(segOffset);
  }
  // Now segBLSS <= segOffsetBLSS.
  
#if 0
  diag_printf("AddSubseg, lss root=%d ofs=%d seg=%d\n",
             rootBLSS, segOffsetBLSS, segBLSS);
  diag_debug(2, "AddSubseg, lss root=%d ofs=%d seg=%d\n",
             rootBLSS, segOffsetBLSS, segBLSS);
#endif

  if ( keyBits_IsType(&segRoot, KKT_Wrapper) || keyBits_IsType(&segRoot, KKT_Segment) )
    diag_fatal(4, "AddPageToSegment: Cannot traverse subsegment\n");

  if (rootBLSS < segOffsetBLSS) {
    /* Inserting a segment whose offset BLSS is too large - need to
     * grow a new root.
     */

    KeyBits newRoot = ei_AddNode(ei, false);
    keyBits_SetBlss(&newRoot, segOffsetBLSS);

    if (expandRedSegment) {
      unsigned i;
      uint64_t pathMask = lss_Mask(segOffsetBLSS);
      uint64_t slotPath = path & ~pathMask;
      uint64_t slotIncr = 1;

      diag_printf("Expanding red seg...\n");
      slotPath |= 3u;		/* background window key */
      slotIncr <<= segOffsetBLSS * EROS_NODE_LGSIZE;

      for (i = 0; i < EROS_NODE_SIZE; i++) {
	uint64_t bkWindowValue = slotPath;
	
	KeyBits slotKey;
	init_NumberKey(&slotKey, 
		       (uint32_t) (bkWindowValue),
		       (uint32_t) (bkWindowValue >> 32),
		       0 );
	ei_SetNodeSlot(ei, newRoot, i, slotKey);
	slotPath += slotIncr;
      }
    }
    
    ei_SetNodeSlot(ei, newRoot, 0, segRoot);
    
    return ei_DoAddSubsegToBlackSegment(ei, newRoot, segOffset, segKey, path,
				      expandRedSegment);
  }
  // Now segOffsetBLSS <= rootBLSS.
  // Therefore segBLSS <= segOffsetBLSS <= rootBLSS.

  /* The segment key might replace the current key: */
  if (rootBLSS == segBLSS)
    return segKey;

  if ((rootBLSS -1) == segBLSS) {
    // Seg fits exactly in a slot of root.
    uint32_t slot = lss_SlotNdx(segOffset, rootBLSS);
    ei_SetNodeSlot(ei, segRoot, slot, segKey);
    return segRoot;
  }

  // Traverse deeper in the tree.
  uint32_t slot = lss_SlotNdx(segOffset, rootBLSS);
  uint64_t subSegOffset = segOffset & lss_Mask(rootBLSS -1);
  KeyBits subSeg = ei_GetNodeSlot(ei, segRoot, slot);

  KeyBits newSlotKey =
    ei_DoAddSubsegToBlackSegment(ei, subSeg, subSegOffset, segKey, path,
		 expandRedSegment);

  ei_SetNodeSlot(ei, segRoot, slot, newSlotKey);
  return segRoot;
}

/* 
If segKey designates a page, the page must be in the image.
If segKey designates a node, the node must be in the image
  and the key must be a Node, Segment, or Wrapper key.
segKey may be a void key.
Other miscellaneous keys are not checked.
 */
static void
ValidateSegKey(const ErosImage *ei, KeyBits segKey)
{
  if (keyBits_IsNodeKeyType(&segKey) && segKey.u.unprep.oid >= (OID)ei->hdr.nNodes)
    diag_fatal(4, "Segment node not in image file\n");
  else if (keyBits_IsType(&segKey, KKT_Page)) {
    if (keyBits_IsPrepared(&segKey)) {
      if (segKey.u.unprep.oid >= (OID)ei->hdr.nZeroPages)
        diag_fatal(4, "Segment page not in image file\n");
    }
    else if (segKey.u.unprep.oid >= (OID)ei->hdr.nPages)
      diag_fatal(4, "Segment page not in image file\n");
  }

  if (keyBits_IsType(&segKey, KKT_Process))
    diag_fatal(4, "ValidateSegKey: Domain key not valid for segment\n");
  if (keyBits_IsType(&segKey, KKT_Start))
    diag_fatal(4, "ValidateSegKey: Start key not valid for segment\n");
  if (keyBits_IsType(&segKey, KKT_Resume))
    diag_fatal(4, "ValidateSegKey: Resume key not valid for segment\n");
}

/* Outputs in blackRootKey a copy of segRoot, except that if segRoot
   is red, blackRootKey is black. */
static void
PrepareToAddObjectToSegment(ErosImage * ei,
                   KeyBits segRoot,
		   uint64_t segOffset,
		   KeyBits objKey,
		   KeyBits *blackRootKey)
{
  uint32_t segOffsetBLSS;
  uint32_t rootBLSS;
  uint32_t segBLSS;

  ValidateSegKey(ei, segRoot);

  segOffsetBLSS = lss_BiasedLSS(segOffset);
  rootBLSS = ei_GetAnyBlss(ei, segRoot);
  segBLSS = ei_GetAnyBlss(ei, objKey);

  if (! keyBits_IsVoidKey(&segRoot)) {
    if (segOffsetBLSS <= segBLSS &&
        rootBLSS <= segOffsetBLSS)
      diag_fatal(4, "Inserted object and offset would replace entire existing segment.\n");
  }

  /* It is permissable for the root segment node to be a red segment.
   * If so, we must verify that we are not about to get ourselves in
   * trouble by growing the red segment to suitable size and fabricate
   * a black segment key to pass down into the actual insertion
   * routine:
   */

  *blackRootKey = segRoot;
  
  if ( keyBits_IsType(&segRoot, KKT_Wrapper) ) {
    KeyBits fmtKey = ei_GetNodeSlot(ei, segRoot, WrapperFormat);
    uint32_t newSegBlss;
    uint32_t initialSlots;
    uint32_t slot;

    rootBLSS = WRAPPER_GET_BLSS(fmtKey.u.nk);
    newSegBlss = max(rootBLSS, segOffsetBLSS);
    initialSlots = 1;

    slot = lss_SlotNdx(segOffset, segOffsetBLSS);
    if (slot >= initialSlots)
      newSegBlss++;

    if (newSegBlss > rootBLSS) {
      /* must grow the red segment by rewriting the format key: */
      WRAPPER_SET_BLSS(fmtKey.u.nk, newSegBlss);
      ei_SetNodeSlot(ei, segRoot, WrapperFormat, fmtKey);
    }

    /* Now fabricate a black segment key to the segment's root node
     * whose BLSS matches that of the red segment:
     */
    
    keyBits_SetBlss(blackRootKey,newSegBlss);
  }
}

KeyBits
ei_AddSubsegToSegment(ErosImage *ei, KeyBits segRoot,
		      uint64_t segOffset,
		      KeyBits segKey)
{
  KeyBits rootKey;
  KeyBits newSegRoot;

  ValidateSegKey(ei, segKey);

  if (! keyBits_IsType(&segKey, KKT_Node) &&
      ! keyBits_IsType(&segKey, KKT_Segment) &&
      ! keyBits_IsType(&segKey, KKT_Page) &&
      ! keyBits_IsVoidKey(&segKey) )
    diag_fatal(4, "AddSubsegToSegment: added subseg must be segment, "
		"segtree, page, or void\n");

  keyBits_InitToVoid(&rootKey);

  PrepareToAddObjectToSegment(ei, segRoot, segOffset, segKey,
                              &rootKey /* output */ );

  bool wasSeg = keyBits_IsType(&rootKey, KKT_Segment);
  if (wasSeg) {
    keyBits_SetType(&rootKey, KKT_Node);
  }
  
  newSegRoot =
    ei_DoAddSubsegToBlackSegment(ei, rootKey, segOffset, segKey, segOffset,
		       keyBits_IsType(&segRoot, KKT_Wrapper) ? true : false);

  if (wasSeg)
    keyBits_SetType(&newSegRoot, KKT_Segment);

  if ( keyBits_IsType(&segRoot, KKT_Wrapper) )
    return segRoot;

  return newSegRoot;
}

bool
ei_DoGetPageInSegment(ErosImage *ei, KeyBits segRoot,
		      uint64_t segOffset,
		      KeyBits *pageKey /* OUT */)
{
  /* pageSegLSS holds the LSS of the smallest segment that could
   * conceivably contain pageAddr.
   */
  uint32_t segOffsetBLSS = lss_BiasedLSS(segOffset);
  uint32_t rootBLSS = ei_GetAnyBlss(ei, segRoot);
  
  if (segOffsetBLSS > rootBLSS)
    return false;

  if (segOffsetBLSS < rootBLSS) {
    KeyBits subSeg = ei_GetNodeSlot(ei, segRoot, 0);
    return ei_DoGetPageInSegment(ei, subSeg, segOffset, pageKey);
  }

  /* Handle single page or empty segments: */
  if (segOffsetBLSS == rootBLSS && segOffsetBLSS == EROS_PAGE_BLSS) {
    *pageKey = segRoot;
    return (keyBits_IsType(&segRoot, KKT_Page)) ? true : false;
  }

  {
    /* segOffsetBlss == rootBLSS, not a page:
     * page key goes somewhere beneath current tree:
     */
    uint32_t slot = lss_SlotNdx(segOffset, segOffsetBLSS);
    uint64_t subSegOffset = segOffset & lss_Mask(segOffsetBLSS -1);
    KeyBits subSeg;

    subSeg = ei_GetNodeSlot(ei, segRoot, slot);
    return ei_DoGetPageInSegment(ei,subSeg, subSegOffset, pageKey);
  }
}

bool
ei_GetPageInSegment(ErosImage *ei, KeyBits segRoot,
		    uint64_t segOffset,
		    KeyBits *pageKey)
{
  ValidateSegKey(ei, segRoot);

  return ei_DoGetPageInSegment(ei, segRoot, segOffset, pageKey);
}

void
ei_SetPageWord(ErosImage *ei, KeyBits *pageKey, uint32_t offset,
	       uint32_t value)
{
  if (keyBits_IsType(pageKey, KKT_Page) == false)
    diag_fatal(5, "ei_SetPageWord requires page key!\n");
  
  if (offset % 4)
    diag_fatal(5, "ei_SetPageWord offset must be word address!\n");
    
  /* Okay, here's the tricky part.  It's quite possible that the page
   * key we were handed was a zero page key.  If so, we fabricate a
   * new (nonzero) page to replace it and change all of the existing
   * keys to this page to point to the new page:
   */
  if (keyBits_IsPrepared(pageKey)) {
    uint8_t buf[EROS_PAGE_SIZE];
    KeyBits newPage;
    uint32_t nodeNdx;
    uint32_t dirNdx;

    memset(buf, 0, EROS_PAGE_SIZE);
    
    newPage = ei_AddDataPage(ei, buf, false);

    /* Relocate all of the keys in the image to reflect the removal of
     * this zero page.
     */
    for (nodeNdx = 0; nodeNdx < ei->hdr.nNodes; nodeNdx++) {
      uint32_t keyNdx;

      for (keyNdx = 0; keyNdx < EROS_NODE_SIZE; keyNdx++) {
	KeyBits *key = &ei->nodeImages[nodeNdx].slot[keyNdx];

	if (! keyBits_IsType(key, KKT_Page) || ! keyBits_IsPrepared(key))
	  continue;
	
	/* It's a zero page key.  May need relocation:  */

	if (key->u.unprep.oid == pageKey->u.unprep.oid) {
	  /* This is a key to the old (zero) page.  Do an in-place
	   * swap for the new key, leaving all old attributes untouched.
	   */
	  key->u.unprep.oid = newPage.u.unprep.oid;
	}

	if (key->u.unprep.oid > pageKey->u.unprep.oid) {
	  /* This is a key to a zero page that is AFTER the one we are
	   * removing. Decrement it's oidLo by 1:
	   */
	  key->u.unprep.oid = key->u.unprep.oid-1;
	}
      }
    }

    for (dirNdx = 0; dirNdx < ei->hdr.nDirEnt; dirNdx++) {
	KeyBits *key = &ei->dir[dirNdx].key;

	if (! keyBits_IsType(key, KKT_Page) || 
	    ! keyBits_IsPrepared(key) )
	  continue;
	
	/* It's a zero page key.  May need relocation:  */

	if (key->u.unprep.oid == pageKey->u.unprep.oid) {
	  /* This is a key to the old (zero) page.  Do an in-place
	   * swap for the new key, leaving all old attributes untouched.
	   */
	  key->u.unprep.oid = newPage.u.unprep.oid;
	}

	if (key->u.unprep.oid > pageKey->u.unprep.oid) {
	  /* This is a key to a zero page that is AFTER the one we are
	   * removing. Decrement it's oidLo by 1:
	   */
	  key->u.unprep.oid = key->u.unprep.oid-1;
	}
    }
    
    /* We have replaced a zero page with a nonZero page, and removed
     * all references to the old zero page, relocating accordingly.
     * We now have one less zero page:
     */
    ei->hdr.nZeroPages--;

    /* Finally, we need to relocate the page key we were handed too! */
    pageKey->u.unprep.oid = newPage.u.unprep.oid;
  }
  
  {
    uint32_t pageNdx = pageKey->u.unprep.oid;
    uint8_t *pageContent = &ei->pageImages[pageNdx*EROS_PAGE_SIZE];

    *((uint32_t *) &pageContent[offset]) = value;
  }
}

void
ei_PrintNode(const ErosImage *ei, KeyBits nodeKey)
{
  unsigned i;

  for (i = 0; i < EROS_NODE_SIZE; i++) {
    KeyBits key = ei_GetNodeSlot(ei, nodeKey, i);

    if (keyBits_IsVoidKey(&key))
      continue;
    
    diag_printf("  [%2d] ", i);
    PrintDiskKey(key);
    diag_printf("\n");
  }
}

void
ei_PrintPage(const ErosImage *ei, KeyBits pageKey)
{
  if (keyBits_IsPrepared(&pageKey)) {
    diag_printf("Page OID=");
    diag_printOid(pageKey.u.unprep.oid);
    diag_printf(" flags=Z (zero page)\n");
  }
  else {
    uint8_t *bufp = 
      &ei->pageImages[((uint32_t)pageKey.u.unprep.oid) * EROS_PAGE_SIZE];
    int i;

    diag_printf("Page OID=");
    diag_printOid(pageKey.u.unprep.oid);
    diag_printf("\n");

    for (i = 0; i < 8; i++) {
      int j;

      diag_printf("    ");
      for (j = 0; j < 16; j++) {
	diag_printf("%02x", *bufp);
	bufp++;
      }
      diag_printf("\n");
    }
    diag_printf("...\n");
  }
  diag_printf("\n");
}

/* Given a node key, pretend it's a domain and print it out: */
void
ei_PrintDomain(const ErosImage *ei, KeyBits domRoot)
{
  KeyBits genKeys;
  unsigned i;

  if (keyBits_IsNodeKeyType(&domRoot) == false)
    diag_fatal(4, "Non-node key passed to PrintDomain\n");

  genKeys = ei_GetNodeSlot(ei, domRoot, ProcGenKeys);

  diag_printf("Domain root:\n");
  for (i = 0; i < EROS_NODE_SIZE; i++) {
    KeyBits key = ei_GetNodeSlot(ei, domRoot, i);

    diag_printf("  [%2d] ", i);
    PrintDiskKey(key);

    /* keeper key must be start key: */
    if (i == ProcKeeper &&
	keyBits_IsType(&key, KKT_Start) == false &&
	! keyBits_IsVoidKey(&key) )
      diag_printf(" (malformed)");

    /* address space key must be segmode key: */
    if (i == ProcAddrSpace &&
	keyBits_IsType(&key, KKT_Segment) == false &&
	keyBits_IsType(&key, KKT_Node) == false &&
	keyBits_IsType(&key, KKT_Page) == false)
      diag_printf(" (malformed)");
      
    if (i == ProcGenKeys && !keyBits_IsNodeKeyType(&key))
      diag_printf(" (malformed)");
    
    diag_printf("\n");
  }

  if (keyBits_IsNodeKeyType(&genKeys)) {
    diag_printf("General Keys:\n");

    for (i = 0; i < EROS_NODE_SIZE; i++) {
      KeyBits key = ei_GetNodeSlot(ei, genKeys, i);

      diag_printf("  [%2d] ", i);
    
      PrintDiskKey(key);
      diag_printf("\n");
    }
  }
}

void
ei_DoPrintSegment(const ErosImage *ei, uint32_t slot, KeyBits segKey,
		  uint32_t indent, const char *annotation,
		  bool startKeyOK)
{
  uint32_t i;

  /* Only print top-level void keys: */
  if (indent && keyBits_IsVoidKey(&segKey))
    return;

  for (i = 0; i < indent; i++) 
    diag_printf("  ");

  switch(keyBits_GetType(&segKey)) {
  case KKT_Node:
  case KKT_Segment:
    diag_printf("[%2d] :", slot);
    PrintDiskKey(segKey);
    if (annotation)
      diag_printf(" (%s)", annotation);
    diag_printf("\n");

    if ( keyBits_IsType(&segKey, KKT_Wrapper) ) {
      uint32_t i;
      KeyBits fmtKey = ei_GetNodeSlot(ei, segKey, WrapperFormat);

      if (keyBits_IsType(&fmtKey, KKT_Number) == false) {
	int i;

	for (i = 0; i < WrapperFormat; i++) {
	  KeyBits key = ei_GetNodeSlot(ei, segKey, i);
	  ei_DoPrintSegment(ei, i, key, indent+1, 0, false);
	}
	ei_DoPrintSegment(ei, WrapperFormat, fmtKey, indent+1, "format, malformed", 0);

	return;
      }

      /* Have valid format key and valid kpr/bg slots: */
      for (i = 0; i < EROS_NODE_SIZE; i++) {
	char *annotation = 0;
	bool startKeyOK = false;
	KeyBits key = ei_GetNodeSlot(ei, segKey, i);

	if (i == WrapperFormat) {
	  annotation = "format";
	}
	else if (i == WrapperBackground &&
		 (fmtKey.u.nk.value[0] & WRAPPER_BACKGROUND)) {
	  annotation = "background";
	}
	else if (i == WrapperKeeper &&
		 (fmtKey.u.nk.value[0] & WRAPPER_KEEPER)) {
	  annotation = "keeper";
	  startKeyOK = true;
	}
	
	ei_DoPrintSegment(ei, i, key, indent+1, annotation, startKeyOK);
      }
    }
    else {
      unsigned i;

      for (i = 0; i < EROS_NODE_SIZE; i++) {
	KeyBits key = ei_GetNodeSlot(ei, segKey, i);
	ei_DoPrintSegment(ei, i, key, indent+1, 0, false);
      }
    }
    break;
  case KKT_Page:
  case KKT_Number:
    diag_printf("[%2d] ", slot);
    PrintDiskKey(segKey);
    if ((segKey.u.nk.value[0] & 3) == 3)
      diag_printf(" (background window)");
    if (annotation)
      diag_printf(" (%s)", annotation);
    diag_printf("\n");
    break;
  case KKT_Start:
  case KKT_Resume:
    {
      if (startKeyOK) {
	diag_printf("[%2d] ", slot);
	PrintDiskKey(segKey);
	if (annotation)
	  diag_printf(" (%s)", annotation);
	diag_printf("\n");
	break;
      }
      /* fall through */
    }
  default:
    diag_printf("[%2d] ", slot);
    PrintDiskKey(segKey);
    if (annotation)
      diag_printf(" (%s, malformed)\n", annotation);
    else
      diag_printf("(malformed)\n");
  }
}

void
ei_PrintSegment(const ErosImage *ei, KeyBits segKey)
{
  ei_DoPrintSegment(ei, 0, segKey, 0, 0, false);
}

void
ei_SetProcessState(ErosImage *ei, KeyBits procRoot, uint8_t state)
{
  KeyBits key = ei_GetNodeSlot(ei, procRoot, ProcTrapCode);
  key.u.nk.value[2] &= key.u.nk.value[2] & 0xffffff00;
  key.u.nk.value[2] |= state;
  ei_SetNodeSlot(ei, procRoot, ProcTrapCode, key);
}
	  
uint8_t
ei_GetProcessState(ErosImage *ei, KeyBits procRoot)
{
  KeyBits key = ei_GetNodeSlot(ei, procRoot, ProcTrapCode);
  uint32_t state = key.u.nk.value[2] & 0xffu;
  return state;
}
	  
KeyBits
ei_AddProcess(ErosImage *ei)
{
  KeyBits rootKey = ei_AddNode(ei, false);
  KeyBits keyregsKey = ei_AddNode(ei, false);

  ei_SetNodeSlot(ei, rootKey, ProcGenKeys, keyregsKey);

  /* Domain is initially available: */
  ei_SetProcessState(ei, rootKey, RS_Available);

  return rootKey;
}

