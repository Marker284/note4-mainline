/*
 * Copyright 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *  
 * A copy of the GPL is available at 
 * http://www.broadcom.com/licenses/GPLv2.php, or by writing to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * The BBD (Broadcom Bridge Driver)
 *
 * tabstop = 8
 */

#ifndef __BBD_DEBUG_H__ // {
#define __BBD_DEBUG_H__

bool bbd_debug(void);

/* Runtime-switchable debug output (BBD:DEBUG=1 on /dev/bbd_control or
 * /sys/devices/platform/bbd/debug). */
#define dprint(fmt, args...) \
	do { if (bbd_debug()) pr_info("BBD:%s()/%d " fmt, __func__, __LINE__, ##args); } while (0)

#define FUNC()     dprint("\n")
#define FUNS(s)    dprint("\"%s\"\n", s)
#define FUNI(i)    dprint("%lu\n", (unsigned long)(i))
#define FUNSI(s,i) dprint("\"%s\",%lu\n", s, (unsigned long)(i))

void bbd_log_hex(const char*          pIntroduction,
                 const unsigned char* pData,
                 unsigned long        ulDataLen);


#endif // } __BBD_DEBUG_H__
