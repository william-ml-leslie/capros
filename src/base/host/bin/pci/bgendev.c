/*
 * Copyright (C) 1998, 1999, Jonathan S. Shapiro.
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


#include <ctype.h>
#include "boemler.list.c"

main()
{
  int i = 0;

  /* Walk the vendor table, generating vendor macros: */
  while (PciDevTable[i].VenId != 0xffff) {
    char *devName = PciDevTable[i].ChipDesc;

    printf("0x%04x ", PciDevTable[i].DevId);

    printf("PCI_DEVICE_ID_");

    if (*devName == 0)
      printf("NO_NAME");
    
    while (*devName) {
      if (isalnum (*devName))
	putchar( isalpha(*devName) ? toupper(*devName) : *devName );
      else
	putchar('_');
      
      devName ++;
    }

    printf("\n");

    i++;
  }

  exit(0);
}
