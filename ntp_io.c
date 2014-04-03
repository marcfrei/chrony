/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Timo Teras  2009
 * Copyright (C) Miroslav Lichvar  2009
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  This file deals with the IO aspects of reading and writing NTP packets
  */

#include "config.h"

#include "sysincl.h"

#include "ntp_io.h"
#include "ntp_core.h"
#include "ntp_sources.h"
#include "sched.h"
#include "local.h"
#include "logging.h"
#include "conf.h"
#include "util.h"

#define INVALID_SOCK_FD -1

union sockaddr_in46 {
  struct sockaddr_in in4;
#ifdef HAVE_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr u;
};

/* The server/peer and client sockets for IPv4 and IPv6 */
static int server_sock_fd4;
static int client_sock_fd4;
#ifdef HAVE_IPV6
static int server_sock_fd6;
static int client_sock_fd6;
#endif

/* Flag indicating we create a new connected client socket for each
   server instead of sharing client_sock_fd4 and client_sock_fd6 */
static int separate_client_sockets;

/* Flag indicating that we have been initialised */
static int initialised=0;

/* ================================================== */

/* Forward prototypes */
static void read_from_socket(void *anything);

/* ================================================== */

static void
do_size_checks(void)
{
  /* Assertions to check the sizes of certain data types
     and the positions of certain record fields */

  /* Check that certain invariants are true */
  assert(sizeof(NTP_int32) == 4);
  assert(sizeof(NTP_int64) == 8);

  /* Check offsets of all fields in the NTP packet format */
  assert(offsetof(NTP_Packet, lvm)             ==  0);
  assert(offsetof(NTP_Packet, stratum)         ==  1);
  assert(offsetof(NTP_Packet, poll)            ==  2);
  assert(offsetof(NTP_Packet, precision)       ==  3);
  assert(offsetof(NTP_Packet, root_delay)      ==  4);
  assert(offsetof(NTP_Packet, root_dispersion) ==  8);
  assert(offsetof(NTP_Packet, reference_id)    == 12);
  assert(offsetof(NTP_Packet, reference_ts)    == 16);
  assert(offsetof(NTP_Packet, originate_ts)    == 24);
  assert(offsetof(NTP_Packet, receive_ts)      == 32);
  assert(offsetof(NTP_Packet, transmit_ts)     == 40);

}

/* ================================================== */

static int
prepare_socket(int family, int port_number, int client_only)
{
  union sockaddr_in46 my_addr;
  socklen_t my_addr_len;
  int sock_fd;
  IPAddr bind_address;
  int on_off = 1;
  
  /* Open Internet domain UDP socket for NTP message transmissions */

#if 0
  sock_fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
#else
  sock_fd = socket(family, SOCK_DGRAM, 0);
#endif 

  if (sock_fd < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not open %s NTP socket : %s",
        family == AF_INET ? "IPv4" : "IPv6", strerror(errno));
    return INVALID_SOCK_FD;
  }

  /* Close on exec */
  UTI_FdSetCloexec(sock_fd);

  /* Make the socket capable of re-using an old address if binding to a specific port */
  if (port_number &&
      setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on_off, sizeof(on_off)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not set reuseaddr socket options");
    /* Don't quit - we might survive anyway */
  }
  
  /* Make the socket capable of sending broadcast pkts - needed for NTP broadcast mode */
  if (!client_only &&
      setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, (char *)&on_off, sizeof(on_off)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not set broadcast socket options");
    /* Don't quit - we might survive anyway */
  }

#ifdef SO_TIMESTAMP
  /* Enable receiving of timestamp control messages */
  if (setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMP, (char *)&on_off, sizeof(on_off)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not set timestamp socket options");
    /* Don't quit - we might survive anyway */
  }
#endif

  if (family == AF_INET) {
#ifdef IP_PKTINFO
    /* We want the local IP info on server sockets */
    if (!client_only &&
        setsockopt(sock_fd, IPPROTO_IP, IP_PKTINFO, (char *)&on_off, sizeof(on_off)) < 0) {
      LOG(LOGS_ERR, LOGF_NtpIO, "Could not request packet info using socket option");
      /* Don't quit - we might survive anyway */
    }
#endif
  }
#ifdef HAVE_IPV6
  else if (family == AF_INET6) {
#ifdef IPV6_V6ONLY
    /* Receive IPv6 packets only */
    if (setsockopt(sock_fd, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&on_off, sizeof(on_off)) < 0) {
      LOG(LOGS_ERR, LOGF_NtpIO, "Could not request IPV6_V6ONLY socket option");
    }
#endif

    if (!client_only) {
#ifdef IPV6_RECVPKTINFO
      if (setsockopt(sock_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, (char *)&on_off, sizeof(on_off)) < 0) {
        LOG(LOGS_ERR, LOGF_NtpIO, "Could not request IPv6 packet info socket option");
      }
#elif defined(IPV6_PKTINFO)
      if (setsockopt(sock_fd, IPPROTO_IPV6, IPV6_PKTINFO, (char *)&on_off, sizeof(on_off)) < 0) {
        LOG(LOGS_ERR, LOGF_NtpIO, "Could not request IPv6 packet info socket option");
      }
#endif
    }
  }
#endif

  /* Bind the port */
  memset(&my_addr, 0, sizeof (my_addr));

  switch (family) {
    case AF_INET:
      my_addr_len = sizeof (my_addr.in4);
      my_addr.in4.sin_family = family;
      my_addr.in4.sin_port = htons(port_number);

      if (!client_only)
        CNF_GetBindAddress(IPADDR_INET4, &bind_address);
      else
        CNF_GetBindAcquisitionAddress(IPADDR_INET4, &bind_address);

      if (bind_address.family == IPADDR_INET4)
        my_addr.in4.sin_addr.s_addr = htonl(bind_address.addr.in4);
      else
        my_addr.in4.sin_addr.s_addr = htonl(INADDR_ANY);
      break;
#ifdef HAVE_IPV6
    case AF_INET6:
      my_addr_len = sizeof (my_addr.in6);
      my_addr.in6.sin6_family = family;
      my_addr.in6.sin6_port = htons(port_number);

      if (!client_only)
        CNF_GetBindAddress(IPADDR_INET6, &bind_address);
      else
        CNF_GetBindAcquisitionAddress(IPADDR_INET6, &bind_address);

      if (bind_address.family == IPADDR_INET6)
        memcpy(my_addr.in6.sin6_addr.s6_addr, bind_address.addr.in6,
            sizeof (my_addr.in6.sin6_addr.s6_addr));
      else
        my_addr.in6.sin6_addr = in6addr_any;
      break;
#endif
    default:
      assert(0);
  }

#if 0
  LOG(LOGS_INFO, LOGF_NtpIO, "Initialising, socket fd=%d", sock_fd);
#endif

  if (bind(sock_fd, &my_addr.u, my_addr_len) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not bind %s NTP socket : %s",
        family == AF_INET ? "IPv4" : "IPv6", strerror(errno));
    close(sock_fd);
    return INVALID_SOCK_FD;
  }

  /* Register handler for read events on the socket */
  SCH_AddInputFileHandler(sock_fd, read_from_socket, (void *)(long)sock_fd);

#if 0
  if (fcntl(sock_fd, F_SETFL, O_NONBLOCK | O_NDELAY) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not make socket non-blocking");
  }

  if (ioctl(sock_fd, I_SETSIG, S_INPUT) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not enable signal");
  }
#endif
  return sock_fd;
}

/* ================================================== */

static int
connect_socket(int sock_fd, NTP_Remote_Address *remote_addr)
{
  union sockaddr_in46 addr;
  socklen_t addr_len;

  memset(&addr, 0, sizeof (addr));

  switch (remote_addr->ip_addr.family) {
    case IPADDR_INET4:
      addr_len = sizeof (addr.in4);
      addr.in4.sin_family = AF_INET;
      addr.in4.sin_addr.s_addr = htonl(remote_addr->ip_addr.addr.in4);
      addr.in4.sin_port = htons(remote_addr->port);
      break;
#ifdef HAVE_IPV6
    case IPADDR_INET6:
      addr_len = sizeof (addr.in6);
      addr.in6.sin6_family = AF_INET6;
      memcpy(addr.in6.sin6_addr.s6_addr, remote_addr->ip_addr.addr.in6,
             sizeof (addr.in6.sin6_addr.s6_addr));
      addr.in6.sin6_port = htons(remote_addr->port);
      break;
#endif
    default:
      assert(0);
  }

  if (connect(sock_fd, &addr.u, addr_len) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not connect NTP socket to %s:%d : %s",
        UTI_IPToString(&remote_addr->ip_addr), remote_addr->port,
        strerror(errno));
    return 0;
  }

  return 1;
}

/* ================================================== */

static void
close_socket(int sock_fd)
{
  if (sock_fd == INVALID_SOCK_FD)
    return;

  SCH_RemoveInputFileHandler(sock_fd);
  close(sock_fd);
}

/* ================================================== */

void
NIO_Initialise(int family)
{
  int server_port, client_port;

  assert(!initialised);
  initialised = 1;

  do_size_checks();

  server_port = CNF_GetNTPPort();
  client_port = CNF_GetAcquisitionPort();

  /* Use separate connected sockets if client port is not set */
  separate_client_sockets = client_port == 0;

  server_sock_fd4 = INVALID_SOCK_FD;
  client_sock_fd4 = INVALID_SOCK_FD;
#ifdef HAVE_IPV6
  server_sock_fd6 = INVALID_SOCK_FD;
  client_sock_fd6 = INVALID_SOCK_FD;
#endif

  if (family == IPADDR_UNSPEC || family == IPADDR_INET4) {
    if (server_port)
      server_sock_fd4 = prepare_socket(AF_INET, server_port, 0);
    if (!separate_client_sockets) {
      if (client_port != server_port || !server_port)
        client_sock_fd4 = prepare_socket(AF_INET, client_port, 1);
      else
        client_sock_fd4 = server_sock_fd4;
    }
  }
#ifdef HAVE_IPV6
  if (family == IPADDR_UNSPEC || family == IPADDR_INET6) {
    if (server_port)
      server_sock_fd6 = prepare_socket(AF_INET6, server_port, 0);
    if (!separate_client_sockets) {
      if (client_port != server_port || !server_port)
        client_sock_fd6 = prepare_socket(AF_INET6, client_port, 1);
      else
        client_sock_fd6 = server_sock_fd6;
    }
  }
#endif

  if ((server_port && server_sock_fd4 == INVALID_SOCK_FD
#ifdef HAVE_IPV6
       && server_sock_fd6 == INVALID_SOCK_FD
#endif
      ) || (!separate_client_sockets && client_sock_fd4 == INVALID_SOCK_FD
#ifdef HAVE_IPV6
       && client_sock_fd6 == INVALID_SOCK_FD
#endif
      )) {
    LOG_FATAL(LOGF_NtpIO, "Could not open NTP sockets");
  }
}

/* ================================================== */

void
NIO_Finalise(void)
{
  if (server_sock_fd4 != client_sock_fd4)
    close_socket(client_sock_fd4);
  close_socket(server_sock_fd4);
  server_sock_fd4 = client_sock_fd4 = INVALID_SOCK_FD;
#ifdef HAVE_IPV6
  if (server_sock_fd6 != client_sock_fd6)
    close_socket(client_sock_fd6);
  close_socket(server_sock_fd6);
  server_sock_fd6 = client_sock_fd6 = INVALID_SOCK_FD;
#endif
  initialised = 0;
}

/* ================================================== */

int
NIO_GetClientSocket(NTP_Remote_Address *remote_addr)
{
  if (separate_client_sockets) {
    int sock_fd;

    switch (remote_addr->ip_addr.family) {
      case IPADDR_INET4:
        sock_fd = prepare_socket(AF_INET, 0, 1);
        break;
#ifdef HAVE_IPV6
      case IPADDR_INET6:
        sock_fd = prepare_socket(AF_INET6, 0, 1);
        break;
#endif
      default:
        sock_fd = INVALID_SOCK_FD;
    }

    if (sock_fd == INVALID_SOCK_FD)
      return INVALID_SOCK_FD;

    if (!connect_socket(sock_fd, remote_addr)) {
      close_socket(sock_fd);
      return INVALID_SOCK_FD;
    }

    return sock_fd;
  } else {
    switch (remote_addr->ip_addr.family) {
      case IPADDR_INET4:
        return client_sock_fd4;
#ifdef HAVE_IPV6
      case IPADDR_INET6:
        return client_sock_fd6;
#endif
      default:
        return INVALID_SOCK_FD;
    }
  }
}

/* ================================================== */

int
NIO_GetServerSocket(NTP_Remote_Address *remote_addr)
{
  switch (remote_addr->ip_addr.family) {
    case IPADDR_INET4:
      return server_sock_fd4;
#ifdef HAVE_IPV6
    case IPADDR_INET6:
      return server_sock_fd6;
#endif
    default:
      return INVALID_SOCK_FD;
  }
}

/* ================================================== */

void
NIO_CloseClientSocket(int sock_fd)
{
  if (separate_client_sockets)
    close_socket(sock_fd);
}

/* ================================================== */

int
NIO_IsServerSocket(int sock_fd)
{
  return sock_fd != INVALID_SOCK_FD &&
    (sock_fd == server_sock_fd4
#ifdef HAVE_IPV6
     || sock_fd == server_sock_fd6
#endif
    );
}

/* ================================================== */

static void
read_from_socket(void *anything)
{
  /* This should only be called when there is something
     to read, otherwise it will block. */

  int status, sock_fd;
  ReceiveBuffer message;
  union sockaddr_in46 where_from;
  unsigned int flags = 0;
  struct timeval now, now_raw;
  double now_err;
  NTP_Remote_Address remote_addr;
  NTP_Local_Address local_addr;
  char cmsgbuf[256];
  struct msghdr msg;
  struct iovec iov;
  struct cmsghdr *cmsg;

  assert(initialised);

  SCH_GetLastEventTime(&now, &now_err, &now_raw);

  iov.iov_base = message.arbitrary;
  iov.iov_len = sizeof(message);
  msg.msg_name = &where_from;
  msg.msg_namelen = sizeof(where_from);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = (void *) cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  msg.msg_flags = 0;

  sock_fd = (long)anything;
  status = recvmsg(sock_fd, &msg, flags);

  /* Don't bother checking if read failed or why if it did.  More
     likely than not, it will be connection refused, resulting from a
     previous sendto() directing a datagram at a port that is not
     listening (which appears to generate an ICMP response, and on
     some architectures e.g. Linux this is translated into an error
     reponse on a subsequent recvfrom). */

  if (status > 0) {
    switch (where_from.u.sa_family) {
      case AF_INET:
        remote_addr.ip_addr.family = IPADDR_INET4;
        remote_addr.ip_addr.addr.in4 = ntohl(where_from.in4.sin_addr.s_addr);
        remote_addr.port = ntohs(where_from.in4.sin_port);
        break;
#ifdef HAVE_IPV6
      case AF_INET6:
        remote_addr.ip_addr.family = IPADDR_INET6;
        memcpy(&remote_addr.ip_addr.addr.in6, where_from.in6.sin6_addr.s6_addr,
            sizeof (remote_addr.ip_addr.addr.in6));
        remote_addr.port = ntohs(where_from.in6.sin6_port);
        break;
#endif
      default:
        assert(0);
    }

    local_addr.ip_addr.family = IPADDR_UNSPEC;
    local_addr.sock_fd = sock_fd;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
#ifdef IP_PKTINFO
      if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
        struct in_pktinfo ipi;

        memcpy(&ipi, CMSG_DATA(cmsg), sizeof(ipi));
        local_addr.ip_addr.addr.in4 = ntohl(ipi.ipi_spec_dst.s_addr);
        local_addr.ip_addr.family = IPADDR_INET4;
      }
#endif

#if defined(IPV6_PKTINFO) && defined(HAVE_IN6_PKTINFO)
      if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
        struct in6_pktinfo ipi;

        memcpy(&ipi, CMSG_DATA(cmsg), sizeof(ipi));
        memcpy(&local_addr.ip_addr.addr.in6, &ipi.ipi6_addr.s6_addr,
            sizeof (local_addr.ip_addr.addr.in6));
        local_addr.ip_addr.family = IPADDR_INET6;
      }
#endif

#ifdef SO_TIMESTAMP
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMP) {
        struct timeval tv;

        memcpy(&tv, CMSG_DATA(cmsg), sizeof(tv));

        /* This should be more accurate than LCL_CookTime(&now_raw,...) */
        UTI_AddDiffToTimeval(&now, &now_raw, &tv, &now);
      }
#endif
    }

    if (status >= NTP_NORMAL_PACKET_SIZE && status <= sizeof(NTP_Packet)) {

      NSR_ProcessReceive((NTP_Packet *) &message.ntp_pkt, &now, now_err,
                         &remote_addr, &local_addr, status);

    } else {

      /* Just ignore the packet if it's not of a recognized length */

    }
  }
}

/* ================================================== */
/* Send a packet to given address */

static void
send_packet(void *packet, int packetlen, NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr)
{
  union sockaddr_in46 remote;
  struct msghdr msg;
  struct iovec iov;
  char cmsgbuf[256];
  int cmsglen;
  socklen_t addrlen;

  assert(initialised);

  switch (remote_addr->ip_addr.family) {
    case IPADDR_INET4:
      memset(&remote.in4, 0, sizeof (remote.in4));
      addrlen = sizeof (remote.in4);
      remote.in4.sin_family = AF_INET;
      remote.in4.sin_port = htons(remote_addr->port);
      remote.in4.sin_addr.s_addr = htonl(remote_addr->ip_addr.addr.in4);
      break;
#ifdef HAVE_IPV6
    case IPADDR_INET6:
      memset(&remote.in6, 0, sizeof (remote.in6));
      addrlen = sizeof (remote.in6);
      remote.in6.sin6_family = AF_INET6;
      remote.in6.sin6_port = htons(remote_addr->port);
      memcpy(&remote.in6.sin6_addr.s6_addr, &remote_addr->ip_addr.addr.in6,
          sizeof (remote.in6.sin6_addr.s6_addr));
      break;
#endif
    default:
      return;
  }

  if (local_addr->sock_fd == INVALID_SOCK_FD) {
    DEBUG_LOG(LOGF_NtpIO, "No socket to send to %s:%d",
              UTI_IPToString(&remote_addr->ip_addr), remote_addr->port);
    return;
  }

  iov.iov_base = packet;
  iov.iov_len = packetlen;
  msg.msg_name = &remote.u;
  msg.msg_namelen = addrlen;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  msg.msg_flags = 0;
  cmsglen = 0;

#ifdef IP_PKTINFO
  if (local_addr->ip_addr.family == IPADDR_INET4) {
    struct cmsghdr *cmsg;
    struct in_pktinfo *ipi;

    cmsg = CMSG_FIRSTHDR(&msg);
    memset(cmsg, 0, CMSG_SPACE(sizeof(struct in_pktinfo)));
    cmsglen += CMSG_SPACE(sizeof(struct in_pktinfo));

    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));

    ipi = (struct in_pktinfo *) CMSG_DATA(cmsg);
    ipi->ipi_spec_dst.s_addr = htonl(local_addr->ip_addr.addr.in4);
  }
#endif

#if defined(IPV6_PKTINFO) && defined(HAVE_IN6_PKTINFO)
  if (local_addr->ip_addr.family == IPADDR_INET6) {
    struct cmsghdr *cmsg;
    struct in6_pktinfo *ipi;

    cmsg = CMSG_FIRSTHDR(&msg);
    memset(cmsg, 0, CMSG_SPACE(sizeof(struct in6_pktinfo)));
    cmsglen += CMSG_SPACE(sizeof(struct in6_pktinfo));

    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type = IPV6_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

    ipi = (struct in6_pktinfo *) CMSG_DATA(cmsg);
    memcpy(&ipi->ipi6_addr.s6_addr, &local_addr->ip_addr.addr.in6,
        sizeof(ipi->ipi6_addr.s6_addr));
  }
#endif

#if 0
    LOG(LOGS_INFO, LOGF_NtpIO, "sending to %s:%d from %s",
        UTI_IPToString(&remote_addr->ip_addr), remote_addr->port, UTI_IPToString(&remote_addr->local_ip_addr));
#endif

  msg.msg_controllen = cmsglen;
  /* This is apparently required on some systems */
  if (!cmsglen)
    msg.msg_control = NULL;

  if (sendmsg(local_addr->sock_fd, &msg, 0) < 0 &&
#ifdef ENETUNREACH
      errno != ENETUNREACH &&
#endif
#ifdef ENETDOWN
      errno != ENETDOWN &&
#endif
      !LOG_RateLimited()) {
    LOG(LOGS_WARN, LOGF_NtpIO, "Could not send to %s:%d : %s",
        UTI_IPToString(&remote_addr->ip_addr), remote_addr->port, strerror(errno));
  }
}

/* ================================================== */
/* Send an unauthenticated packet to a given address */

void
NIO_SendNormalPacket(NTP_Packet *packet, NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr)
{
  send_packet((void *) packet, NTP_NORMAL_PACKET_SIZE, remote_addr, local_addr);
}

/* ================================================== */
/* Send an authenticated packet to a given address */

void
NIO_SendAuthenticatedPacket(NTP_Packet *packet, NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr, int auth_len)
{
  send_packet((void *) packet, NTP_NORMAL_PACKET_SIZE + auth_len, remote_addr, local_addr);
}

/* ================================================== */

/* We ought to use getservbyname, but I can't really see this changing */
#define ECHO_PORT 7

void
NIO_SendEcho(NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr)
{
  unsigned long magic_message = 0xbe7ab1e7UL;
  NTP_Remote_Address addr;

  addr = *remote_addr;
  addr.port = ECHO_PORT;

  send_packet((void *) &magic_message, sizeof(unsigned long), &addr, local_addr);
}
