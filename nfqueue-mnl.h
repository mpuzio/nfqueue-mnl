/*
 *  nfqueue-mnl.h - Interface to netfilter (nfqueue and conntrack) using libmnl
 *  Copyright (c) 2019 Maciej Puzio
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program - see the file COPYING.
 *
 *  This file includes code from netfilter libnml example files,
 *  which had been placed in public domain by their author.
 */


#ifndef NFQUEUE_MNL_H
#define NFQUEUE_MNL_H


#include <stdio.h>        //fprintf
#include <stdlib.h>       //malloc, free
#include <stdbool.h>      //bool type
#include <sys/select.h>   //pselect
#include <errno.h>        //errno, EINTR, ...
#include <netinet/in.h>   //in_addr, in6_addr, ...
#include <time.h>         //timespec, clock_gettime
#include <string.h>       //memset, strerror
#include <libmnl/libmnl.h>

//The following includes are needed only for constants
#include <linux/netfilter.h>                      //NF_ACCEPT and NF_DROP
#include <linux/netfilter/nfnetlink_queue.h>      //NFQA_* and NFQNL_*
#include <linux/netfilter/nfnetlink_conntrack.h>  //CTA_*
#include <syslog.h>                               //LOG_*

#ifdef HAVE_CONFIG_H
    #include "config.h"  //created by configure script
#endif


/*
Kernel compatibility notes

Kernel 3.8 or later is required, as this code does not perform PF_BIND anf PF_UNBIND.
A bug in kernel causes it not to pass the timestamp attribute (always zero), depending on kernel version and NIC driver.
This is worked around by using our own timestamps.
*/


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Enums and object definitions


//ip_version
enum
{
    IPV4 = 4,
    IPV6 = 6
};

//I/O result
enum
{
    IO_ERROR    = -1,
    IO_NOTREADY =  0,  //timeout or no more data
    IO_READY    =  1   //data ready
};


/*
Reasons for attributes packed and may_alias:
Attribute 'packed' is needed here to force the compiler to emit unaligned-access opcodes when accessing
objects of ip_address_t type. Such unaligned accesses happen because IP addresses are passed as Netlink
payloads that are not aligned to 16-byte boundaries. On x86_64 this means emiting movdqu and movups
instead of movdqa and movaps.
Attribute 'may_alias" is used to prevent the compiler using strict aliasing optimizations for accesses
to ip_address_t. This is desirable because cross-type aliasing is what ip_address_t is designed for.
*/

typedef union __attribute__((packed, may_alias))
{
    __uint128_t ip;
    uint32_t    ip4;
    struct in_addr  in4;
    struct in6_addr in6;
    struct
    {
        uint64_t hi;
        uint64_t lo;
    };
} ip_address_t;


struct ip_tuple
{
    int           ip_version;
    ip_address_t  src;
    ip_address_t  dst;
    uint16_t      src_port;
    uint16_t      dst_port;
};


//Netlink packet object
//This structure collects packet information passed by netfilter.

struct nf_packet
{
    int               queue_num;
    uint32_t          packet_id;      // from NFQA_PACKET_HDR
    uint16_t          hw_protocol;    // from NFQA_PACKET_HDR; see https://en.wikipedia.org/wiki/EtherType
    size_t            payload_len;    // NFQA_PAYLOAD length
    void*             payload;        // NFQA_PAYLOAD
    bool              has_timestamp;  // true if netfilter provided a timestamp
    struct timeval    timestamp;      // NFQA_TIMESTAMP (if has_timestamp) or zero
    struct timespec   wall_time;      // clock_gettime(CLOCK_REALTIME)
    struct timespec   mono_time;      // clock_gettime(CLOCK_MONOTONIC_RAW)
    bool              has_conntrack;  // true if netfilter provided conntrack info (if not, the following fields are zero)
    bool              has_connmark;   // true if CTA_MARK is present
    uint32_t          conn_id;        // NFQA_CT > CTA_ID; see https://www.spinics.net/lists/netdev/msg443125.html and https://patchwork.kernel.org/patch/9820809/
    uint32_t          conn_mark;      // NFQA_CT > CTA_MARK
    uint32_t          conn_state;     // NFQA_CT_INFO (IP_CT_NEW/ESTABLISHED/RELATED/...)
    uint32_t          conn_status;    // NDQA_CT > CTA_STATUS (IPS_SEEN_REPLY/CONFIRMED/...)
    struct ip_tuple   orig;           // NFQA_CT > CTA_TUPLE_ORIG
    struct ip_tuple   reply;          // NFQA_CT > CTA_TUPLE_REPLY
};

/* addr_tuple fields
    int               ip_version;     // IPV4 or IPV6
    ip_address_t      src;            // CTA_TUPLE_IP > CTA_IP_V?_SRC
    ip_address_t      dst;            // CTA_TUPLE_IP > CTA_IP_V?_DST
    uint16_t          src_port;       // CTA_TUPLE_PROTO > CTA_PROTO_SRC_PORT
    uint16_t          dst_port;       // CTA_TUPLE_PROTO > CTA_PROTO_DST_PORT
*/


struct nf_queue
{
    int                queue_num;
    struct mnl_socket* nl_socket;
};

struct nf_buffer
{
    void*            data;
    struct nlmsghdr* nlh;
    int              len;   //used part of data buffer
};


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Error handling and logging


#ifndef LOG
    #define LOG(priority, fmt, ...)  fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#endif

#ifndef DIE
    #define DIE()  exit(EXIT_FAILURE)
#endif


#define LOG_ONCE(priority, fmt, ...) \
    do { static bool done = false; if (!done) { LOG((priority), fmt, ##__VA_ARGS__); done = true; } } while(0)

#define LOG_SYSERR(fmt, ...) \
    LOG(LOG_ERR, fmt ": %s", ##__VA_ARGS__, strerror(errno))

#define DEBUG(fmt, ...) \
    LOG(LOG_DEBUG, fmt, ##__VA_ARGS__)

#define ASSERT(condition) \
    do { if (!(condition)) { LOG(LOG_CRIT, "Assert failed: %s [%s:%s:%d]", #condition, __func__, __FILE__, __LINE__); DIE(); } } while(0)


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MNL integer attr functions redefinitions

/*
Reason:
libmnl contains functions mnl_attr_get_u[16|32] that may return unaligned 16-bit and 32-bit integers.
Below we redefine these functions with alignment-safe implementations.
mnl_attr_get_u8 is immune from aligment issues, by virtue of uint8_t requiring alignment 1.
mnl_attr_get_u64 is implemented by libmnl in an alignment-safe manner, similar to one that we use below.
*/

static uint16_t fixed_attr_get_u16(const struct nlattr* attr)
{
    uint16_t tmp;
    memcpy(&tmp, mnl_attr_get_payload(attr), sizeof(tmp));
    return tmp;
}

static uint32_t fixed_attr_get_u32(const struct nlattr* attr)
{
    uint32_t tmp;
    memcpy(&tmp, mnl_attr_get_payload(attr), sizeof(tmp));
    return tmp;
}

#define mnl_attr_get_u16(attr) fixed_attr_get_u16(attr)
#define mnl_attr_get_u32(attr) fixed_attr_get_u32(attr)


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sending Netlink commands


#define SEND_BUF_LEN  MNL_SOCKET_BUFFER_SIZE
#define RECV_BUF_LEN (MNL_SOCKET_BUFFER_SIZE + 0xFFFF)
#define BUF_TOO_SHORT -42

//helper
//Return NULL on failure
static struct nlmsghdr* nfqueue_put_header(int queue_num, int msg_type)
{
    if (MNL_ALIGN(sizeof(struct nlmsghdr)) > SEND_BUF_LEN)  //check buffer size
        return NULL;
    void* buf = malloc(SEND_BUF_LEN);
    ASSERT(buf != NULL);
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    ASSERT(nlh == buf);
	nlh->nlmsg_type	= (NFNL_SUBSYS_QUEUE << 8) | msg_type;
	nlh->nlmsg_flags = NLM_F_REQUEST;

    if (nlh->nlmsg_len + MNL_ALIGN(sizeof(struct nfgenmsg)) > SEND_BUF_LEN)  //check buffer size
    {
        free(buf);
        return NULL;
    }
	struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfg->nfgen_family = AF_UNSPEC;
	nfg->version = NFNETLINK_V0;
	nfg->res_id = htons(queue_num);

    return nlh;
}


//Return <0 on failure
static int nfqueue_send_command(struct mnl_socket* nl, int queue_num, int msg_type, int attr_type, void* data, size_t data_size)
{
    int ret = BUF_TOO_SHORT;
    struct nlmsghdr* nlh;
    if ((nlh = nfqueue_put_header(queue_num, msg_type)) == NULL)
        return ret;
    if (!mnl_attr_put_check(nlh, SEND_BUF_LEN, attr_type, data_size, data))
        goto end;
    ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);

end:
    free(nlh);
    if (ret == BUF_TOO_SHORT)
        errno = ENOBUFS;
    return ret;
}


/*
Note about pf parameter of NFQNL_CFG_CMD_BIND command
This command can be executed only once, so we cannot call it twice with AF_INET and then AF_INET6.
It appears that pf parameter is ignored, and bind is for all AF types.
*/

//Return <0 on failure
static int nfqueue_bind(struct mnl_socket* nl, int queue_num)
{
	struct nfqnl_msg_config_cmd cmd =
	{
		.command = NFQNL_CFG_CMD_BIND,
		.pf = 0,  //this parameter is ignored
	};
    return nfqueue_send_command(nl, queue_num, NFQNL_MSG_CONFIG, NFQA_CFG_CMD, &cmd, sizeof(cmd));
}


//Return <0 on failure
//If maxlen or flags are zero, use defaults
static int nfqueue_set_params(struct mnl_socket* nl, int queue_num, uint8_t mode, int range, uint32_t maxlen, uint32_t flags)
{
    struct nfqnl_msg_config_params params =
    {
		.copy_range = htonl(range),
		.copy_mode = mode,
	};

    int ret = BUF_TOO_SHORT;
    struct nlmsghdr* nlh;
    if ((nlh = nfqueue_put_header(queue_num, NFQNL_MSG_CONFIG)) == NULL)
        return ret;
    if (!mnl_attr_put_check(nlh, SEND_BUF_LEN, NFQA_CFG_PARAMS, sizeof(params), &params))
        goto end;

    if (maxlen > 0)
    {
        if (!mnl_attr_put_u32_check(nlh, SEND_BUF_LEN, NFQA_CFG_QUEUE_MAXLEN, htonl(maxlen)))
            goto end;
    }

    if (flags)
    {
        if (!mnl_attr_put_u32_check(nlh, SEND_BUF_LEN, NFQA_CFG_FLAGS, htonl(flags)))
            goto end;
        if (!mnl_attr_put_u32_check(nlh, SEND_BUF_LEN, NFQA_CFG_MASK,  htonl(flags)))
            goto end;
    }

	ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);

end:
    free(nlh);
    if (ret == BUF_TOO_SHORT)
        errno = ENOBUFS;
    return ret;
}


//Version that does not set a connmark
static int nfqueue_send_verdict(struct mnl_socket* nl, int queue_num, uint32_t packet_id, int verdict)
{
	struct nfqnl_msg_verdict_hdr vh =
	{
		.verdict = htonl(verdict),
		.id = htonl(packet_id),
	};
    return nfqueue_send_command(nl, queue_num, NFQNL_MSG_VERDICT, NFQA_VERDICT_HDR, &vh, sizeof(vh));
}


//Return <0 on failure
//Connmark is set if it is >= 0
//connmark is uint64_t to allow full 32-bit unsigned integer and also -1
static int nfqueue_send_verdict_connmark(struct mnl_socket* nl, int queue_num, uint32_t packet_id, int verdict, int64_t connmark)
{
	struct nfqnl_msg_verdict_hdr vh =
	{
		.verdict = htonl(verdict),
		.id = htonl(packet_id),
	};

    int ret = BUF_TOO_SHORT;
    struct nlmsghdr* nlh;
    if ((nlh = nfqueue_put_header(queue_num, NFQNL_MSG_VERDICT)) == NULL)
        return ret;
    if (!mnl_attr_put_check(nlh, SEND_BUF_LEN, NFQA_VERDICT_HDR, sizeof(vh), &vh))
        goto end;

    //Connmark
    if (connmark >= 0)
    {
        struct nlattr* nest;
        if ((nest = mnl_attr_nest_start_check(nlh, SEND_BUF_LEN, NFQA_CT)) == NULL)
            goto end;
        if (!mnl_attr_put_u32_check(nlh, SEND_BUF_LEN, CTA_MARK, htonl((uint32_t)connmark)))
            goto end;
        mnl_attr_nest_end(nlh, nest);
    }

	ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);

end:
    free(nlh);
    if (ret == BUF_TOO_SHORT)
        errno = ENOBUFS;
    return ret;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// NFQA and CTA attribute callbacks


//Code from libnetfilter_queue/nlmsg.c
//Source: https://git.netfilter.org/libnetfilter_queue/tree/src/nlmsg.c
//This is to avoid linking libnfnetlink and libnetfilter_queue in addition to libmnl
//Return MNL_CB_ERROR on failure
static int parse_nfqa_attr_cb(const struct nlattr* attr, void* data)
{
	const struct nlattr** tb = data;
	int type = mnl_attr_get_type(attr);

	//skip unsupported attribute in user-space
	if (mnl_attr_type_valid(attr, NFQA_MAX) < 0)
		return MNL_CB_OK;

	switch (type)
    {
        case NFQA_MARK:
        case NFQA_IFINDEX_INDEV:
        case NFQA_IFINDEX_OUTDEV:
        case NFQA_IFINDEX_PHYSINDEV:
        case NFQA_IFINDEX_PHYSOUTDEV:
        case NFQA_CAP_LEN:
        case NFQA_SKB_INFO:
        case NFQA_SECCTX:
        case NFQA_UID:
        case NFQA_GID:
        case NFQA_CT_INFO:
            if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
                return MNL_CB_ERROR;
            break;
        case NFQA_TIMESTAMP:
            if (mnl_attr_validate2(attr, MNL_TYPE_UNSPEC, sizeof(struct nfqnl_msg_packet_timestamp)) < 0)
                return MNL_CB_ERROR;
            break;
        case NFQA_HWADDR:
            if (mnl_attr_validate2(attr, MNL_TYPE_UNSPEC, sizeof(struct nfqnl_msg_packet_hw)) < 0)
                return MNL_CB_ERROR;
            break;
        case NFQA_PACKET_HDR:
            if (mnl_attr_validate2(attr, MNL_TYPE_UNSPEC, sizeof(struct nfqnl_msg_packet_hdr)) < 0)
                return MNL_CB_ERROR;
            break;
        case NFQA_PAYLOAD:
        case NFQA_CT:
        case NFQA_EXP:
            break;
	}

	tb[type] = attr;
	return MNL_CB_OK;
}


//Return MNL_CB_ERROR on failure
static int parse_cta_attr_cb(const struct nlattr* attr, void* data)
{
	const struct nlattr** tb = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, CTA_MAX) < 0)
		return MNL_CB_OK;

	switch(type)
    {
        case CTA_TUPLE_ORIG:
        case CTA_TUPLE_REPLY:
        case CTA_COUNTERS_ORIG:
        case CTA_COUNTERS_REPLY:
            if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
                return MNL_CB_ERROR;
            break;
        case CTA_STATUS:
        case CTA_TIMEOUT:
        case CTA_MARK:
        case CTA_SECMARK:
        case CTA_ID:
            if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
                return MNL_CB_ERROR;
            break;
	}

	tb[type] = attr;
	return MNL_CB_OK;
}


//Return MNL_CB_ERROR on failure
static int parse_tuple_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, CTA_TUPLE_MAX) < 0)
		return MNL_CB_OK;

	switch(type)
    {
        case CTA_TUPLE_IP:
            if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
                return MNL_CB_ERROR;
            break;
        case CTA_TUPLE_PROTO:
            if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
                return MNL_CB_ERROR;
            break;
	}

	tb[type] = attr;
	return MNL_CB_OK;
}


//Return MNL_CB_ERROR on failure
static int parse_ip_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, CTA_IP_MAX) < 0)
		return MNL_CB_OK;

	switch(type)
    {
        case CTA_IP_V4_SRC:
        case CTA_IP_V4_DST:
            if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
                return MNL_CB_ERROR;
            break;
        case CTA_IP_V6_SRC:
        case CTA_IP_V6_DST:
            if (mnl_attr_validate2(attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) < 0)
                return MNL_CB_ERROR;
            break;
	}

	tb[type] = attr;
	return MNL_CB_OK;
}


//Return MNL_CB_ERROR on failure
static int parse_proto_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, CTA_PROTO_MAX) < 0)
		return MNL_CB_OK;

	switch(type)
    {
        case CTA_PROTO_NUM:
        case CTA_PROTO_ICMP_TYPE:
        case CTA_PROTO_ICMP_CODE:
            if (mnl_attr_validate(attr, MNL_TYPE_U8) < 0)
                return MNL_CB_ERROR;
            break;
        case CTA_PROTO_SRC_PORT:
        case CTA_PROTO_DST_PORT:
        case CTA_PROTO_ICMP_ID:
            if (mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
                return MNL_CB_ERROR;
            break;
	}

	tb[type] = attr;
	return MNL_CB_OK;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parse netfilter data into nf_packet


//helper
static bool inline read_ip_addr(struct nlattr* attr, int af, ip_address_t* ip_num, int* ip_version)
{
    if (attr)
    {
        //Note: addr may be unaligned, so we use memcpy to copy it to an integer
        void* addr = mnl_attr_get_payload(attr);
        if (addr == NULL)
            return false;
        int ip_ver = 0;
        if (af == AF_INET)
        {
            ip_num->ip = 0;  //clear unused bits
            memcpy(&ip_num->ip4, addr, sizeof(uint32_t));
            ip_ver = IPV4;
        }
        else if (af == AF_INET6)
        {
            memcpy(&ip_num->ip, addr, sizeof(__uint128_t));
            ip_ver = IPV6;
        }
        if (*ip_version == 0)  //not set yet
            *ip_version = ip_ver;
        else if (*ip_version != ip_ver)  //mismatch
        {
            LOG(LOG_ERR, "Packet with mismatched IP versions");  //can that happen at all?
            return false;
        }
    }
    return true;
}


//helper
//Return false on failure
static bool read_addr_tuple(struct nlattr* attr, struct ip_tuple* tuple)
{
    struct nlattr* attr2[CTA_TUPLE_MAX+1] = {0};
    if (mnl_attr_parse_nested(attr, parse_tuple_cb, attr2) < 0)
        return false;
    if (attr2[CTA_TUPLE_IP])
    {
        struct nlattr* attr3[CTA_IP_MAX+1] = {0};
        if (mnl_attr_parse_nested(attr2[CTA_TUPLE_IP], parse_ip_cb, attr3) < 0)
            return false;
        bool success = true;
        success &= read_ip_addr(attr3[CTA_IP_V4_SRC], AF_INET,  &tuple->src, &tuple->ip_version);
        success &= read_ip_addr(attr3[CTA_IP_V4_DST], AF_INET,  &tuple->dst, &tuple->ip_version);
        success &= read_ip_addr(attr3[CTA_IP_V6_SRC], AF_INET6, &tuple->src, &tuple->ip_version);
        success &= read_ip_addr(attr3[CTA_IP_V6_DST], AF_INET6, &tuple->dst, &tuple->ip_version);
        if (!success)
            return false;
    }
    if (attr2[CTA_TUPLE_PROTO])
    {
        struct nlattr* attr3[CTA_PROTO_MAX+1] = {0};
        if (mnl_attr_parse_nested(attr2[CTA_TUPLE_PROTO], parse_proto_cb, attr3) < 0)
            return false;
        if (attr3[CTA_PROTO_SRC_PORT])
            tuple->src_port = ntohs(mnl_attr_get_u16(attr3[CTA_PROTO_SRC_PORT]));
        if (attr3[CTA_PROTO_DST_PORT])
            tuple->dst_port = ntohs(mnl_attr_get_u16(attr3[CTA_PROTO_DST_PORT]));
    }

    return true;
}


// The functions below log errors


//Return false on failure
static bool nfqueue_parse(const struct nlmsghdr* nlh, struct nf_packet* packet)
{
    memset(packet, 0, sizeof(*packet));
    clock_gettime(CLOCK_MONOTONIC_RAW, &packet->mono_time);
    clock_gettime(CLOCK_REALTIME, &packet->wall_time);

    struct nlattr* attr[NFQA_MAX+1] = {0};

	if (mnl_attr_parse(nlh, sizeof(struct nfgenmsg), parse_nfqa_attr_cb, attr) < 0)
    {
        LOG_SYSERR("mnl_attr_parse");
        return false;
    }

	if (attr[NFQA_PACKET_HDR] == NULL)
    {
        LOG(LOG_ERR, "Packet metaheader not set");
        return false;
    }

    if (attr[NFQA_PAYLOAD] == NULL)
    {
        LOG(LOG_ERR, "Packet has no payload");
        return false;
    }

    //Queue number
    struct nfgenmsg* nfg = mnl_nlmsg_get_payload(nlh);
    packet->queue_num = ntohs(nfg->res_id);

    //Data from netlink message header
    struct nfqnl_msg_packet_hdr *ph = mnl_attr_get_payload(attr[NFQA_PACKET_HDR]);
    if (ph == NULL)
    {
        LOG(LOG_ERR, "Packet metaheader is null");
        return false;
    }
    packet->packet_id = ntohl(ph->packet_id);
    packet->hw_protocol = ntohs(ph->hw_protocol);

    //Packet payload
    //Create a separate copy so that buffer can be reused
    void* payload = mnl_attr_get_payload(attr[NFQA_PAYLOAD]);
    packet->payload_len = mnl_attr_get_payload_len(attr[NFQA_PAYLOAD]);
    if (packet->payload_len == 0)
    {
        LOG(LOG_ERR, "Packet payload has zero length");
        return false;
    }
    packet->payload = malloc(packet->payload_len);
    ASSERT(packet->payload != NULL);
    memmove(packet->payload, payload, packet->payload_len);

    //Timestamp
    //Note: Packet timestamps do not always work reliably, e.g. kernel 4.4 always passes timestamp zero.
    //See https://patchwork.ozlabs.org/patch/269090/
    packet->has_timestamp = false;
    if (attr[NFQA_TIMESTAMP])
    {
        struct nfqnl_msg_packet_timestamp* ts = mnl_attr_get_payload(attr[NFQA_TIMESTAMP]);
        if (ts->sec != 0 || ts->usec != 0)
        {
            packet->has_timestamp = true;
            packet->timestamp.tv_sec  = __be64_to_cpu(ts->sec);
            packet->timestamp.tv_usec = __be64_to_cpu(ts->usec);
        }
    }
    if (!packet->has_timestamp)
    {
        LOG_ONCE(LOG_WARNING, "Kernel does not support packet timestamps");
        DEBUG("Packet timestamp not set");
    }

    //Conntrack data
    if (attr[NFQA_CT])
    {
        struct nlattr* attr1[CTA_MAX+1] = {0};
        packet->has_conntrack = true;
        if (mnl_attr_parse_nested(attr[NFQA_CT], parse_cta_attr_cb, attr1) < 0)
        {
            LOG_SYSERR("mnl_attr_parse_nested(CT)");
            return false;
        }
        if (attr1[CTA_ID])
            packet->conn_id = ntohl(mnl_attr_get_u32(attr1[CTA_ID]));
        if (attr1[CTA_STATUS])
            packet->conn_status = ntohl(mnl_attr_get_u32(attr1[CTA_STATUS]));
        if (attr1[CTA_MARK])
        {
            packet->has_connmark = true;
            packet->conn_mark = ntohl(mnl_attr_get_u32(attr1[CTA_MARK]));
        }
        if (attr1[CTA_TUPLE_ORIG])
            if (!read_addr_tuple(attr1[CTA_TUPLE_ORIG], &packet->orig))
            {
                LOG_SYSERR("read_addr_tuple(orig)");
                return false;
            }
        if (attr1[CTA_TUPLE_REPLY])
            if (!read_addr_tuple(attr1[CTA_TUPLE_REPLY], &packet->reply))
            {
                LOG_SYSERR("read_addr_tuple(reply)");
                return false;
            }
    }
    else
    {
        packet->has_conntrack = false;
        LOG_ONCE(LOG_WARNING, "Kernel does not support conntrack");
        DEBUG("Conntrack data not passed by kernel");
    }

    //Conntrack state
    if (attr[NFQA_CT_INFO])
        packet->conn_state = ntohl(mnl_attr_get_u32(attr[NFQA_CT_INFO]));
    else
        DEBUG("Conntrack state not passed by kernel");

    return true;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers and workarounds


//Helper
//Return <0 on failure
//See http://www.masterraghu.com/subjects/np/introduction/unix_network_programming_v1.3/ch14lev1sec2.html
static int recv_timeout(int fd, int64_t timeout_ms)
{
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    struct timespec ts =
    {
        .tv_sec  = timeout_ms/1000,
        .tv_nsec = (timeout_ms%1000) * 1000000
    };
    int retval = pselect(fd + 1, &rset, NULL, NULL, timeout_ms > 0? &ts : NULL, NULL);
    if (retval == -1 && errno == EINTR)  //signal
        return 0;  //no data
    else
        return retval;  // >0 if descriptor is readable
}


/*
Workaround for mnl_socket_open2() missing from libmnl.h and libmnl.so
Function has been added only in 2015 and Ubuntu 16.04 does not have it.
libmnl-dev 1.0.4 in Ubuntu 18.04 has it.
Source: https://git.netfilter.org/libmnl/tree/src/socket.c
See https://patchwork.ozlabs.org/patch/525782/
*/

#ifndef HAVE_MNL_SOCKET_OPEN2

struct mnl_socket
{
    int fd;
    struct sockaddr_nl addr;
};

static struct mnl_socket* mnl_socket_open2(int bus, int flags)
{
    struct mnl_socket *nl;
    nl = calloc(1, sizeof(struct mnl_socket));
    if (nl == NULL)
        return NULL;
    nl->fd = socket(AF_NETLINK, SOCK_RAW | flags, bus);
    if (nl->fd == -1)
    {
        free(nl);
        return NULL;
    }
    return nl;
}

#endif  //HAVE_MNL_SOCKET_OPEN2


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public interface

/*
Example usage

    struct nf_buffer buf[1];
    memset(buf, 0, sizeof(struct nf_buffer));

    while (...)
    {
        if (nfqueue_receive(nfqueue, buf, TIMEOUT) == IO_READY)
        {
            struct nf_packet packet[1];
            while (nfqueue_next(buf, packet) == IO_READY)
            {
                handle_packet(packet);
                free(packet->payload);
            }
        }
    }
    free(buf->data);
*/

/*
Note about thread safety

Function nfqueue_open() is not thread safe, but after the queue is open, most operation on it are.
Assuming that receive, send and select operations on a netlink socket are thread-safe, nfqueue_receive() and
nfqueue_verdict() are thread-safe with respect to nf_queue argument. This means that nf_queue object can be
shared between threads. On the other hand, operations on nf_buffer are not thread-safe, and because of that
every thread calling nfqueue_receive() and nfqueue_next() should use its own nf_buffer. By splitting nf_buffer
from nf_queue we allow for multi-threaded access to the netfilter queue.
Note that nfqueue_next() copies the payload from nf_buffer to packet object, thus nf_buffer can be reused in
nfqueue_receive() while packet object is being processed by another thread.
*/


//Return false on failure
//If queue_len is zero, use default value
bool nfqueue_open(struct nf_queue* q, int queue_num, uint32_t queue_len)
{
    memset(q, 0, sizeof(*q));
    q->queue_num = queue_num;

    DEBUG("Initializing nfqueue %d", queue_num);

	if ((q->nl_socket = mnl_socket_open2(NETLINK_NETFILTER, SOCK_NONBLOCK)) == NULL)
    {
		LOG_SYSERR("mnl_socket_open");
        return false;
	}

	if (mnl_socket_bind(q->nl_socket, 0, MNL_SOCKET_AUTOPID) < 0)
    {
		LOG_SYSERR("mnl_socket_bind");
		return false;
	}

	if (nfqueue_bind(q->nl_socket, queue_num) < 0)
    {
		LOG_SYSERR("nfqueue_bind");
		return false;
	}

	DEBUG("Configuring nfqueue %d (len=%u)", queue_num, queue_len);

    /*
    Note: For NFQA_CFG_F_CONNTRACK to be honored, modules nf_conntrack_ipv4 and nf_conntrack_ipv6 need to be loaded.
    This can be done either by including ip[6]tables rules containing -m conntrack, or by using modprobe.
    In older kernels (e.g. 4.4) module nf_conntrack_netlink also needs to be loaded.
    In kernel 4.15 modprobe-only method is not sufficient, rule containing -m conntrack must be used.
    Otherwise conntrack info is not passed by kernel (both NFQA_CT and NFQA_CT_INFO attributes are missing).
    */
	if (nfqueue_set_params(q->nl_socket, queue_num, NFQNL_COPY_PACKET, 0xFFFF, queue_len, NFQA_CFG_F_CONNTRACK) < 0)
    {
		LOG_SYSERR("nfqueue_set_params");
		return false;
	}

    DEBUG("nfqueue %d initialized", queue_num);
    return true;
}


void nfqueue_close(struct nf_queue* q)
{
    ASSERT(q);
    ASSERT(q->nl_socket);
    DEBUG("Closing socket for queue %d", q->queue_num);
	mnl_socket_close(q->nl_socket);  //nl_socket is freed here
}


//Return false on failure
//connmark is uint64_t to allow full 32-bit unsigned integer and also -1 (meaning: don't set connmark)
bool nfqueue_verdict(struct nf_queue* q, uint32_t packet_id, int verdict, int64_t connmark)
{
    ASSERT(q);

    DEBUG("Sending verdict for packet %u: verdict: %d, connmark: %ld", packet_id, verdict, connmark);

    if (nfqueue_send_verdict_connmark(q->nl_socket, q->queue_num, packet_id, verdict, connmark) < 0)
    {
        LOG_SYSERR("nfqueue_send_verdict_connmark");
        return false;
    }

    DEBUG("Verdict for packet %u sent successfully", packet_id);
    return true;
}


//Return 1 on success, -1 on failure, 0 on timeout (if timeout_ms > 0) or data not ready
int nfqueue_receive(struct nf_queue* q, struct nf_buffer* buf, int64_t timeout_ms)
{
    ASSERT(q);
    ASSERT(q->nl_socket);
    ASSERT(buf);

    if (buf->data == NULL)
    {
        buf->data = malloc(RECV_BUF_LEN);
        ASSERT(buf->data != NULL);
    }

    //Note: We execute recv_timeout() when timeout_ms is zero, because we had set SOCK_NONBLOCK when opening the socket.
    //recv_timeout() with zero timeout blocks until data becomes available.
    int retval = recv_timeout(mnl_socket_get_fd(q->nl_socket), timeout_ms);
    if (retval == 0)  //timeout
    {
        DEBUG("Netlink socket timeout");
        return IO_NOTREADY;
    }
    else if (retval < 1)  //error
    {
        if (errno == EINTR)
            return IO_NOTREADY;
        LOG_SYSERR("recv_timeout");
        return IO_ERROR;
    }
    //else data is ready

    int len = mnl_socket_recvfrom(q->nl_socket, buf->data, RECV_BUF_LEN);
    if (len < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)  //no data
        {
            DEBUG("No data from Netlink socket");
            return IO_NOTREADY;
        }
        else
        {
            LOG_SYSERR("mnl_socket_recvfrom");
            return IO_ERROR;
        }
    }
    else if (len == 0)  //EOF
    {
        LOG(LOG_ERR, "Netlink socket closed by peer");
        return IO_ERROR;
    }

    buf->len = len;
    buf->nlh = (struct nlmsghdr*)buf->data;
    DEBUG("Received data from netfilter (length=%d)", len);
    return IO_READY;
}


//Return 1 on success (result in packet), -1 on failure, 0 on no more data
int nfqueue_next(struct nf_buffer* buf, struct nf_packet* packet)
{
    ASSERT(buf);
    ASSERT(buf->data);
    ASSERT(packet);

    while (mnl_nlmsg_ok(buf->nlh, buf->len))
    {
        if (buf->nlh->nlmsg_flags & NLM_F_DUMP_INTR)
        {
            LOG(LOG_ERR, "Netlink dump interrupted");
            return IO_ERROR;
        }

        if (buf->nlh->nlmsg_type >= NLMSG_MIN_TYPE)
        {
            if (!nfqueue_parse(buf->nlh, packet))
                return IO_ERROR;
            buf->nlh = mnl_nlmsg_next(buf->nlh, &buf->len);
            return IO_READY;
        }
    }

    return IO_NOTREADY;
}


#endif //NFQUEUE_MNL_H
