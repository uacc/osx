/* fredia.c
   Free dialer information.

   Copyright (C) 1992, 2002 Ian Lance Taylor

   This file is part of the Taylor UUCP uuconf library.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License
   as published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.

   The author of the program may be contacted at ian@airs.com.
   */

#include "uucnfi.h"

#if USE_RCS_ID
const char _uuconf_fredia_rcsid[] = "$Id: fredia.c,v 1.8 2002/03/05 19:10:42 ian Rel $";
#endif

/* Free the memory allocated for a dialer.  */

#undef uuconf_dialer_free

/*ARGSUSED*/
int
uuconf_dialer_free (pglobal, qdialer)
     pointer pglobal ATTRIBUTE_UNUSED;
     struct uuconf_dialer *qdialer;
{
  uuconf_free_block (qdialer->uuconf_palloc);
  return UUCONF_SUCCESS;
}
