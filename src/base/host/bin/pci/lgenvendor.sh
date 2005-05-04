#
# Copyright (C) 1998, 1999, Jonathan S. Shapiro.
#
# This file is part of the EROS Operating System.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2,
# or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#


#
# lgenvendor: generate the linux vendor list
#

if [ -f /usr/include/linux/pci.h ]
then
	grep PCI_VENDOR_ID /usr/include/linux/pci.h | \
	   awk '{print $3 " " $2}' | grep -v 'PCI_VENDOR_ID$'
fi
