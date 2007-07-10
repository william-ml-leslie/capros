/*
 * Copyright (C) 1998, 1999, Jonathan S. Shapiro.
 *
 * This file is part of the EROS Operating System runtime library.
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

#include <eros/target.h>
#include <eros/Invoke.h>

#include <idl/capros/Number.h>

uint32_t
eros_number_getValue(uint32_t krNumber, capros_Number_value *nkv)
{
  uint32_t w0, w1, w2;
  uint32_t result = capros_Number_get(krNumber, &w0, &w1, &w2);

  if (result == RC_OK) {
    nkv->value[0] = w0;
    nkv->value[1] = w1;
    nkv->value[2] = w2;
  }

  return result;
}


