/*
    Copyright (C) 2003-2005  Thomas Ries <tries@gmx.net>

    This file is part of Siproxd.
    
    Siproxd is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    
    Siproxd is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warrantry of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Siproxd; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
*/

#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

#ifdef HAVE_PTHREAD_SETSCHEDPARAM
   #include <sched.h>
#endif

#include <osipparser2/osip_parser.h>

#include "siproxd.h"
#include "rtpproxy.h"
#include "log.h"

static char const ident[]="$Id: rtpproxy_relay.c,v 1.1 2008/08/25 13:42:51 l65130 Exp $";

/* configuration storage */
extern struct siproxd_config configuration;

/*
 * table to remember all active rtp proxy streams
 */
rtp_proxytable_t rtp_proxytable[RTPPROXY_SIZE];

/*
 * Mutex for thread synchronization (locking when accessing common 
 * data structures -> rtp_proxytable[]).
 *
 * use a 'fast' mutex for synchronizing - as these are portable... 
 */
static pthread_mutex_t rtp_proxytable_mutex = PTHREAD_MUTEX_INITIALIZER;

/* thread id of RTP proxy */
static pthread_t rtpproxy_tid=0;

/* master fd_set */
static fd_set master_fdset;
static int    master_fd_max;

/* forward declarations */
static void *rtpproxy_main(void *i);
static int rtp_recreate_fdset(void);
void rtpproxy_kill( void );
static void sighdl_alm(int sig) {/* just wake up from select() */};


/*
 * initialize and create rtp_relay proxy thread
 *
 * RETURNS
 *	STS_SUCCESS on success
 */
int rtp_relay_init( void ) {
   int sts;
   int arg=0;
   struct sigaction sigact;

   atexit(rtpproxy_kill);  /* cancel RTP thread at exit */

   /* clean proxy table */
   memset (rtp_proxytable, 0, sizeof(rtp_proxytable));

   /* initialize fd set for RTP proxy thread */
   FD_ZERO(&master_fdset); /* start with an empty fdset */
   master_fd_max=-1;

   /* install signal handler for SIGALRM - used to wake up
      the rtpproxy thread from select() hibernation */
   sigact.sa_handler = sighdl_alm;
   sigemptyset(&sigact.sa_mask);
   sigact.sa_flags=0;
   sigaction(SIGALRM, &sigact, NULL);

   DEBUGC(DBCLASS_RTP,"create thread");
   sts=pthread_create(&rtpproxy_tid, NULL, rtpproxy_main, (void *)&arg);
   DEBUGC(DBCLASS_RTP,"created, sts=%i", sts);

   /* set realtime scheduling - if started by root */
#ifdef HAVE_PTHREAD_SETSCHEDPARAM
   {
      int uid,euid;
      struct sched_param schedparam;

#ifndef _CYGWIN
      uid=getuid();
      euid=geteuid();
      DEBUGC(DBCLASS_RTP,"uid=%i, euid=%i", uid, euid);
      if (uid != euid) seteuid(0);

      if (geteuid()==0) {
#endif

#if defined(HAVE_SCHED_GET_PRIORITY_MAX) && defined(HAVE_SCHED_GET_PRIORITY_MIN)
         int pmin, pmax;
         /* place ourself at 1/3 of the available priority space */
         pmin=sched_get_priority_min(SCHED_RR);
         pmax=sched_get_priority_max(SCHED_RR);
         schedparam.sched_priority=pmin+(pmax-pmin)/3;
         DEBUGC(DBCLASS_RTP,"pmin=%i, pmax=%i, using p=%i", pmin, pmax,
                schedparam.sched_priority);
#else
         /* just taken a number out of thin air */
         schedparam.sched_priority=10;
         DEBUGC(DBCLASS_RTP,"using p=%i", schedparam.sched_priority);
#endif
         sts=pthread_setschedparam(rtpproxy_tid, SCHED_RR, &schedparam);
         if (sts != 0) {
            ERROR("pthread_setschedparam failed: %s", strerror(errno));
         }
#ifndef _CYGWIN
      } else {
         INFO("Unable to use realtime scheduling for RTP proxy");
         INFO("You may want to start siproxd as root and switch UID afterwards");
      }
      if (uid != euid)  seteuid(euid);
#endif
   }
#endif

   return STS_SUCCESS;
}


/*
 * main() of rtpproxy
 */
static void *rtpproxy_main(void *arg) {
   struct timeval tv;
   fd_set fdset;
   int fd_max;
   time_t t, last_t=0;
   int i, sts;
   int num_fd;
   osip_call_id_t callid;
   static char rtp_buff[RTP_BUFFER_SIZE];
   int count;
   int j;

   memcpy(&fdset, &master_fdset, sizeof(fdset));
   fd_max=master_fd_max;

   /* loop forever... */
   for (;;) {

      tv.tv_sec = 5;
      tv.tv_usec = 0;

      num_fd=select(fd_max+1, &fdset, NULL, NULL, &tv);
      pthread_testcancel();
      if ((num_fd<0) && (errno==EINTR)) {
         /*
          * wakeup due to a change in the proxy table:
          * lock mutex, copy master FD set and unlock
          */
         pthread_mutex_lock(&rtp_proxytable_mutex);
         memcpy(&fdset, &master_fdset, sizeof(fdset));
         fd_max=master_fd_max;
         pthread_mutex_unlock(&rtp_proxytable_mutex);
         continue;
      }

      time(&t);

      /*
       * LOCK the MUTEX
       */
      pthread_mutex_lock(&rtp_proxytable_mutex);

      /* check for data available and send to destination */
      for (i=0;(i<RTPPROXY_SIZE) && (num_fd>0);i++) {
         if ( (rtp_proxytable[i].rtp_rx_sock != 0) && 
            FD_ISSET(rtp_proxytable[i].rtp_rx_sock, &fdset) ) {
            /* yup, have some data to send */
            num_fd--;

	    /* read from sock rtp_proxytable[i].sock*/
            count=read(rtp_proxytable[i].rtp_rx_sock, rtp_buff, RTP_BUFFER_SIZE);

            /* check if something went banana */
            if (count < 0) {
               /*
                * It has been seen on linux 2.2.x systems that for some
                * reason (ICMP issue? -> below) inside the RTP relay, select()
                * claims that a certain file descriptor has data available to
                * read, a subsequent call to read() or recv() then does block!!
                * So lets make the FD's we are going to use non-blocking, so
                * we will at least survive and not run into a deadlock.
                * 
                * We catch this here with this workaround (pronounce "HACK")
                * and hope that next time we pass by it will be ok again.
                */
               if (errno == EAGAIN) {
                  /*&&&& I may want to remove this WARNing */
                  WARN("read() [fd=%i, %s:%i] would block, but select() "
                       "claimed to be readable!",
                       rtp_proxytable[i].rtp_rx_sock,
                       utils_inet_ntoa(rtp_proxytable[i].local_ipaddr),
                       rtp_proxytable[i].local_port);
                  continue;
               }

               /*
                * I *MAY* receive ICMP destination unreachable messages when I
                * try to send RTP traffic to a destination that is in HOLD
                * (better: is not listening on the UDP port where I send
                * my RTP data to).
                * So I should *not* do this - or ignore errors originating
                * by this -> ECONNREFUSED
                *
                * Note: This error is originating from a previous send() on the
                *       same socket and has nothing to do with the read() we have
                *       done above!
                */
               if (errno != ECONNREFUSED) {
                  /* some other error that I probably want to know about */
                  int j;
                  WARN("read() [fd=%i, %s:%i] returned error [%i:%s]",
                  rtp_proxytable[i].rtp_rx_sock,
                  utils_inet_ntoa(rtp_proxytable[i].local_ipaddr),
                  rtp_proxytable[i].local_port, errno, strerror(errno));
                  for (j=0; j<RTPPROXY_SIZE;j++) {
                     DEBUGC(DBCLASS_RTP, "%i - rx:%i tx:%i %s@%s dir:%i "
                            "lp:%i, rp:%i rip:%s",
                            j,
                            rtp_proxytable[j].rtp_rx_sock,
                            rtp_proxytable[j].rtp_tx_sock,
                            rtp_proxytable[j].callid_number,
                            rtp_proxytable[j].callid_host,
                            rtp_proxytable[j].direction,
                            rtp_proxytable[j].local_port,
                            rtp_proxytable[j].remote_port,
                            utils_inet_ntoa(rtp_proxytable[j].remote_ipaddr));
                  } /* for j */
              } /* if errno != ECONNREFUSED */
            } /* count < 0 */

            /*
             * forwarding an RTP packet only makes sense if we really
             * have got some data in it (count > 0)
             */
            if (count > 0) {
               /* find the corresponding TX socket */
               if (rtp_proxytable[i].rtp_tx_sock == 0) {
                  int rtp_direction = rtp_proxytable[i].direction;
                  //int media_stream_no = rtp_proxytable[i].media_stream_no;

                  callid.number = rtp_proxytable[i].callid_number;
                  callid.host = rtp_proxytable[i].callid_host;

                  for (j=0;(j<RTPPROXY_SIZE);j++) {
                     char *client_id = rtp_proxytable[i].client_id;
                     osip_call_id_t cid;
                     cid.number = rtp_proxytable[j].callid_number;
                     cid.host = rtp_proxytable[j].callid_host;

                     /* match on:
                      * - same call ID
                      * - same media stream
                      * - opposite direction
                      * - different client ID
                      */
                     if ( (rtp_proxytable[j].rtp_rx_sock != 0) &&
                          (compare_callid(&callid, &cid) == STS_SUCCESS) &&
                          //(media_stream_no == rtp_proxytable[j].media_stream_no) &&
                          (rtp_direction != rtp_proxytable[j].direction) &&
                          (strcmp(rtp_proxytable[j].client_id, client_id) != 0) ) {
                        rtp_proxytable[i].rtp_tx_sock = rtp_proxytable[j].rtp_rx_sock;
                        DEBUGC(DBCLASS_RTP, "connected entry %i (fd=%i) <-> entry %i (fd=%i)",
                               j, rtp_proxytable[j].rtp_rx_sock,
                               i, rtp_proxytable[i].rtp_rx_sock);
                        break;
                     }
                  }
               } /* rtp_tx_sock == 0 */

               if (rtp_proxytable[i].rtp_tx_sock != 0) {
                  /* write to dest via socket rtp_tx_sock */
                  struct sockaddr_in dst_addr;
                  dst_addr.sin_family = AF_INET;
                  memcpy(&dst_addr.sin_addr.s_addr,
                         &rtp_proxytable[i].remote_ipaddr, 
                         sizeof(struct in_addr));
                  dst_addr.sin_port= htons(rtp_proxytable[i].remote_port);

                  sts = sendto(rtp_proxytable[i].rtp_tx_sock, rtp_buff,
                               count, 0, (const struct sockaddr *)&dst_addr,
                               (socklen_t)sizeof(dst_addr));
                  if (sts == -1) {
                     if (errno != ECONNREFUSED) {
                        ERROR("sendto() [%s:%i size=%i] call failed: %s",
                        utils_inet_ntoa(rtp_proxytable[i].remote_ipaddr),
                        rtp_proxytable[i].remote_port, count, strerror(errno));

                    /* if sendto() fails with bad filedescriptor,
                     * this means that the opposite stream has been
                     * canceled or timed out.
                     * we should then cancel this stream as well.*/

                    WARN("stopping opposite stream");
                    /* don't lock the mutex, as we own the lock */
                    callid.number=rtp_proxytable[i].callid_number;
                    callid.host=rtp_proxytable[i].callid_host;
                    rtp_relay_stop_fwd(rtp_proxytable[i].rtp_rx_sock, &callid, rtp_proxytable[i].direction, 1);
                     }
                  }
               }
               /* in case of rtp_tx_sock is not same as rtp_rx_sock, need update rtp_tx_sock too */
               for (j=0;(j<RTPPROXY_SIZE);j++) {
                  if (rtp_proxytable[i].rtp_tx_sock == rtp_proxytable[j].rtp_rx_sock) {
                     rtp_proxytable[j].timestamp=t;
                     break;
                  }
               }
            } /* count > 0 */

            /* update timestamp of last usage */
            rtp_proxytable[i].timestamp=t;
         }

         /* handle rtcp packet */
         if ( (rtp_proxytable[i].rtcp_rx_sock != 0) && 
            FD_ISSET(rtp_proxytable[i].rtcp_rx_sock, &fdset) ) {
            /* yup, have some data to send */
            num_fd--;

            /* read from sock rtp_proxytable[i].sock*/
            count=read(rtp_proxytable[i].rtcp_rx_sock, rtp_buff, RTP_BUFFER_SIZE);

            /*
             * forwarding an RTP packet only makes sense if we really
             * have got some data in it (count > 0)
             */
            if (count > 0) {
               /* find the corresponding TX socket */
               if (rtp_proxytable[i].rtcp_tx_sock == 0) {
                  int j;
                  int rtp_direction = rtp_proxytable[i].direction;
                  //int media_stream_no = rtp_proxytable[i].media_stream_no;

                  callid.number = rtp_proxytable[i].callid_number;
                  callid.host = rtp_proxytable[i].callid_host;

                  for (j=0;(j<RTPPROXY_SIZE);j++) {
                     char *client_id = rtp_proxytable[i].client_id;
                     osip_call_id_t cid;
                     cid.number = rtp_proxytable[j].callid_number;
                     cid.host = rtp_proxytable[j].callid_host;

                     if ( (rtp_proxytable[j].rtcp_rx_sock != 0) &&
                          (compare_callid(&callid, &cid) == STS_SUCCESS) &&
                          //(media_stream_no == rtp_proxytable[j].media_stream_no) &&
                          (rtp_direction != rtp_proxytable[j].direction) &&
                          (strcmp(rtp_proxytable[j].client_id, client_id) != 0) ) {
                        rtp_proxytable[i].rtcp_tx_sock = rtp_proxytable[j].rtcp_rx_sock;
                        break;
                     }
                  }
               } /* rtcp_tx_sock == 0 */

               if (rtp_proxytable[i].rtcp_tx_sock != 0) {
                  /* write to dest via socket rtcp_tx_sock */
                  struct sockaddr_in dst_addr;
                  dst_addr.sin_family = AF_INET;
                  memcpy(&dst_addr.sin_addr.s_addr,
                         &rtp_proxytable[i].remote_ipaddr, 
                         sizeof(struct in_addr));
                  dst_addr.sin_port= htons(rtp_proxytable[i].remote_port+1);

                  sts = sendto(rtp_proxytable[i].rtcp_tx_sock, rtp_buff,
                               count, 0, (const struct sockaddr *)&dst_addr,
                               (socklen_t)sizeof(dst_addr));
                  if (sts == -1) {
                     if (errno != ECONNREFUSED) {
                        ERROR("sendto() [%s:%i size=%i] call failed: %s",
                        utils_inet_ntoa(rtp_proxytable[i].remote_ipaddr),
                        rtp_proxytable[i].remote_port+1, count, strerror(errno));

                    /* if sendto() fails with bad filedescriptor,
                     * this means that the opposite stream has been
                     * canceled or timed out.
                     * we should then cancel this stream as well.*/

                    WARN("stopping opposite stream");
                    /* don't lock the mutex, as we own the lock */
                    callid.number=rtp_proxytable[i].callid_number;
                    callid.host=rtp_proxytable[i].callid_host;
                    rtp_relay_stop_fwd(rtp_proxytable[i].rtcp_rx_sock, &callid, rtp_proxytable[i].direction, 1);
                     }
                  }
               }
            } /* count > 0 */

            /* update timestamp of last usage */
            rtp_proxytable[i].timestamp=t;
         }
     } /* for i */

      /*
       * age and clean rtp_proxytable (check every 10 seconds)
       */
      if (t > (last_t+10) ) {
         last_t = t;
	 for (i=0;i<RTPPROXY_SIZE; i++) {
            if ( (rtp_proxytable[i].rtp_rx_sock != 0) &&
		 ((rtp_proxytable[i].timestamp+configuration.rtp_timeout)<t)) {
               /* this one has expired, clean it up */
               callid.number=rtp_proxytable[i].callid_number;
               callid.host=rtp_proxytable[i].callid_host;
               DEBUGC(DBCLASS_RTP,"RTP stream rx_sock=%i tx_sock=%i "
                      "%s@%s (idx=%i) has expired",
                      rtp_proxytable[i].rtp_rx_sock,
                      rtp_proxytable[i].rtp_tx_sock,
                      callid.number, callid.host, i);
               /* don't lock the mutex, as we own the lock already here */
               rtp_relay_stop_fwd(rtp_proxytable[i].rtp_rx_sock, &callid, rtp_proxytable[i].direction, 1);
	    }
	 }
      } /* if (t>...) */

      /* copy master FD set */
      memcpy(&fdset, &master_fdset, sizeof(fdset));
      fd_max=master_fd_max;

      /*
       * UNLOCK the MUTEX
       */
      pthread_mutex_unlock(&rtp_proxytable_mutex);
   } /* for(;;) */

   return NULL;
}


/*
 * start an rtp stream on the proxy
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int rtp_relay_start_fwd (osip_call_id_t *callid, char *client_id,
                         int rtp_direction,
                         int media_stream_no, struct in_addr local_ipaddr,
                         int *local_port, struct in_addr remote_ipaddr,
                         int remote_port) {
   int i, j;
   int sock, port;
   int freeidx;
   int sts=STS_SUCCESS;
   osip_call_id_t cid;
   int rtcp_sock;
   

   if (callid == NULL) {
      ERROR("rtp_relay_start_fwd: callid is NULL!");
      return STS_FAILURE;
   }

   if (client_id == NULL) {
      ERROR("rtp_relay_start_fwd: did not get a client ID!");
      return STS_FAILURE;
   }

   /*
    * life insurance: check size of received call_id strings
    * I don't know what the maximum allowed size within SIP is,
    * so if this test fails maybe it's just necessary to increase
    * the constants CALLIDNUM_SIZE and/or CALLIDHOST_SIZE.
    */
   if (callid->number && strlen(callid->number) > CALLIDNUM_SIZE) {
      ERROR("rtp_relay_start_fwd: received callid number "
            "has too many characters (%i, max=%i)",
            strlen(callid->number),CALLIDNUM_SIZE);
      return STS_FAILURE;
   }
   if (callid->host && strlen(callid->host) > CALLIDHOST_SIZE) {
      ERROR("rtp_relay_start_fwd: received callid host "
            "has too many characters (%i, max=%i)",
            strlen(callid->host),CALLIDHOST_SIZE);
      return STS_FAILURE;
   }
   if (client_id && strlen(client_id) > CLIENT_ID_SIZE) {
      ERROR("rtp_relay_start_fwd: client ID has too many characters "
            "(%i, max=%i) (maybe you need to increase CLIENT_ID_SIZE",
            strlen(client_id),CLIENT_ID_SIZE);
      return STS_FAILURE;
   }

   DEBUGC(DBCLASS_RTP,"rtp_relay_start_fwd: starting RTP proxy "
          "stream for: %s@%s[%s] (%s) #=%i",
          callid->number, callid->host, client_id,
          ((rtp_direction == DIR_INCOMING) ? "incoming RTP" : "outgoing RTP"),
          media_stream_no);

   /* lock mutex */
   #define return is_forbidden_in_this_code_section
   pthread_mutex_lock(&rtp_proxytable_mutex);
   /*
    * !! We now have a locked MUTEX! It is forbidden to return() from
    * !! here up to the end of this funtion where the MUTEX is
    * !! unlocked again.
    * !! Per design, a mutex is locked (for one purpose) at *exactly one*
    * !! place in the code and unlocked also at *exactly one* place.
    * !! this minimizes the risk of deadlocks.
    */

   /*
    * figure out, if this is an request to start an RTP proxy stream
    * that is already existing (identified by SIP Call-ID, direction,
    * media_stream_no and some other client unique thing).
    * This can be due to UDP repetitions of the INVITE request...
    */
   for (i=0; i<RTPPROXY_SIZE; i++) {
      cid.number = rtp_proxytable[i].callid_number;
      cid.host   = rtp_proxytable[i].callid_host;
      /* BEGIN: Modified by pengqiuming(48166), 2007/3/26   PN:A36D03358*/
      /* PROBLEM DESCRIPTION: 网关SIP ALG做主叫T38传真不能成*/
      /* 通过这几个条件来判断这个请求是否有，也就是是否rtp_proxytable[i]记录过，
         如果有记录，那这个函数就结束了
         如果没有记录，就要找到freeidx分配新的rtp_proxytable[freeidx]，并绑定新port的socket 

         当发起传真reinvite时，media_stream_no有时为1，有时为0，client_id有时为ipaddress，有时为username，
         所以新分配的rtp_proxytable[freeidx]和新创建的sockbind（local_ipaddr，port）多次，端口分配了7070，7072，7074，出现混乱，传真就不成功
         正常通话时，SDP 的media description只有audio，所以media_stream_no总是为0，

         当被叫发起传真时，SDP 的media description里有image和audio，image排在前面，audio排在后面，处理image时为0，而audio为1，
         其实这个条件不去掉,T38传真也会成功，但这时audio会分配新的rtp_proxytable[],并audio port变成7072

         整个\userapps\opensource\siproxd\src只处理proxy forward，而sip信令的parse都是在\userapps\opensource\libosip2\src\osipparser2里分析的
         然后传到siproxd_main（）里来，当发起传真时，sip信令的parse就会出问题，有时username为空，client_id就为ipaddress，
         但sip parse的代码非常复杂，而且很关键，不能随便改动，想办法让T38传真时干脆client_id一直为ipaddress，但这样会出现port unreachable等很奇怪的错误，传真不成功

         其实只要保证通话时invite的SDP media descriptionaudio port为7070，传真时reinvite的image port为7070，audio port为7070就能保证正向、反向传真都能成功，把这两个条件都去掉，能满足
         
      if (rtp_proxytable[i].rtp_rx_sock &&
         (compare_callid(callid, &cid) == STS_SUCCESS) &&
         (rtp_proxytable[i].direction == rtp_direction) &&
         (rtp_proxytable[i].media_stream_no == media_stream_no) &&
         (strcmp(rtp_proxytable[i].client_id, client_id) == 0)) 
      */
      if (rtp_proxytable[i].rtp_rx_sock &&
         (compare_callid(callid, &cid) == STS_SUCCESS) &&
         (rtp_proxytable[i].direction == rtp_direction) &&
         //下面选项如果下一个INVITE中对应的流顺序改变，一定要修改成rtp_proxytable[i].remote_port 和 remote_port比较相等
         //可是协议是否规定再次协商时不允许减少媒体流，只允许添加，如果不再支持则置0         
         (rtp_proxytable[i].media_stream_no == media_stream_no) &&  
         (strcmp(rtp_proxytable[i].client_id, client_id) == 0))     //可以考虑去掉  
      {
      /* END:   Modified by pengqiuming(48166), 2007/3/26 */

         /*
          * The RTP port number reported by the UA MAY change
          * for a given media stream
          * (seen with KPhone during HOLD/unHOLD)
          * Also the destination IP may change during a re-Invite
          * (seen with Sipphone.com, re-Invites when using
          * the SIP - POTS gateway [SIP Minutes] 
          */
         /* Port number */
         if (rtp_proxytable[i].remote_port != remote_port) {
            DEBUGC(DBCLASS_RTP,"RTP port number changed %i -> %i",
                   rtp_proxytable[i].remote_port, remote_port);
            rtp_proxytable[i].remote_port = remote_port;
         }
         /* IP address */
         if (memcmp(&rtp_proxytable[i].remote_ipaddr, &remote_ipaddr,
                    sizeof(remote_ipaddr))) {
            DEBUGC(DBCLASS_RTP,"RTP IP address changed to %s",
                   utils_inet_ntoa(remote_ipaddr));
            memcpy (&rtp_proxytable[i].remote_ipaddr, &remote_ipaddr,
                     sizeof(remote_ipaddr));
         }
         /* return the already known local port number */
         DEBUGC(DBCLASS_RTP,"RTP stream already active (remaddr=%s, "
                "remport=%i, lclport=%i, id=%s, #=%i)",
                utils_inet_ntoa(remote_ipaddr),
                rtp_proxytable[i].remote_port,
                rtp_proxytable[i].local_port,
                rtp_proxytable[i].callid_number,
                rtp_proxytable[i].media_stream_no);
	 *local_port=rtp_proxytable[i].local_port;
	 sts = STS_SUCCESS;
	 goto unlock_and_exit;
      }
   }


   /*
    * find first free slot in rtp_proxytable
    */
   freeidx=-1;
   for (j=0; j<RTPPROXY_SIZE; j++) {
      if (rtp_proxytable[j].rtp_rx_sock==0) {
         freeidx=j;
	 break;
      }
   }

   /* rtp_proxytable port pool full? */
   if (freeidx == -1) {
      ERROR("rtp_relay_start_fwd: rtp_proxytable is full!");
      sts = STS_FAILURE;
      goto unlock_and_exit;
   }

   /* TODO: randomize the port allocation - start at a random offset to
         search in the allowed port range (so some modulo stuff w/
	 random start offset 
	 - for i=x to (p1-p0)+x; p=p0+mod(x,p1-p0) */

   /* find a local port number to use and bind to it */
   sock=0;
   port=0;
   rtcp_sock=0;
   for (i=configuration.rtp_port_low; i<=configuration.rtp_port_high; i+=2) {
      for (j=0; j<RTPPROXY_SIZE; j++) {
         /* check if port already in use */
         if ((memcmp(&rtp_proxytable[j].local_ipaddr,
	             &local_ipaddr, sizeof(struct in_addr))== 0) &&
	     (rtp_proxytable[j].local_port == i) ) break;
      }

      /* port is available, try to allocate */
      if (j == RTPPROXY_SIZE) {
         port=i;
         sock=sockbind(local_ipaddr, port, 0);
         /* if success break, else try further on */
         if (sock) {
            rtcp_sock=sockbind(local_ipaddr, port+1, 0);
            if (rtcp_sock)
                break;
            else
                close(sock);
         }
      }
   } /* for i */

   DEBUGC(DBCLASS_RTP,"rtp_relay_start_fwd: addr=%s, port=%i, sock=%i rtcp_sock=%i "
          "freeidx=%i", utils_inet_ntoa(local_ipaddr), port, sock, rtcp_sock, freeidx);

   /* found an unused port? No -> RTP port pool fully allocated */
   if ((port == 0) || (sock == 0) || (rtcp_sock == 0)) {
      ERROR("rtp_relay_start_fwd: no RTP port available or bind() failed");
      sts = STS_FAILURE;
      goto unlock_and_exit;
   }

   /* write entry into rtp_proxytable slot (freeidx) */
   rtp_proxytable[freeidx].rtp_rx_sock=sock;
   rtp_proxytable[freeidx].rtcp_rx_sock=rtcp_sock;

   if (callid->number) {
      strcpy(rtp_proxytable[freeidx].callid_number, callid->number);
   } else {
      rtp_proxytable[freeidx].callid_number[0]='\0';
   }

   if (callid->host) {
      strcpy(rtp_proxytable[freeidx].callid_host, callid->host);
   } else {
      rtp_proxytable[freeidx].callid_host[0]='\0';
   }

   if (client_id) {
      strcpy(rtp_proxytable[freeidx].client_id, client_id);
   } else {
      rtp_proxytable[freeidx].client_id[0]='\0';
   }

   rtp_proxytable[freeidx].direction = rtp_direction;
   rtp_proxytable[freeidx].media_stream_no = media_stream_no;
   memcpy(&rtp_proxytable[freeidx].local_ipaddr,
          &local_ipaddr, sizeof(struct in_addr));
   rtp_proxytable[freeidx].local_port=port;
   memcpy(&rtp_proxytable[freeidx].remote_ipaddr,
          &remote_ipaddr, sizeof(struct in_addr));
   rtp_proxytable[freeidx].remote_port=remote_port;
   time(&rtp_proxytable[freeidx].timestamp);

   *local_port=port;

   /* call to firewall API */
   fwapi_start_rtp(rtp_proxytable[freeidx].direction,
                   rtp_proxytable[freeidx].local_ipaddr,
                   rtp_proxytable[freeidx].local_port,
                   rtp_proxytable[freeidx].remote_ipaddr,
                   rtp_proxytable[freeidx].remote_port);

   /* prepare FD set for next select operation */
   rtp_recreate_fdset();

   /* wakeup/signal rtp_proxythread from select() hibernation */
   if (!pthread_equal(rtpproxy_tid, pthread_self()))
      pthread_kill(rtpproxy_tid, SIGALRM);

unlock_and_exit:
   /* unlock mutex */
   pthread_mutex_unlock(&rtp_proxytable_mutex);
   #undef return

   return sts;
}


/*
 * stop a rtp stream on the proxy
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int rtp_relay_stop_fwd (int rtp_rx_sock, osip_call_id_t *callid,
                        int rtp_direction, int nolock) {
   int i, sts;
   int retsts=STS_SUCCESS;
   int got_match=0;
   osip_call_id_t cid;
 
   if (callid == NULL) {
      ERROR("rtp_relay_stop_fwd: callid is NULL!");
      return STS_FAILURE;
   }

   DEBUGC(DBCLASS_RTP,"rtp_relay_stop_fwd: stopping RTP proxy "
          "stream for: %s@%s (%s)",
          callid->number, callid->host,
          ((rtp_direction == DIR_INCOMING) ? "incoming" : "outgoing"));

   /*
    * lock mutex - only if not requested to skip the lock.
    * this is needed as we are also called from within
    * the RTP thread itself - and there we already own the lock.
    */
   #define return is_forbidden_in_this_code_section
   if (nolock == 0) {
      pthread_mutex_lock(&rtp_proxytable_mutex);
      /*
       * !! We now have a locked MUTEX! It is forbidden to return() from
       * !! here up to the end of this funtion where the MUTEX is
       * !! unlocked again.
       * !! Per design, a mutex is locked (for one purpose) at *exactly one*
       * !! place in the code and unlocked also at *exactly one* place.
       * !! this minimizes the risk of deadlocks.
       */
   }
   /* 
   * wakeup/signal rtp_proxythread from select() hibernation.
   * This must be done here before we close the socket, otherwise
   * we may get an select() error later from the proxy thread that
   * is still hibernating in select() now.
   */
   if (!pthread_equal(rtpproxy_tid, pthread_self()))
      pthread_kill(rtpproxy_tid, SIGALRM);

   /*
    * find the proper entry in rtp_proxytable
    * we need to loop the whole table, as there might be multiple
    * media strema active for the same callid (audio + video stream)
    */
   for (i=0; i<RTPPROXY_SIZE; i++) {
      cid.number = rtp_proxytable[i].callid_number;
      cid.host   = rtp_proxytable[i].callid_host;
      if (rtp_proxytable[i].rtp_rx_sock &&
         (compare_callid(callid, &cid) == STS_SUCCESS) &&
         (rtp_proxytable[i].direction == rtp_direction)) {
         if (rtp_rx_sock != 0) {
            if (rtp_proxytable[i].rtp_rx_sock != rtp_rx_sock) {
               continue;
            }
         }
         sts = close(rtp_proxytable[i].rtp_rx_sock);
	 DEBUGC(DBCLASS_RTP,"closed socket %i for RTP stream "
                "%s:%s == %s:%s  (idx=%i) sts=%i",
	        rtp_proxytable[i].rtp_rx_sock,
	        rtp_proxytable[i].callid_number,
	        rtp_proxytable[i].callid_host,
	        callid->number, callid->host, i, sts);
         if (sts < 0) {
            ERROR("Error in close(%i): %s nolock=%i %s:%s\n",
                  rtp_proxytable[i].rtp_rx_sock,
                  strerror(errno), nolock,
                  callid->number, callid->host);
         }
         if (rtp_proxytable[i].rtcp_rx_sock) {
            sts = close(rtp_proxytable[i].rtcp_rx_sock);
            DEBUGC(DBCLASS_RTP,"closed socket %i for RTP stream "
                    "%s:%s == %s:%s  (idx=%i) sts=%i",
	            rtp_proxytable[i].rtcp_rx_sock,
	            rtp_proxytable[i].callid_number,
	            rtp_proxytable[i].callid_host,
	            callid->number, callid->host, i, sts);
            if (sts < 0) {
                ERROR("Error in close(%i): %s nolock=%i %s:%s\n",
                    rtp_proxytable[i].rtcp_rx_sock,
                    strerror(errno), nolock,
                    callid->number, callid->host);
            }
         }
         /* call to firewall API */
         fwapi_stop_rtp(rtp_proxytable[i].direction,
                   rtp_proxytable[i].local_ipaddr,
                   rtp_proxytable[i].local_port,
                   rtp_proxytable[i].remote_ipaddr,
                   rtp_proxytable[i].remote_port);
         /* clean up */
         memset(&rtp_proxytable[i], 0, sizeof(rtp_proxytable[0]));
         got_match=1;
      }
 
   }

   /* did not find an active stream... */
   if (!got_match) {
      DEBUGC(DBCLASS_RTP,
             "rtp_relay_stop_fwd: can't find active stream for %s@%s (%s)",
             callid->number, callid->host,
             ((rtp_direction == DIR_INCOMING) ? "incoming RTP" : "outgoing RTP"));
      retsts = STS_FAILURE;
      goto unlock_and_exit;
   }


   /* prepare FD set for next select operation */
   rtp_recreate_fdset();
   

unlock_and_exit:
   /*
    * unlock mutex - only if not requested to skip the lock.
    * this is needed as we are also called from within
    * the RTP thread itself - and there we already own the lock.
    */
   if (nolock == 0) {
      pthread_mutex_unlock(&rtp_proxytable_mutex);
   }
   #undef return

   return retsts;
}


/*
 * some sockets have been newly created or removed -
 * recreate the FD set for next select operation
 *
 * RETURNS
 *	STS_SUCCESS on success (always)
 */
static int rtp_recreate_fdset(void) {
   int i;

   FD_ZERO(&master_fdset);
   master_fd_max=-1;
   for (i=0;i<RTPPROXY_SIZE;i++) {
      if (rtp_proxytable[i].rtp_rx_sock != 0) {
         FD_SET(rtp_proxytable[i].rtp_rx_sock, &master_fdset);
         if (rtp_proxytable[i].rtp_rx_sock > master_fd_max) {
	       master_fd_max=rtp_proxytable[i].rtp_rx_sock;
         }
      }
      if (rtp_proxytable[i].rtcp_rx_sock != 0) {
         FD_SET(rtp_proxytable[i].rtcp_rx_sock, &master_fdset);
         if (rtp_proxytable[i].rtcp_rx_sock > master_fd_max) {
	       master_fd_max=rtp_proxytable[i].rtcp_rx_sock;
         }
      }
   } /* for i */
   return STS_SUCCESS;
}


/*
 * kills the rtp_proxy thread
 *
 * RETURNS
 *	-
 */
void rtpproxy_kill( void ) {
   void *thread_status;
   osip_call_id_t cid;
   int i, sts;

   /* stop any active RTP stream */
   for (i=0;i<RTPPROXY_SIZE;i++) {
      if (rtp_proxytable[i].rtp_rx_sock != 0) {
         cid.number = rtp_proxytable[i].callid_number;
         cid.host   = rtp_proxytable[i].callid_host;
         sts = rtp_relay_stop_fwd(rtp_proxytable[i].rtp_rx_sock, &cid, rtp_proxytable[i].direction, 0);
      }
   }
   

   /* kill the thread */
   if (rtpproxy_tid) {
      pthread_cancel(rtpproxy_tid);
      pthread_kill(rtpproxy_tid, SIGALRM);
      pthread_join(rtpproxy_tid, &thread_status);
   }

   DEBUGC(DBCLASS_RTP,"killed RTP proxy thread");
   return;
}

