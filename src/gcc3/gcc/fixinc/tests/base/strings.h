/*  DO NOT EDIT THIS FILE.

    It has been auto-edited by fixincludes from:

	"fixinc/tests/inc/strings.h"

    This had to be done to correct non-standard usages in the
    original, manufacturer supplied header file.  */

#ifndef FIXINC_ULTRIX_STRINGS_CHECK
#define FIXINC_ULTRIX_STRINGS_CHECK 1



#if defined( SUNOS_STRLEN_CHECK )
 __SIZE_TYPE__ strlen(); /* string length */
#endif  /* SUNOS_STRLEN_CHECK */


#if defined( ULTRIX_STRINGS_CHECK )
@(#)strings.h   6.1     (ULTRIX)

#endif  /* ULTRIX_STRINGS_CHECK */

#endif  /* FIXINC_ULTRIX_STRINGS_CHECK */
