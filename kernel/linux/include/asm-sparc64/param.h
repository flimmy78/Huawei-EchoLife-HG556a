/* $Id: param.h,v 1.1 2008/08/26 05:11:54 l65130 Exp $ */
#ifndef _ASMSPARC64_PARAM_H
#define _ASMSPARC64_PARAM_H

#ifdef __KERNEL__
# define HZ		1000	/* Internal kernel timer frequency */
# define USER_HZ	100	/* .. some user interfaces are in "ticks" */
# define CLOCKS_PER_SEC (USER_HZ)
#endif

#ifndef HZ
#define HZ 100
#endif

#define EXEC_PAGESIZE	8192    /* Thanks for sun4's we carry baggage... */

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#endif /* _ASMSPARC64_PARAM_H */
