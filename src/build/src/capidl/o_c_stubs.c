/*
 * Copyright (C) 2002, The EROS Group, LLC.
 * Copyright (C) 2008, 2009, Strawberry Development Group.
 *
 * This file is part of the CapROS Operating System runtime library,
 * and is derived from the EROS Operating System runtime library.
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
/* This material is based upon work supported by the US Defense Advanced
Research Projects Agency under Contract No. W31P4Q-07-C-0070.
Approved for public release, distribution unlimited. */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

/* GNU multiple precision library: */
#include <gmp.h>

#include <applib/xmalloc.h>
#include <applib/Intern.h>
#include <applib/Diag.h>
#include <applib/PtrVec.h>
#include <applib/path.h>
#include <applib/buffer.h>
#include "SymTab.h"
#include "util.h"
#include "backend.h"

/* Size of native register */
#define REGISTER_BITS      32

/* Size of largest integral type that we will attempt to registerize: */
#define MAX_REGISTERIZABLE 64

#define FIRST_REG 1		/* reserve 1 for opcode/result code */
#define MAX_REGS  4

static Buffer *preamble;

extern bool c_byreftype(Symbol *s);
extern const char* c_typename(Symbol *s);
extern void output_c_type(Symbol *s, FILE *out, int indent);
extern void output_c_type_trailer(Symbol *s, FILE *out, int indent);
extern MP_INT compute_value(Symbol *s);

/* can_registerize(): given a symbol /s/ denoting an argument, and a
   number /nReg/ indicating how many register slots are taken, return:

     0   if the symbol /s/ cannot be passed in registers, or
     N>0 the number of registers that the symbol /s/ will occupy.

   note that a 64 bit integer quantity will be registerized, but only
   if it can be squeezed into the number of registers that remain
   available.
*/
unsigned 
can_registerize(Symbol *s, unsigned nReg)
{
  s = symbol_ResolveType(s);
  
  assert(symbol_IsBasicType(s));

  if (nReg == MAX_REGS)
    return 0;

  while (s->cls == sc_symRef)
    s = s->value;

  if (s->cls == sc_typedef)
    return can_registerize(s->type, nReg);

  if (s->cls == sc_enum)
    return 1;

  switch(s->cls) {
  case sc_primtype:
    {
      switch(s->v.lty) {
      case lt_integer:
      case lt_unsigned:
      case lt_char:
      case lt_bool:
	{
	  /* Integral types are aligned to their size. Floating types
	     are aligned to their size, not to exceed 64 bits. If we
	     ever support larger fixed integers, we'll need to break
	     these cases up. */

	  unsigned bits = mpz_get_ui(s->v.i);
      
	  if (bits == 0)
	    return 0;

	  if (nReg >= MAX_REGS)
	    return 0;

	  if (bits <= REGISTER_BITS)
	    return 1;

	  if (bits <= MAX_REGISTERIZABLE) {
	    unsigned needRegs = bits / REGISTER_BITS;
	    if (nReg + needRegs <= MAX_REGS)
	      return needRegs;
	  }

	  return 0;
	}
      default:
	return 0;
      }
    }
  case sc_enum:
    {
      /* Note assumption that enumeral type fits in a register */
      if (nReg >= MAX_REGS)
	return 0;

      return 1;
    }

  default:
    return 0;
 }
}

void
emit_registerize(FILE *out, Symbol *child, int indent, unsigned regCount)
{
  unsigned bitsOutput = 0;
  unsigned bits = mpz_get_ui(child->type->v.i);

  {
    do_indent(out, indent);
    fprintf(out, "msg.snd_w%d = %s;\n", regCount, child->name);

    bitsOutput += REGISTER_BITS;
    regCount++;
  }

  /* If the quantity is larger than a single register, we need to
     output the rest of it: */

  while (bitsOutput < bits) {
    do_indent(out, indent);
    fprintf(out, "msg.snd_w%d = (%s >> %d);\n", 
	    regCount, child->name, bitsOutput);

    bitsOutput += REGISTER_BITS;
    regCount++;
  }
}

void
emit_deregisterize(FILE *out, Symbol *child, int indent, unsigned regCount)
{
  unsigned bitsInput = 0;
  unsigned bits = mpz_get_ui(child->type->v.i);

  {
    do_indent(out, indent);
    fprintf(out, "if (%s) *%s = msg.rcv_w%d;\n", 
	    child->name, child->name, regCount);

    bitsInput += REGISTER_BITS;
    regCount++;
  }

  /* If the quantity is larger than a single register, we need to
     decode the rest of it: */

  while (bitsInput < bits) {
    do_indent(out, indent);
    fprintf(out, "if (%s) *%s |= ( ((",
	    child->name, child->name);

    output_c_type(child->type, out, 0);

    fprintf(out, ")msg.rcv_w%d) << %d );\n", 
	    regCount, bitsInput);

    bitsInput += REGISTER_BITS;
    regCount++;
  }
}

PtrVec *
extract_registerizable_arguments(Symbol *s, SymClass sc)
{
  unsigned i;
  unsigned nReg = FIRST_REG;
  unsigned needRegs;
  PtrVec *regArgs = ptrvec_create();

  for(i = 0; i < vec_len(s->children); i++) {
    Symbol *child = symvec_fetch(s->children,i);

    if (child->cls != sc)
      continue;

    if (symbol_IsInterface(child->type)) {
      ptrvec_append(regArgs, child);
      continue;
    }

    if ((needRegs = can_registerize(child->type, nReg))) {
      ptrvec_append(regArgs, child);
      nReg += needRegs;
      continue;
    }
  }

  return regArgs;
}

PtrVec *
extract_string_arguments(Symbol *s, SymClass sc)
{
  unsigned i;
  unsigned nReg = FIRST_REG;
  unsigned needRegs;
  PtrVec *stringArgs = ptrvec_create();

  for(i = 0; i < vec_len(s->children); i++) {
    Symbol *child = symvec_fetch(s->children,i);

    if (child->cls != sc)
      continue;

    if (symbol_IsInterface(child->type))
      continue;

    if ((needRegs = can_registerize(child->type, nReg))) {
      nReg += needRegs;
      continue;
    }

    ptrvec_append(stringArgs, child);
  }

  return stringArgs;
}

unsigned
compute_indirect_bytes(PtrVec *symVec)
{
  unsigned i;
  unsigned sz = 0;

  for(i = 0; i < vec_len(symVec); i++) {
    Symbol *sym = vec_fetch(symVec, i);
    sz = round_up(sz, symbol_alignof(sym));
    sz += symbol_indirectSize(sym);
  }

  return sz;
}

unsigned
compute_direct_bytes(PtrVec *symVec)
{
  unsigned i;
  unsigned sz = 0;

  for(i = 0; i < vec_len(symVec); i++) {
    Symbol *sym = vec_fetch(symVec, i);
    sz = round_up(sz, symbol_alignof(sym));
    sz += symbol_directSize(sym);
  }

  return sz;
}

unsigned
emit_symbol_align(const char *lenVar, FILE *out, int indent,
		  unsigned elemAlign, unsigned align)
{
  if ((elemAlign & align) == 0) {
    do_indent(out, indent);
    fprintf(out, "%s += (%d-1); %s -= (%s %% %d);\t/* align to %d */\n",
	    lenVar, elemAlign,
	    lenVar, lenVar, elemAlign, elemAlign);
  }

  return (elemAlign * 2) - 1;
}
     
/* The /align/ field is a bitmap representing which power of two
   alignments are known to be valid. */
unsigned
emit_direct_symbol_size(const char *fullName, Symbol *s, 
			FILE *out, int indent, SymClass sc, unsigned align)
{
  const char *lenVar = (sc == sc_formal) ? "sndLen" : "rcvLen";
      
  switch(s->cls) {
  case sc_formal:
  case sc_outformal:
  case sc_typedef:
    {
      return emit_direct_symbol_size(fullName, s->type, out,
				     indent, sc, align);
    }
  case sc_member:
    {
      return emit_direct_symbol_size(symname_join(fullName, s->name, '.'),
				     s->type, out, indent, sc, align);
    }
  case sc_symRef:
    {
      return emit_direct_symbol_size(fullName, s->value, out, indent, sc,
				     align);
    }
    
  case sc_primtype:
  case sc_enum:
  case sc_struct:
  case sc_seqType:
  case sc_bufType:
  case sc_arrayType:
    {
      /* No indirect size */
      unsigned elemAlign = symbol_alignof(s);
      unsigned elemSize = symbol_directSize(s);
      elemSize = round_up(elemSize, elemAlign);

      align = emit_symbol_align(lenVar, out, indent, elemAlign, align);

      do_indent(out, indent);
      fprintf(out, "%s += %d;\t/* sizeof(%s), align=%d */\n",
	      lenVar, elemSize, fullName, elemAlign);

      return align;
    }

  case sc_union:
    {
      assert(false);
      return align;
    }
  case sc_interface:
  case sc_absinterface:
    {
      assert(false);
      return align;
    }
  default:
    {
      diag_fatal(1, "Size computation of unknown type for symbol \"%s\"\n", 
		 symbol_QualifiedName(s, '.'));
      return align;
    }
  }

  return align;
}

/* The /align/ field is a bitmap representing which power of two
   alignments are known to be valid. */
unsigned
emit_indirect_symbol_size(const char *fullName, Symbol *s, 
			  FILE *out, int indent, SymClass sc, unsigned align)
{
  const char *lenVar = (sc == sc_formal) ? "sndLen" : "rcvLen";
      
  switch(s->cls) {
  case sc_formal:
  case sc_outformal:
  case sc_typedef:
    {
      return emit_indirect_symbol_size(fullName, s->type, out,
				       indent, sc, align);
    }
  case sc_member:
    {
      return emit_indirect_symbol_size(symname_join(fullName, s->name, '.'),
				       s->type, out, indent, sc, align);
    }
  case sc_symRef:
    {
      return emit_indirect_symbol_size(fullName, s->value, out, indent, sc,
				       align);
    }
    
  case sc_primtype:
  case sc_enum:
    /* No indirect size */
    return align;

  case sc_struct:
    {
      unsigned i;

      /* Compute indirect size on each member in turn */
      for(i = 0; i < vec_len(s->children); i++) {
	Symbol *child = vec_fetch(s->children, i);
	align = emit_indirect_symbol_size(fullName, child, out, indent,
					  sc, align);
      }

      return align;
    }
  case sc_seqType:
  case sc_bufType:
    {
      unsigned elemAlign = symbol_alignof(s->type);
      unsigned elemSize = symbol_directSize(s->type);
      elemSize = round_up(elemSize, elemAlign);

      /* See if an alignment adjustment is needed: */
      align = emit_symbol_align(lenVar, out, indent, elemAlign, align);
      
      do_indent(out, indent);
      if (sc == sc_formal) {
	fprintf(out, "%s += %d * %s.len; /* %s vector content, align=%d */\n",
		lenVar, elemSize, fullName, fullName, elemAlign);
      }
      else {
	Symbol *theSeqType = symbol_ResolveType(s);
	MP_INT bound = compute_value(theSeqType->value);
	fprintf(out, "%s += %d * %d; /* %s vector MAX content, align=%d */\n",
		lenVar, elemSize, mpz_get_ui(&bound), fullName, elemAlign);
      }
      
      return (2 * elemAlign) - 1;
    }
  case sc_arrayType:
    {
      /* Array type is always inline, never indirect. */
      return align;
    }
  case sc_union:
    {
      assert(false);
      return align;
    }
  case sc_interface:
  case sc_absinterface:
    {
      assert(false);
      return align;
    }
  default:
    {
      diag_fatal(1, "Size computation of unknown type for symbol \"%s\"\n", 
		 symbol_QualifiedName(s, '.'));
      return align;
    }
  }

  return align;
}

unsigned
emit_direct_byte_computation(PtrVec *symVec, FILE *out, int indent,
			     SymClass sc, unsigned align)
{
  unsigned i;

  for(i = 0; i < vec_len(symVec); i++) {
    Symbol *sym = vec_fetch(symVec, i);
    align = emit_direct_symbol_size(sym->name, sym, out, indent, sc, align);
  }

  return align;
}

unsigned
emit_indirect_byte_computation(PtrVec *symVec, FILE *out, int indent,
			       SymClass sc, unsigned align)
{
  unsigned i;

  for(i = 0; i < vec_len(symVec); i++) {
    Symbol *sym = vec_fetch(symVec, i);
    align = emit_indirect_symbol_size(sym->name, sym, out, indent, sc, align);
  }

  return align;
}

extern const char* c_serializer(Symbol *s);

void
emit_send_string(PtrVec *symVec, FILE *out, int indent)
{
  unsigned i;

  if (vec_len(symVec) == 0)
    return;

  /* Choose strategy: */
  if (vec_len(symVec) == 1 &&
      symbol_IsDirectSerializable(symvec_fetch(symVec,0)->type)) {
    Symbol *s0 = symvec_fetch(symVec, 0);
    Symbol * s0BaseType = symbol_ResolveType(s0->type);

    do_indent(out, indent);
    fprintf(out, "/* Using direct method */\n");

    if (symbol_IsVarSequenceType(s0BaseType)) {
      do_indent(out, indent);
      fprintf(out, "msg.snd_len = %s_len * sizeof(",
	      symvec_fetch(symVec,0)->name);
      output_c_type(s0->type, out, 0);
      fprintf(out, ");\n");
    }
    else if (symbol_IsFixSequenceType(s0BaseType)) {
      do_indent(out, indent);
      fprintf(out, "msg.snd_len = sizeof(%s);\n", s0->name);
    }
    else {
      do_indent(out, indent);
      fprintf(out, "msg.snd_len = sizeof(%s%s);\n",
	      (c_byreftype(s0->type) ? "*" : ""),
	      s0->name);
    }
    do_indent(out, indent);
    fprintf(out, "msg.snd_data = %s%s;\n",
	    (c_byreftype(s0->type) ? "" : "&"),
	    s0->name);
    fputc('\n', out);
  }
  else {
    unsigned align = 0xfu;
    unsigned indirAlign = 0xfu;

    do_indent(out, indent);
    fprintf(out, "msg.snd_data = sndData;\n");
    do_indent(out, indent);
    fprintf(out, "msg.snd_len = sndLen;\n");
    fprintf(out, "\n");
    do_indent(out, indent);
    fprintf(out, "sndLen = 0;\n");

    for(i = 0; i < vec_len(symVec); i++) {
      Symbol *arg = vec_fetch(symVec, i);
      Symbol *argType = symbol_ResolveRef(arg->type);
      Symbol *argBaseType = symbol_ResolveType(arg->type);
      
      fprintf(out, "\n");
      do_indent(out, indent);
      fprintf(out, "/* Serialize %s */\n", arg->name);
      do_indent(out, indent);
      fprintf(out, "{\n");
      do_indent(out, indent+2);

      output_c_type(argType, out, 0);
      fprintf(out, " *_CAPIDL_arg;\n\n");

      align = emit_symbol_align("sndLen", out, indent+2,
				symbol_alignof(argBaseType), align);

      do_indent(out, indent+2);
      fprintf(out, "_CAPIDL_arg = (");
      output_c_type(argType, out, 0);
      fprintf(out, " *) (sndData + sndLen);\n");

      if (symbol_IsFixSequenceType(argBaseType)) {
	MP_INT bound = compute_value(argBaseType->value);

	do_indent(out, indent+2);
	fprintf(out, "__builtin_memcpy(_CAPIDL_arg, %s, %d * sizeof(*%s));\n",
		arg->name, mpz_get_ui(&bound), arg->name);
      }
      else {
	do_indent(out, indent+2);
	fprintf(out, "*_CAPIDL_arg = %s;\n", arg->name);
	do_indent(out, indent+2);
	fprintf(out, "sndLen += sizeof(%s);\n", arg->name);

	if (symbol_IsVarSequenceType(argBaseType)) {
	  do_indent(out, indent+2);

	  indirAlign = emit_symbol_align("sndIndir", out, indent+2,
				    symbol_alignof(argBaseType), indirAlign);

	  fprintf(out, "_CAPIDL_arg->data = (");
	  output_c_type(argBaseType->type, out, 0);
	  fprintf(out, " *) (sndData + sndIndir);\n", arg->name);

	  do_indent(out, indent+2);
	  fprintf(out, "__builtin_memcpy(_CAPIDL_arg->data, %s.data, "
		  "sizeof(*%s.data) * %s.len);\n",
		  arg->name, arg->name, arg->name);

	  fprintf(out, "\n");
	  do_indent(out, indent+2);
#if 0
	  fprintf(out, "/* Encode the vector offset for transmission */\n");
	  do_indent(out, indent+2);
	  fprintf(out, "((unsigned long) _CAPIDL_arg->data) = sndIndir;\n\n");

	  do_indent(out, indent+2);
#endif
	  fprintf(out, "sndIndir += sizeof(%s.data) * %s.len;\n",
		  arg->name, arg->name);
	}
	  
      }
      do_indent(out, indent);
      fprintf(out, "}\n");
      
    }
  }
}

static void
emit_receive_string(PtrVec *symVec, FILE *out, int indent)
{
  if (vec_len(symVec) == 0)
    return;
  
  /* Choose strategy: */
  if (vec_len(symVec) == 1 &&
      symbol_IsDirectSerializable(symvec_fetch(symVec,0)->type)) {
    Symbol *s0 = symvec_fetch(symVec, 0);

    do_indent(out, indent);
    fprintf(out, "/* Using direct method */\n");

    do_indent(out, indent);
    fprintf(out, "msg.rcv_limit = sizeof(*%s);\n",
	    s0->name);
    do_indent(out, indent);
    fprintf(out, "msg.rcv_data = %s;\n", /* already set up as a pointer */
	    s0->name);
    fputc('\n', out);
  }
  else {
    do_indent(out, indent);
    fprintf(out, "msg.rcv_data = rcvData;\n");
    do_indent(out, indent);
    fprintf(out, "msg.rcv_limit = rcvLen;\n");
  }
}

static void
emit_unpack_return_registers(PtrVec *symVec, FILE *out, int indent)
{
  unsigned i;
  unsigned nReg = FIRST_REG;
  unsigned needRegs;

  for(i = 0; i < vec_len(symVec); i++) {
    Symbol *child = symvec_fetch(symVec,i);

    if (child->cls != sc_outformal)
      continue;

    if (symbol_IsInterface(child->type))
      continue;

    if ((needRegs = can_registerize(child->type, nReg))) {
      emit_deregisterize(out, child, indent+2, nReg);
      nReg += needRegs;
      continue;
    }
  }
}

static void
emit_unpack_return_string(PtrVec *symVec, FILE *out, int indent)
{
  bool isDirect;
  unsigned i;
  unsigned align = 0xfu;
  unsigned indirAlign = 0xfu;

  /* Choose strategy: */
  isDirect = (vec_len(symVec) == 1 &&
	      symbol_IsDirectSerializable(symvec_fetch(symVec,0)->type));

  if (isDirect)
    return;

  do_indent(out, indent);
  fprintf(out, "rcvLen = 0;\n");

  for(i = 0; i < vec_len(symVec); i++) {
    Symbol *arg = vec_fetch(symVec, i);
    Symbol *argType = symbol_ResolveRef(arg->type);
    Symbol *argBaseType = symbol_ResolveType(arg->type);
      
    fprintf(out, "\n");
    do_indent(out, indent);
    fprintf(out, "/* Deserialize %s */\n", arg->name);
    do_indent(out, indent);
    fprintf(out, "{\n");
    do_indent(out, indent+2);

    output_c_type(argType, out, 0);
    fprintf(out, " *_CAPIDL_arg;\n\n");

    align = emit_symbol_align("rcvLen", out, indent+2,
			      symbol_alignof(argBaseType), align);

    do_indent(out, indent+2);
    fprintf(out, "_CAPIDL_arg = (");
    output_c_type(argType, out, 0);
    fprintf(out, " *) (rcvData + rcvLen);\n");

    if (symbol_IsFixSequenceType(argBaseType)) {
      MP_INT bound = compute_value(argBaseType->value);

      do_indent(out, indent+2);
      fprintf(out, "__builtin_memcpy(%s, rcvData + rcvLen, %d * sizeof(*%s));\n",
	      arg->name, mpz_get_ui(&bound), arg->name);

      do_indent(out, indent+2);
      fprintf(out, "rcvLen += (%d * sizeof(*%s));\n",
	      mpz_get_ui(&bound), arg->name);
    }
    else {
      do_indent(out, indent+2);
      fprintf(out, "*%s = *_CAPIDL_arg;\n", arg->name);
      do_indent(out, indent+2);
      fprintf(out, "rcvLen += sizeof(*%s);\n", arg->name);

      if (symbol_IsVarSequenceType(argBaseType)) {
	MP_INT bound = compute_value(argBaseType->value);
	unsigned int ubound = mpz_get_ui(&bound);
	
	do_indent(out, indent+2);

	indirAlign = emit_symbol_align("rcvIndir", out, indent+2,
				       symbol_alignof(argBaseType), indirAlign);

	fprintf(out, "_CAPIDL_arg->data = (");
	output_c_type(argBaseType->type, out, 0);
	fprintf(out, " *) (rcvData + rcvIndir);\n", arg->name);

	do_indent(out, indent+2);
	fprintf(out, "__builtin_memcpy(%s->data, _CAPIDL_arg->data, "
		"sizeof(%s->data) * "
		"(_CAPIDL_arg->len < %d ? _CAPIDL_arg->len : %d));\n",
		arg->name, arg->name, ubound, ubound);

	fprintf(out, "\n");

	do_indent(out, indent+2);
	fprintf(out, "rcvIndir += sizeof(%s->data) * "
		"(_CAPIDL_arg->len < %d ? _CAPIDL_arg->len : %d);\n",
		arg->name, ubound, ubound);
      }
	  
    }
    do_indent(out, indent);
    fprintf(out, "}\n");
      
  }
}

#if 0
bool
c_excpt_needs_message_string(Symbol *ex)
{
  assert(ex->cls == sc_exception);

  /* Exception structures are ALWAYS passed as stringified responses. */
  return vec_len(ex->children) != 0;
}
#endif

#if 0
bool
c_if_needs_exception_string(Symbol *s)
{
  unsigned i;

  if (s->baseType) {
    Symbol *base = s->baseType;
    while (base->cls == sc_symRef)
      base = base->value;

    if (c_if_needs_exception_string(base))
      return true;
  }

  for(i = 0; i < vec_len(s->raises); i++) {
    Symbol *ex = symvec_fetch(s->raises,i);
    while(ex->cls == sc_symRef)
      ex = ex->value;

    if (c_excpt_needs_message_string(ex))
      return true;
  }

  return false;
}
#endif

#if 0
bool
c_op_needs_exception_string(Symbol *op)
{
  unsigned a;
  for (a = 0; a < vec_len(op->raises); a++) {
    Symbol *ex = symvec_fetch(op->raises,a);
    while(ex->cls == sc_symRef)
      ex = ex->value;

    if (c_excpt_needs_message_string(ex))
      return true;
  }

  if (c_if_needs_exception_string(op->nameSpace))
    return true;

  return false;
}
#endif

/* c_op_needs_message_string(): returns
 *
 *     0 if no message string is required
 *     1 if a message string is needed and a struct must be built
 *     2 if a message string is needed but can be serialized directly.
 */
static unsigned
c_op_needs_message_string(Symbol *op, SymClass sc)
{
  unsigned a;
  unsigned nReg = FIRST_REG;
  unsigned needRegs;

  unsigned nStringElem = 0;
  unsigned nDirectElem = 0;

  for (a = 0; a < vec_len(op->children); a++) {
    Symbol *arg = symvec_fetch(op->children,a);

    if (arg->cls != sc)
      continue;

    if (symbol_IsInterface(arg->type)) {
    }
    else if ((needRegs = can_registerize(arg->type, nReg))) {
      nReg += needRegs;
    }
    else {
      nStringElem ++;
      if (symbol_IsDirectSerializable(arg->type))
	nDirectElem ++;
    }
  }

  if (nStringElem == 1 && nDirectElem == 1)
    return 2;
  else 
    return (nStringElem ? 1 : 0);
}

#if 0
bool
c_if_needs_message_string(Symbol *s, SymClass sc)
{
  unsigned i;

  if (s->baseType) {
    Symbol *base = s->baseType;
    while (base->cls == sc_symRef)
      base = base->value;

    if (c_if_needs_message_string(base, sc))
      return true;
  }

  for(i = 0; i < vec_len(s->children); i++) {
    Symbol *op = symvec_fetch(s->children,i);
    if (op->cls != sc_operation)
      continue;

    if (c_op_needs_message_string(op, sc))
      return true;
  }

  return false;
}
#endif

static unsigned rcv_capcount;
static void
loadRcvCap(FILE * out, int indent, const char * name)
{
  if (rcv_capcount >= 4)
    diag_fatal(1, "Too many capabilities received\n");

  do_indent(out, indent + 2);
  if (rcv_capcount == 3)	// RESUME_SLOT
    fprintf(out, "msg.rcv_rsmkey = %s;\n", 
	    name);
  else
    fprintf(out, "msg.rcv_key%d = %s;\n", 
	    rcv_capcount, name);
  rcv_capcount++;
}

static void
output_client_stub(FILE *out, Symbol *s, int indent)
{
  unsigned i;
  PtrVec *sndRegs = extract_registerizable_arguments(s, sc_formal);
  PtrVec *rcvRegs = extract_registerizable_arguments(s, sc_outformal);
  PtrVec *sndString = extract_string_arguments(s, sc_formal);
  PtrVec *rcvString = extract_string_arguments(s, sc_outformal);

  unsigned snd_capcount = 0;
  rcv_capcount = 0;
  unsigned snd_regcount = FIRST_REG;
  unsigned rcv_regcount = FIRST_REG;

  unsigned needSendString = c_op_needs_message_string(s, sc_formal);
  unsigned needRcvString = c_op_needs_message_string(s, sc_outformal);

  unsigned needRegs;

  {
    BufferChunk bc;
    off_t pos = 0;
    off_t end = buffer_length(preamble);

    while (pos < end) {
      bc = buffer_getChunk(preamble, pos, end - pos);
      fwrite(bc.ptr, 1, bc.len, out);
      pos += bc.len;
    }
  }

  /***************************************************************
   * Function return value, name, and parameters
   ***************************************************************/
  assert(symbol_IsVoidType(s->type));
  fprintf(out,"result_t");

  /* Function prefix and arguments */
  fprintf(out, "\n%s(cap_t _self", symbol_QualifiedName(s,'_'));

  for(i = 0; i < vec_len(s->children); i++) {
    Symbol *child = symvec_fetch(s->children,i);

    bool wantPtr = (child->cls == sc_outformal) || c_byreftype(child->type);
    wantPtr = wantPtr && !symbol_IsInterface(child->type);

    fprintf(out, ", ");
    output_c_type(child->type, out, 0);
    fprintf(out, " ");
    if (wantPtr)
      fprintf(out, "*");
    fprintf(out, "%s", child->name);
    /* Normally would call output_c_type_trailer, but that is only
       used to add the trailing "[size]" to vectors, and we don't want
       that in the procedure signature. 

       output_c_type_trailer(child->type, out, 0);
    */
  }

  fprintf(out, ")\n{\n");

  /***************************************************************
   * Function body.
   ***************************************************************/
    
  /* Setup generic message structure. Some of this will be overwritten 
     below, but the compiler's CSE/DCE optimizations will eliminate
     the redundancy. */
  do_indent(out, indent + 2);
  fprintf(out, "Message msg;\n\n");

  if (needSendString == 1) {
    do_indent(out, indent + 2);
    fprintf(out, "unsigned char *sndData;\n");
    do_indent(out, indent + 2);
    fprintf(out, "unsigned sndLen = 0;\n");
    do_indent(out, indent + 2);
    fprintf(out, "unsigned sndIndir = 0;\n");
  }
  if (needRcvString == 1) {
    do_indent(out, indent + 2);
    fprintf(out, "unsigned char *rcvData;\n");
    do_indent(out, indent + 2);
    fprintf(out, "unsigned rcvLen = 0;\n");
    do_indent(out, indent + 2);
    fprintf(out, "unsigned rcvIndir = 0;\n");
  }

  if (needSendString == 1) {
    unsigned align = 0xfu;	/* alloca returns word-aligned storage */
    fprintf(out, "\n");
    do_indent(out, indent + 2);
    fprintf(out, "/* send string size computation */\n");
    align = emit_direct_byte_computation(sndString, out, indent+2,
					 sc_formal, align);
    /* Align up to an 8 byte boundary to begin the indirect bytes */
    align = emit_symbol_align("sndLen", out, indent+2, 8, align);

    do_indent(out, indent + 2);
    fprintf(out, "sndIndir = sndLen;\n");
    align = emit_indirect_byte_computation(sndString, out, indent+2,
					   sc_formal, align);
    do_indent(out, indent + 2);
    fprintf(out, "sndData = alloca(sndLen);\n");
  }
  if (needRcvString == 1) {
    unsigned align = 0xfu;	/* alloca returns word-aligned storage */
    fprintf(out, "\n");
    do_indent(out, indent + 2);
    fprintf(out, "/* receive string size computation */\n");
    align = emit_direct_byte_computation(rcvString, out, indent+2,
					 sc_outformal, align);

    /* Align up to an 8 byte boundary to begin the indirect bytes */
    align = emit_symbol_align("rcvLen", out, indent+2, 8, align);

    do_indent(out, indent + 2);
    fprintf(out, "rcvIndir = rcvLen;\n");

    emit_indirect_byte_computation(rcvString, out, indent+2,
				   sc_outformal, align);
    do_indent(out, indent + 2);
    fprintf(out, "rcvData = alloca(rcvLen);\n");
  }
    
  fprintf(out, "\n");

  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_invKey = _self;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_code = OC_%s;\n", symbol_QualifiedName(s,'_'));
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_w1 = 0;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_w2 = 0;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_w3 = 0;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_len = 0;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_key0 = KR_VOID;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_key1 = KR_VOID;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_key2 = KR_VOID;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.snd_rsmkey = KR_VOID;\n");

  fprintf(out, "\n");

  do_indent(out, indent + 2);
  fprintf(out, "msg.rcv_limit = 0;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.rcv_key0 = KR_VOID;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.rcv_key1 = KR_VOID;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.rcv_key2 = KR_VOID;\n");
  do_indent(out, indent + 2);
  fprintf(out, "msg.rcv_rsmkey = KR_VOID;\n");

  fputc('\n', out);

  /* Reserve slot for the return type first, if it is registerizable: */
  rcv_regcount += can_registerize(s->type, rcv_regcount);

  /* Sent registerizable arguments */
  for(i = 0; i < vec_len(sndRegs); i++) {
    Symbol *child = symvec_fetch(sndRegs,i);

    if (symbol_IsInterface(child->type)) {
      if (snd_capcount >= 3)
	diag_fatal(1, "Too many capabilities transmitted\n");

      do_indent(out, indent + 2);
      fprintf(out, "msg.snd_key%d = %s;\n", 
	      snd_capcount++, child->name);
    }
    else if ((needRegs = can_registerize(child->type, snd_regcount))) {
      emit_registerize(out, child, indent + 2, snd_regcount);
      snd_regcount += needRegs;
    }
  }

  /* Received registerizable arguments */
  for(i = 0; i < vec_len(rcvRegs); i++) {
    Symbol *child = symvec_fetch(rcvRegs,i);

    if (symbol_IsInterface(child->type)) {
      loadRcvCap(out, indent, child->name);
    }
    else if ((needRegs = can_registerize(child->type, rcv_regcount))) {
      /* do nothing -- handled through registerization below */
      rcv_regcount += needRegs;
    }
  }

  /* If the return type is a capability type, convert it to an out
     parameter at the end of the parameter list */
  if (symbol_IsInterface(s->type)) {
    loadRcvCap(out, indent, "_resultCap");
  }

  emit_send_string(sndString, out, indent+2);

  emit_receive_string(rcvString, out, indent+2);

  if (needSendString || needRcvString)
    fputc('\n', out);

  do_indent(out, indent + 2);
  fprintf(out, "CALL(&msg);\n");
  fputc('\n', out);

  rcv_regcount = 1;

  do_indent(out, indent+2);
  fprintf(out, "if (msg.rcv_code != RC_OK) return msg.rcv_code;\n");

  /* Unpack the registers and the structure (if any) that contain the
     returned arguments (if any) */
  emit_unpack_return_registers(rcvRegs, out, indent);
  if (needRcvString)
    emit_unpack_return_string(rcvString, out, indent + 2);

  do_indent(out, indent + 2);
  fprintf(out, "return msg.rcv_code;\n");

  fprintf(out, "}\n");
}

#if 0
static bool
output_server_message_strings(FILE *out, Symbol *s, SymClass sc, int indent)
{
  unsigned i;
  bool haveString = false;

  if (s->baseType) {
    Symbol *base = s->baseType;
    while (base->cls == sc_symRef)
      base = base->value;

    output_server_message_strings(out, base, sc, indent);
  }

  for(i = 0; i < vec_len(s->children); i++) {
    InternedString nm;
   Symbol *op = symvec_fetch(s->children,i);
    if (op->cls != sc_operation)
      continue;

    {
      char *tmp = VMALLOC(char, strlen("OP_") + strlen(op->name) + 1);
      strcpy(tmp, "OP_");
      strcat(tmp, op->name);

      nm = intern(tmp);
      free(tmp);
    }

    if (setup_message_string(op, out, sc, indent, 
			     nm, false))
      haveString = true;
  }

  return haveString;
}
#endif

#if 0
/** This is the dual of the client-side stub generation, but somewhat
    more complicated by the need to pre-allocate the storage earlier
    in the procedure and perform all of the necessary casts.  Input
    symbol /op/ is the sc_operation symbol. */
static void
output_server_dispatch_args(FILE *out, Symbol *op)
{
  unsigned nRcvReg = 1;
  unsigned nRcvCap = 1;
  unsigned nSndReg = 1;
  unsigned nSndCap = 1;

  char *comma = "";

  for(unsigned i = 0; i < vec_len(op->children); i++) {
    Symbol *arg = op->children[i];

    out << comma;

    if (arg->cls == sc_formal) {
      if (symbol_IsInterface(arg->type))
	out << "msg->rcv_key" << nRcvCap++;
      else if (can_registerize(arg->type) && nRcvReg < MAX_REGS) {
	out << '(';
	output_c_type(arg->type, out, 0);
	out << ") msg->rcv_w" << nRcvReg++;
      }
      else
	out << "TheInputString.OP_" << op->name
	    << '.' << arg->name;
    }
    else /* sc_outformal */ {
      if (symbol_IsInterface(arg->type))
	out << "msg->snd_key" << nSndCap++;
      else if (can_registerize(arg->type) && nSndReg < MAX_REGS)
	out << '&' << arg->name;
      else
	out << "TheOutputString.OP_" << op->name
	    << '.' << arg->name;
    }

    comma = ", ";
  }
}
#endif

#if 0
static void
output_server_dispatch(FILE *out, Symbol *s, int indent)
{
  if (s->baseType) {
    Symbol *base = s->baseType;
    while (base->cls == sc_symRef)
      base = base->value;

    output_server_dispatch(out, base, indent);
  }

  fputc('\n', out);
  do_indent(out, indent);
  out << "/* dispatch for " << s->QualifiedName() << " */\n";

  for(unsigned i = 0; i < vec_len(s->children); i++) {
    Symbol *op = symvec_fetch(s->children,i);
    if (op->cls != sc_operation)
      continue;

    do_indent(out, indent);
    out << "case KT_" 
	<< op->QualifiedName()
	<< ':' << endl;
    do_indent(out, indent+2);
    out << '{' << endl;

    {
      unsigned nSndReg = 1;
      for (unsigned a = 0; a < vec_len(op->children); a++) {
	Symbol *arg = symvec_fetch(op->children,a);

	if (arg->cls == sc_outformal 
	    && can_registerize(arg->type)
	    && nSndReg < MAX_REGS) {

	  do_indent(out, indent+4);
	  output_c_type(arg->type, out, 0);
	  out << ' ' << arg->name << ';'
	      << endl;
	}
      }
    }

    do_indent(out, indent+4);
    out << "/* Do something vaguely purposeful */\n";
    do_indent(out, indent+4);
    out << "msg.snd_code = OP_" 
	<< op->name
	<< "(";

    output_server_dispatch_args(out, op);

    out << ");\n";

    do_indent(out, indent+4);
    out <<"break;\n";
    do_indent(out, indent+2);
    out << '}' << endl;
  }
}
#endif

#if 0
static void
output_server_dispatch(FILE *out, Symbol *s, int indent)
{
  unsigned i;
  if (s->baseType) {
    Symbol *base = s->baseType;
    while (base->cls == sc_symRef)
      base = base->value;

    output_server_dispatch(out, base, indent);
  }

  fputc('\n', out);
  do_indent(out, indent);
  fprintf(out, "/* dispatch for %s */\n", symbol_QualifiedName(s,'_'));

  for(i = 0; i < vec_len(s->children); i++) {
    Symbol *op = symvec_fetch(s->children,i);
    if (op->cls != sc_operation)
      continue;

    do_indent(out, indent);
    fprintf(out, "case KT_%s:\n",symbol_QualifiedName(op,'_'));
    do_indent(out, indent+2);
    fprintf(out, "{\n");

    do_indent(out, indent+4);
    fprintf(out, "done = OP_%s(pMsg", op->name);

    if (c_op_needs_message_string(op, sc_formal))
      fprintf(out, ", (const OP_IS_%s *) pMsg->rcv_data", op->name);

    if (c_op_needs_message_string(op, sc_outformal))
      fprintf(out, ", (OP_OS_%s *) pMsg->snd_data", op->name);

    fprintf(out, ");\n");

    if (c_op_needs_message_string(op, sc_outformal)) {
      do_indent(out, indent+4);
      fprintf(out, "pMsg->snd_len = sizeof(OP_OS_%s);\n", op->name);
    }

    do_indent(out, indent+4);
    fprintf(out, "break;\n");
    do_indent(out, indent+2);
    fprintf(out, "}\n");
  }
}

static void
output_interface_dispatch(FILE *out, Symbol *s, int indent)
{
  fprintf(out, 
	  "unsigned\n"
	  "DISPATCH_%s(Message *pMsg)\n",
	  symbol_QualifiedName(s,'_'));
  fprintf(out, "{\n");

  do_indent(out, indent+2);
  fprintf(out, "unsigned done = 0;\n\n");

  do_indent(out, indent+2);
  fprintf(out, "switch(pMsg->rcv_code) {\n");

  output_server_dispatch(out, s, indent+4);

  do_indent(out, indent+4);
  fprintf(out, "default:\n");

  do_indent(out, indent+6);
  fprintf(out, "{\n");

  do_indent(out, indent+8);
  fprintf(out, "pMsg->snd_code = RC_capros_key_UnknownRequest;\n");

  do_indent(out, indent+8);
  fprintf(out, "break;\n");

  do_indent(out, indent+6);
  fprintf(out, "}\n");

  do_indent(out, indent+2);
  fprintf(out, "}\n");

  fputc('\n', out);

  do_indent(out, indent+2);
  fprintf(out, "return done;\n");

  fprintf(out, "}\n\n");
}
#endif

#if 0
static void
output_server_dispatchers(FILE *out, Symbol *s, int indent)
{
  fprintf(out, "%s\n", preamble);

  output_interface_dispatch(out, s, indent);

#if 0
  out << "int"
      << endl
      << "PROCESS_"
      << symbol_QualifiedName(s,'_')
      << "(void)\n";
  out << '{' << endl;
  do_indent(out, indent+2);
  fprintf(out, "/* Kilroy was here */\n");

  do_indent(out, indent+2);
  fprintf(out, "unsigned done = 0;\n");

  do_indent(out, indent+2);
  fprintf(out, "Message msg;\n");

  if (needInputString || needOutputString) {
    fputc('\n', out);

    if (needInputString) {
      do_indent(out, indent+2);
      out << "IF_IS_" << s->name << " TheInputString;\n";
    }

    if (needOutputString) {
      do_indent(out, indent+2);
      out << "IF_OS_" << s->name << " TheOutputString;\n";
    }

    fputc('\n', out);
  }

  do_indent(out, indent+2);
  fprintf(out, "msg.snd_code = 0;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_w1 = 0;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_w2 = 0;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_w3 = 0;\n");

  do_indent(out, indent+2);
  fprintf(out, "msg.snd_invKey = KR_VOID;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_key0 = KR_VOID;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_key1 = KR_VOID;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_key2 = KR_VOID;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.snd_rsmkey = KR_VOID;\n");

  do_indent(out, indent+2);
  fprintf(out, "msg.rcv_key0 = KR_RETURN;\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.rcv_key1 = KR_ARG(0);\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.rcv_key2 = KR_ARG(1);\n");
  do_indent(out, indent+2);
  fprintf(out, "msg.rcv_rsmkey = KR_ARG(2);\n");
  
  if (needInputString) {
    do_indent(out, indent+2);
    fprintf(out, "msg.rcv_data = &TheInputString;\n");
    do_indent(out, indent+2);
    fprintf(out, "msg.rcv_limit = sizeof(TheInputString);\n");
  }
  if (needOutputString) {
    do_indent(out, indent+2);
    fprintf(out, "msg.snd_data = &TheOutputString;\n");
    do_indent(out, indent+2);
    fprintf(out, "msg.snd_len = 0;\n");
  }

  fputc('\n', out);

  do_indent(out, indent+2);
  fprintf(out, "while (!done) {\n");

  do_indent(out, indent+4);
  fprintf(out, "RETURN(&msg);\n");
  fputc('\n', out);

  do_indent(out, indent+4);
  fprintf(out, "msg.snd_invKey = KR_RETURN;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_key0 = KR_VOID;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_key1 = KR_VOID;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_key2 = KR_VOID;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_rsmkey = KR_VOID;\n");

  do_indent(out, indent+4);
  fprintf(out, "msg.snd_code = 0;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_w1 = 0;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_w2 = 0;\n");
  do_indent(out, indent+4);
  fprintf(out, "msg.snd_w3 = 0;\n");

  do_indent(out, indent+4);
  fprintf(out, "msg.snd_len = 0;\n");

  fputc('\n', out);

  do_indent(out, indent+4);
  out << "done = DISPATCH_"
      << symbol_QualifiedName(s,'_')
      << "(&msg);"
      << endl
      << endl;

  do_indent(out, indent+4);
  fprintf(out, "if (msg.snd_code != RC_OK) msg.snd_len = 0;\n");
  if (needInputString) {
    fputc('\n', out);
    do_indent(out, indent+4);
    fprintf(out, "msg.rcv_data = &TheInputString;\n");
    do_indent(out, indent+4);
    fprintf(out, "msg.rcv_limit = sizeof(TheInputString);\n");
  }

  do_indent(out, indent+2);
  fprintf(out, "}\n");

  out << '}' << endl;
#endif
}
#endif

static void
symdump(Symbol *s, FILE *out, int indent)
{
  do_indent(out, indent);

  switch(s->cls){
  case sc_absinterface:
    // enabling stubs for abstract interface.  This can be double checked later
  case sc_interface:
    {
      unsigned i;
#if 0
      extern bool opt_dispatchers;

      if (opt_dispatchers) {
	extern const char *target;
	InternedString fileName;
	FILE *out;

	{
	  const char *sqn = symbol_QualifiedName(s, '_');
	  char *tmp = 
	    VMALLOC(char,
		    strlen(target) + strlen("/") + strlen(sqn) 
		    + strlen(".c") + 1);

	  strcpy(tmp, target);
	  strcat(tmp, "/");
	  strcat(tmp, sqn);
	  strcat(tmp, ".c");

	  fileName = intern(tmp);
	  free(tmp);
	}

	out = fopen(fileName, "w");
	if (out == NULL)
	  diag_fatal(1, "Couldn't open stub source file \"%s\"\n",
		      fileName);

	output_server_dispatchers(out, s, indent);
      }
#endif

      for(i = 0; i < vec_len(s->children); i++)
	symdump(symvec_fetch(s->children,i), out, indent);

      break;
    }
  case sc_package:
  case sc_scope:
  case sc_enum:
    {
      unsigned i;

      for(i = 0; i < vec_len(s->children); i++)
	symdump(symvec_fetch(s->children,i), out, indent);

      break;
    }
  case sc_struct:
    {
#if 0
      extern const char *target;
      InternedString fileName = target;
      fileName << "/";
      fileName << symbol_QualifiedName(s,'_');
      fileName << ".c";

      cout << "  Creating: " << fileName << endl;

      ofstream out(fileName);
      if (!out.is_open())
	diag_fatal(1, "Couldn't open stub source file \"%s\"\n",
		    fileName);

      output_client_struct(out, s, indent);
#endif

      break;
    }

  case sc_operation:
    {
      if ((s->flags & SF_NOSTUB) == 0) {
	FILE *out;
	extern const char *target;
	InternedString fileName;

	{
	  const char *sqn = symbol_QualifiedName(s, '_');
	  char *tmp = 
	    VMALLOC(char,
		    strlen(target) + strlen("/") + strlen(sqn) 
		    + strlen(".c") + 1);

	  strcpy(tmp, target);
	  strcat(tmp, "/");
	  strcat(tmp, symbol_QualifiedName(s,'_'));
	  strcat(tmp, ".c");
	
	  fileName = intern(tmp);
	  free(tmp);
	}

	out = fopen(fileName, "w");
	if (out == NULL)
	  diag_fatal(1, "Couldn't open stub source file \"%s\"\n",
		     fileName);

	output_client_stub(out, s, indent);
      }

      break;
    }
  case sc_const:
#if 0
    {
      MP_INT mi = compute_value(s->value);
      fprintf(out, "const %s %s = %d;\n", 
	       c_typename(s->type), 
	       symbol_QualifiedName(s,'_'), 
	       mpz_get_ui(&mi));
      break;
    }
#endif
  case sc_exception:
  case sc_typedef:
    {
      /* Ignore -- handled in header generator */
      break;
    }
  default:
    {
      fprintf(out, "UNKNOWN/BAD SYMBOL CLASS %s FOR: %s\n", 
	      symbol_ClassName(s), s->name);
      break;
    }
  }
}

void
output_c_stubs(Symbol *s)
{
  unsigned i;
  PtrVec *vec;
  extern const char *target;

  if (s->isActiveUOC == false)
    return;

  path_smkdir(target);

#if 0	// This crashes if s->cls == sc_scope.
  if (symbol_IsFixedSerializable(s) == false)
    diag_fatal(1, "Type \"%s\" is not serializable in bounded space\n", 
		symbol_QualifiedName(s,'.'));
#endif

  preamble = buffer_create();

  buffer_appendString(preamble, "#include <stdbool.h>\n");
  buffer_appendString(preamble, "#include <stddef.h>\n");
  buffer_appendString(preamble, "#include <alloca.h>\n");
  buffer_appendString(preamble, "#include <eros/target.h>\n");
  buffer_appendString(preamble, "#include <eros/Invoke.h>\n");
  // buffer_appendString(preamble, "#include <eros/capidl.h>\n");

  vec = ptrvec_create();

  symbol_ComputeDependencies(s, vec);

  ptrvec_sort_using(vec, symbol_SortByQualifiedName);

  for (i = 0; i < vec_len(vec); i++) {
    buffer_appendString(preamble, "#include <idl/");
    buffer_appendString(preamble, symbol_QualifiedName(symvec_fetch(vec,i), '/'));
    buffer_appendString(preamble, ".h>\n");
  }

  buffer_appendString(preamble, "#include <idl/");
  buffer_appendString(preamble, symbol_QualifiedName(s, '/'));
  buffer_appendString(preamble, ".h>\n");

  /* Blank line between #include's and content: */
  buffer_appendString(preamble, "\n");

  buffer_freeze(preamble);

  symdump(s, stdout, 0);
}

