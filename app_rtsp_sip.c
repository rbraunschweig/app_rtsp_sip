/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Sergio Garcia Murillo <sergio.garcia@fontventa.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief MP4 application -- save and play mp4 files
 * 
 * \ingroup applications
 */

/*
 * Tommy Long
 * Port to Asterisk version 17.3 and 17.5.
 *
 * Port 17.3 upgrades this code to run on Asterisk version 17.3, and adds 
 *   a thin SIP protocol in order to send audio to the same target device.
 * The development environment is Ubuntu 16.04 Desktop on 32bit machine.
 * Porting Generals:
 *   malloc() is now ast_malloc()
 *   free() is now ast_free()
 *   strdup() is now ast_strdup
 *   strndup() is now ast_strndup()
 *   memcpy() of overlapping memory is being changed to memmove().  
 *       This code was using memcpy to copy overlapping memory which is prone to errors (as I found out).
 *         memmove() is recommended instead.
 *   ast_log(LOG_DEBUG,....) change to ast_debug(<level>,...)
 * 
 * SIP is added for sending a one-way call to the same end device.
 *   The SIP protocol used is very simple and lite and completely self-contained within this module.
 *   There will be for sure various aspects of SIP not supported.  
 *
 * Documentation: in-line documentation is added.
 *
 * Port 17.5 
 * Development environment is Ubuntu 20.04 server cloud image running on KVM on 64 bit machine.
 * Fixes a few minor compiler warnings that show up in newer Ubuntu. 
 * Primariy fixes a problem with the ast_frame and buffer structure 
 *   used for receiving RTP from end device (using RTSP).
 * 
 * Has not been tested for:
 * 1) IPv6
 * 2) RTSP Digest Authentication (newly added).
 * 3) RTSP Tunnel
 * 4) Use DTMF to stop RTSP.
 * Tagged this as release [v1.0]
 *
 * [v1.1]
 * Updated to run with Asterisk version 22.2.0.
 *   Around Asterisk version 20.12.0. pjsip auth code was refactored
 *   and the Digest Authuorization used by this app no longer works.
 * Now uses its own Digest Authentication code for MD5
 *   which also uses Asterisk's md5 hash routine.
 *   Currently only supports the simpler Digest Auth based on RFC2069.
 *
 * Development environment uses Home Assistant Asterisk AddOn (a Docker Container)
 *   (See https://github.com/TECH7Fox/asterisk-hass-addons/)
 *   Its Docker file has been modified in order to create
 *   the development environment.
 *
 * [v2.0] 
 * The original code supported searching a RTSP/SIP message header-value
 *    -only when the "header" was present once
 *    -and the header contained only one "value".
 * This is a problem for cameras that support multiple authentication methods
 *   and the methods are spread across multiple headers.
 *
 * This version adds a new way of parsing RTSP/SIP messages, headers, and Auth params
 * but this version of code only applies this new way
 * to Authentication related RTSP/SIP messaging.
 * This will support multiple Auth methods in same WWW-Authenticate header
 * or across multiple WWW-Authenticate headers.
 *
 */

/* Use the following to test for Buffer length issues */
/* #define TEST_BUFFER */

#include <asterisk.h>
#include "asterisk/app.h" /* PORT 17.3 ADDed for parsing args */


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <arpa/inet.h> /* [17.x NEW]. needed for getsockname() */

#include <asterisk/lock.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/utils.h>
#include <asterisk/translate.h>
#include <asterisk/format_compatibility.h>


/* 
 * [v1.1] Now uses a custom digest authentication code
 * which also uses Asterisk's ast_md5_hash() in utils.c.
 * The include file for this ("asterisk/utils.h") has 
 * already been included above 
 */


/*** DOCUMENTATION
	<application name="RTSP-SIP" language="en_US">
		<synopsis>
			<para>Attempt to connect to another device/endpoint using RTSP 
			and play streaming audio (video has not been tested). </para>
			<para>If commanded, will also attempt to connect to the same device/endpoint 
			using SIP and send an audio stream to it. </para>
		</synopsis>
		<description>
			<para>This 'endpoint' application is intended to be used as an execution 
			step of an extension in a Dialplan.  When executed, the application will 
			first attempt to contact and authenticate with the specified target device using RTSP,
			gather the video and audio media types supported by the target device and match them
			with the media types supported by the Asterisk channel connecting to this application.
			It will choose the "best" media types for audio as well as video (if any) and play them
			using RTP into the Asterisk channel and consequently to the calling party.</para>
			<para>Once an audio stream is identifed and played using RTSP, 
			if sip has been specifically enabled, it will next attempt to contact the device using SIP.
			Once contacted, it will setup an audio stream from the Asterisk channel (that is connected to
			this application) to the target device using the same audio media chosen by RTSP.
			Consequently it will play audio from the calling party to the target device using RTP.
			This application uses its own 'lite' version of SIP and is intended 
			for simple uses cases, namely connecting this application as a sip UA 
			directly to the target device over a local area network.</para>
			<example title="Dial extension 103 to talk two-way with a surveillance camera">
			 exten = 103,1,Answer()
			 same = n,Wait(1)
			 same = n,RTSP-SIP(rtsp://2001:mypasswd@192.168.0.34:554/live.sdp,1,streaming_server,5060)
			 same = n,Wait(5)
			 same = n,Hangup()
			</example>
			<example title="Dial extension 101 to receive audio only from a surveillance camera">
			 exten = 101,1,Answer()
			 same = n,Wait(1)
			 same = n,RTSP-SIP(rtsp://user:mypasswd@192.168.0.34:554/live.sdp,0)
			 same = n,Wait(5)
			 same = n,Hangup()
			</example>
		</description>
		<syntax>
			<parameter name="RTSP-URL" required="true">
				<para>RTSP url of the target device to be played. 
				Format: rtsp://username:password@address/stream-id.
				'username' and 'password' are optional for login to the device using Basic Authentication. 
				'address' is domain name or ip address of target device. 
				'stream-id' optionally identifies the stream to be played.</para>
			</parameter>
			<parameter name="enable-sip" required="true">
				<para>0 if using this only to play RTSP streams.  
				1 if an audio stream is also to be setup to the target device.</para>
			</parameter>
			<parameter name="realm" required="false">
				<para>If enable-sip = 1, this parameter is required.  
				SIP uses Digest Authentication, and uses the supplied 
				realm string as part of the authentication.</para>
			</parameter>
			<parameter name="port" required="false">
				<para>If enable-sip = 1, this optional parameter can be used 
				to specify a different SIP port for the target device to listen on.  Default is 5060. </para>
			</parameter>
		</syntax>

		<see-also>
			<ref type="application">Dial</ref>
		</see-also>
	</application>
 ***/

/* [v2.0] Adders for new message/header/auth params parsing */
#define MAX_HEADERS 100		//max number of header lines in an RTSP/SIP message
#define MAX_HEADER_LINE 1024    //max size of a header line
#define MAX_FIELD_NAME 1024     //max size of a header's field name; otherwise strncpy warns of trunc
#define MAX_FIELD_VALUE 1024    //max size of a header's field value; otherwise strncpy warns of trunc

#define MAX_TOKEN 512        //number of chars say in a name, a key, a value
#define MAX_AUTH_KEY_VAL 20  //max num of an auth scheme's parameters as key/value pairs

typedef struct {
    char field_name[MAX_FIELD_NAME];
    char field_value[MAX_FIELD_VALUE];
} HeaderStruct;

typedef struct {
    HeaderStruct headers[MAX_HEADERS];
    int count;
} HeaderStructList;

/*
 * [v2.0] Simple routine that removes whitespaces from a string
 * starting at the beginning moving forward and 
 * then starting at the end and moving backwards.
 */
static void trim_whitespace(char *str) {
    char *end;
    // Trim leading space
    while(isspace((unsigned char)*str)) str++;
    // Trim trailing space
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
}

/* 
 * Helper function to get a trimmed copy of a string without modifying the original
 */
static char* get_trimmed_string(const char *str) {
    if (!str) return NULL;
    
    // Find start of non-whitespace content
    const char *start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    
    // Find end of non-whitespace content
    const char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }
    
    // Calculate length and create copy
    size_t len = end - start + 1;
    char *trimmed = ast_malloc(len + 1);
    if (trimmed) {
        strncpy(trimmed, start, len);
        trimmed[len] = '\0';
    }
    
    return trimmed;
}

/* 
 * [v2.0]
 * Parse raw buffer (string) containing an RTSP or SIP Message 
 *   to get at the start of the list of Headers.
 *   Return the location (as an arg) where the headers start.
 *
 * RFC7826 RSTP2.0 Sect. 20.2.2 RTSP Message
 * RTSP-message      = Request / Response  ; RTSP/2.0 messages
 * Request/Response  = Request-Line or Status-Line
 *                     *((Header) CRLF) 
 *                     CRLF
 *                     [ message-body-data ]
 * Request-Line = Method SP Request-URI SP RTSP-Version CRLF
 * Status-Line  = RTSP-Version SP Status-Code SP Reason-Phrase CRLF
 *
 * RFC3261 SIP Messages Sect. 7 much the same.
 *
 * So Buffer may contain other stuff before the list of headers.
 * Start at beginning (skipping over RTSP/SIP Request/Response)
 *   looking for the first "CRLF", then headers (if any) should start.
 *
 */
static int parse_message(char *buffer, char **headers_start) {
    if ( (*headers_start = strstr(buffer, "\r\n")) == NULL)
    {
	ast_log(LOG_WARNING,"Parsing RTSP/SIP message: No Start Line found\n");
        return -1;
    }
    else
    {
        *headers_start+=2;  //skip over CR LF
      //printf("first header char is %c\n",**headers_start);
    }
    return 0;
}

/*
 * [v2.0]
 * Parse RTSP/SIP Headers (within raw string buffer) beginning at the 
 *  location where the headers actually start.
 *
 * RFC7230 Definition of Header
 *   header-field   = field-name ":" OWS field-value OWS
 *
 *   field-name     = token
 *   field-value    = *( field-content / obs-fold )
 *   field-content  = field-vchar [ 1*( SP / HTAB ) field-vchar ]
 *   field-vchar    = VCHAR / obs-text
 *
 *   obs-fold       = CRLF 1*( SP / HTAB )
 *                  ; obsolete line folding
 */
static int parse_headers(char *buffer, HeaderStructList *header_list) {
    header_list->count = 0;
    char *line_start = buffer;
    char *fold_start;
    char *line_end;
    char line[MAX_HEADER_LINE];

    while (*line_start && header_list->count < MAX_HEADERS) 
    {
        /*
         * handle obsolete line folding where a Header line
         * can be extended using CRLF 1*( SP / HTAB )
         */
        int  line_extended = 0;
        fold_start = line_start;
        do
        {
            line_end = strstr(fold_start, "\r\n");
            if (!line_end) 
            {
                break; //Returned Null - Malformed header block
            }
            if( *(line_end+2) == ' '  || *(line_end+2) == '\t' )
            {
                line_extended=1;
	        ast_debug(6,"  Header line %d is extended\n",header_list->count);
                fold_start = line_end+2;
            }
            else
            {
                if( *(line_end+2) == '\r'  && *(line_end+3) == '\n' )
                {
                    //we're at CRLF after last header line.
                    line_extended=0;
	            ast_debug(6,"  Header line %d is NOT extended\n",header_list->count);
	            ast_debug(6,"  End of Headers found. Processing last line\n");
                }
                else
                {
                    line_extended=0;
	            ast_debug(6,"  Header line %d is NOT extended\n",header_list->count);
                }
            }
        }
        while( line_extended);

        if (!line_end) 
        {  
	    ast_log(LOG_WARNING,"  Malformed Header (no CRLF).\n");
            break; //Above returned Null looking for crlf - Malformed header block
        }
        size_t line_len = line_end - line_start;
        if (line_len == 0)  //line_start and line_end both at CRLF after all headers
        {
	    ast_debug(6,"  Header Line %d has length of zero (so not a line)\n",header_list->count);
            break; // Empty line = end of headers
        }
        if (line_len >= MAX_HEADER_LINE) {
	    ast_log(LOG_WARNING,"  Header line too long\n");
            return -1;
        }

        strncpy(line, line_start, line_len);
        line[line_len] = '\0';

        char *colon = strchr(line, ':');
        if (!colon) {
	    ast_log(LOG_WARNING,"Malformed header line (no colon): %s\n", line);
            return -1;
        }

        *colon = '\0';
        strncpy(header_list->headers[header_list->count].field_name, line, MAX_FIELD_NAME);
        strncpy(header_list->headers[header_list->count].field_value, colon + 1, MAX_FIELD_VALUE);
        trim_whitespace(header_list->headers[header_list->count].field_name);
        trim_whitespace(header_list->headers[header_list->count].field_value);

        header_list->count++;
        line_start = line_end + 2; // Skip over \r\n
    }

    return 0;
}


/* [17.x NEW] SIP: messaging sip:MY_NAME@blah_blah */
#define MY_NAME "agbell"


/* PORT 17.3
 * Adaptive MultiRate (AMR) Narrow Band (IETF RFC4867) is no longer supported in Asterisk.
 * This file is using it to determine MIME type and 
 * for some unknown reason is ORing this to determine best audio formats.
 * For now just zero it out .
 */
#ifndef AST_FORMAT_AMRNB
/* #define AST_FORMAT_AMRNB	(1 << 13) OLD */
#define AST_FORMAT_AMRNB	0
#endif 

/* PORT 17.3  AST_FORMAT_MPEG4 is now AST_FORMAT_MP4 (1LL << 22). The following is no longer needed. */
/* #ifndef AST_FORMAT_MPEG4 OLD 
 * #define AST_FORMAT_MPEG4        (1 << 22) OLD 
 * #endif OLD
 */

/* PORT17.3. Update to use xml based loading and documentation */
/* static char *name_rtsp_sip = "rtsp_sip"; OLD */
/* static char *syn_rtsp_sip = "sip caller with rtsp player"; */
/* static char *des_rtsp_sip = "  rtsp(url):  Play url. \n"; */
static const char app[] = "RTSP-SIP";

/* RTSP states */
#define RTSP_NONE		0
#define RTSP_DESCRIBE		1
#define RTSP_SETUP_AUDIO 	2
#define RTSP_SETUP_VIDEO 	3
#define RTSP_PLAY 		4
#define RTSP_PLAYING		5
#define RTSP_RELEASED 		6

/* [17.x NEW] SIP states */
#define SIP_STATE_NONE		0
#define SIP_STATE_OPTIONS	1
#define SIP_STATE_INVITE	2
#define SIP_STATE_ACK		3
#define SIP_STATE_CANCEL	4
#define SIP_STATE_BYE		5
#define SIP_STATE_REFER		6
#define SIP_STATE_NOTIFY	7
#define SIP_STATE_MESSAGE	8
#define SIP_STATE_SUBSCRIBE	9
#define SIP_STATE_INFO		10


#define PKT_PAYLOAD     9000
#define PKT_SIZE        (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + PKT_PAYLOAD)
#define PKT_OFFSET      (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET)


/* PORT 17.3
 *   Some of the following AST_FORMAT_xx were tweaked in format_compatibility.h;
 *   Plus the bit list is now 64bits
 */
static struct 
{
     /* int format; OLD */
        uint64_t format;
        char*    name;
} mimeTypes[] = {
	{ AST_FORMAT_G723, "G723"}, /* OLD { AST_FORMAT_G723_1, "G723"}, */
	{ AST_FORMAT_GSM, "GSM"},
	{ AST_FORMAT_ULAW, "PCMU"},
	{ AST_FORMAT_ALAW, "PCMA"},
	{ AST_FORMAT_G726, "G726-32"},
	{ AST_FORMAT_ADPCM, "DVI4"},
	{ AST_FORMAT_SLIN, "L16"}, /* OLD { AST_FORMAT_SLINEAR, "L16"}, */
	{ AST_FORMAT_LPC10, "LPC"},
	{ AST_FORMAT_G729, "G729"}, /* { AST_FORMAT_G729A, "G729"}, */
	{ AST_FORMAT_SPEEX, "speex"},
	{ AST_FORMAT_ILBC, "iLBC"},
	{ AST_FORMAT_G722, "G722"},
	{ AST_FORMAT_G726_AAL2, "AAL2-G726-32"},
	{ AST_FORMAT_AMRNB, "AMR"},
	{ AST_FORMAT_JPEG, "JPEG"},
	{ AST_FORMAT_PNG, "PNG"},
	{ AST_FORMAT_H261, "H261"},
	{ AST_FORMAT_H263, "H263"},
      /*{ AST_FORMAT_H263_PLUS, "H263-1998"}, OLD removed*/
	{ AST_FORMAT_H263P, "H263-2000"},/* OLD { AST_FORMAT_H263_PLUS, "H263-2000"}, */
	{ AST_FORMAT_H264, "H264"},
     /*	{ AST_FORMAT_MPEG4, "MP4V-ES"}, OLD */
	{ AST_FORMAT_MP4, "MP4V-ES"},
};

typedef enum 
{
	RTCP_SR   = 200,
	RTCP_RR   = 201,
	RTCP_SDES = 202,
	RTCP_BYE  = 203,
	RTCP_APP  = 204
} RtcpType;

typedef enum 
{
	RTCP_SDES_END    =  0,
	RTCP_SDES_CNAME  =  1,
	RTCP_SDES_NAME   =  2,
	RTCP_SDES_EMAIL  =  3,
	RTCP_SDES_PHONE  =  4,
	RTCP_SDES_LOC    =  5,
	RTCP_SDES_TOOL   =  6,
	RTCP_SDES_NOTE   =  7,
	RTCP_SDES_PRIV   =  8,
	RTCP_SDES_IMG    =  9,
	RTCP_SDES_DOOR   = 10,
	RTCP_SDES_SOURCE = 11
} RtcpSdesType;


struct RtcpCommonHeader
{
	unsigned short count:5;    /* varies by payload type */
	unsigned short p:1;        /* padding flag */
	unsigned short version:2;  /* protocol version */
	unsigned short pt:8;       /* payload type */
	unsigned short length;     /* packet length in words, without this word */
};

struct RtcpReceptionReport
{
	unsigned int  ssrc;            /* data source being reported */
	unsigned int fraction:8;       /* fraction lost since last SR/RR */
	int lost:24;                   /* cumulative number of packets lost (signed!) */
	unsigned int  last_seq;        /* extended last sequence number received */
	unsigned int  jitter;          /* interarrival jitter */
	unsigned int  lsr;             /* last SR packet from this source */
	unsigned int  dlsr;            /* delay since last SR packet */
};

struct RtcpSdesItem
{
	unsigned char type;             /* type of SDES item (rtcp_sdes_type_t) */
	unsigned char length;           /* length of SDES item (in octets) */
	char data[1];                   /* text, not zero-terminated */
};

struct Rtcp
{
	struct RtcpCommonHeader common;    /* common header */
	union 
	{
		/* sender report (SR) */
		struct
		{
			unsigned int ssrc;        /* source this RTCP packet refers to */
			unsigned int ntp_sec;     /* NTP timestamp */
			unsigned int ntp_frac;
			unsigned int rtp_ts;      /* RTP timestamp */
			unsigned int psent;       /* packets sent */
			unsigned int osent;       /* octets sent */ 
			/* variable-length list */
			struct RtcpReceptionReport rr[1];
		} sr;

		/* reception report (RR) */
		struct 
		{
			unsigned int ssrc;        /* source this generating this report */
			/* variable-length list */
			struct RtcpReceptionReport rr[1];
		} rr;

		/* BYE */
		struct 
		{
			unsigned int src[1];      /* list of sources */
			/* can't express trailing text */
		} bye;

		/* source description (SDES) */
		struct rtcp_sdes_t 
		{
			unsigned int src;              /* first SSRC/CSRC */
			struct RtcpSdesItem item[1]; /* list of SDES items */
		} sdes;
	} r;
};

struct RtpHeader
{
    unsigned int cc:4;        /* CSRC count */
    unsigned int x:1;         /* header extension flag */
    unsigned int p:1;         /* padding flag */
    unsigned int version:2;   /* protocol version */
    unsigned int pt:7;        /* payload type */
    unsigned int m:1;         /* marker bit */
    unsigned int seq:16;      /* sequence number */
    unsigned int ts;          /* timestamp */
    unsigned int ssrc;        /* synchronization source */
 /* unsigned int csrc[1];      * optional CSRC list. REMOVE. Not supported BY SIP. */
};

struct MediaStats
{
	unsigned int count;
	unsigned int minSN;
	unsigned int maxSN;
	unsigned int lastTS;
	unsigned int ssrc;
	struct timeval time;
};

static void MediaStatsReset(struct MediaStats *stats)
{
	stats->count 	= 0;
	stats->minSN 	= 0;
	stats->maxSN 	= 0;
	stats->lastTS	= 0;
	stats->time 	= ast_tvnow();
}

static void MediaStatsUpdate(struct MediaStats *stats,unsigned int ts,unsigned int sn,unsigned int ssrc)
{
	stats->ssrc = ssrc;
	stats->count++;
	if (!stats->minSN)
		stats->minSN = sn;
	if (stats->maxSN<sn)
		stats->maxSN = sn;
	stats->lastTS = ts;
}

static void MediaStatsRR(struct MediaStats *stats, struct Rtcp *rtcp)
{
	/* Set pointer as ssrc */
	/* COMMENT 
	 * Build a Receiver Report packet.
	 * An RR RTCP packet starts with the common header followed
	 * by the SSRC assigned to this receiver followed by report blocks
	 * of sender1 and sender2.
 	 */

	/*
 	 * COMMENT
	 * The next line originally set the SSRC to a pointer's value
 	 * because the pointer value is fairly random as the SSRC value.
 	 * However it doesn't always port/compile very well.  Let's use random() instead.
 	 */
     /* rtcp->r.rr.ssrc = htonl(stats); OLD */
        rtcp->r.rr.ssrc = htonl((uint32_t)random()); /* PORT17.5 fix compiler warning */

	/* data source being reported */
	rtcp->r.rr.rr[0].ssrc = htonl(stats->ssrc);

	/* fraction lost since last SR/RR */
	if (stats->maxSN-stats->minSN>0)
		rtcp->r.rr.rr[0].fraction = (signed)(255*stats->count/(stats->maxSN-stats->minSN)); 
	else
		rtcp->r.rr.rr[0].fraction = 0xFF;

	/* cumulative number of packets lost (signed!) */
	rtcp->r.rr.rr[0].lost = htonl((signed int)(stats->maxSN-stats->minSN-stats->count));

	/* extended last sequence number received */
	rtcp->r.rr.rr[0].last_seq = htonl(stats->maxSN);

	/* interarrival jitter */
	rtcp->r.rr.rr[0].jitter	= htonl(0xFF);

	/* last SR packet from this source */	
	rtcp->r.rr.rr[0].lsr = htonl(stats->lastTS);

	/* delay since last SR packet */
	rtcp->r.rr.rr[0].dlsr = htonl(ast_tvdiff_ms(ast_tvnow(),stats->time));

	/* Set common headers */
	rtcp->common.version	= 2;
        rtcp->common.p		= 0;
        rtcp->common.count	= 1;
        rtcp->common.pt		= 201;

	/* Length */
	rtcp->common.length = htons(7);
}


/* [17.x NEW]. For SIP */
enum SipMethodsIndex
{
	INVITE,  /* 0 */
	OPTIONS,
	ACK,
	CANCEL,
	BYE,
	REFER,
	NOTIFY,
	MESSAGE,
	SUBSCRIBE,
	INFO,
	MAX_METHODS
};

/* 
 * [17.x Changed]. 
 * RTSP Player was orignally modelled as a stream player 
 *   to get the camera to send streams to it.
 * Add stuff so SIP will use a Player instance to model 
 *   itself as a SIP Client.
 *  
 */
struct RtspPlayer
{
	int	fd;
	int	state;
	int	cseq;     
	char*	session[2];
	int 	numSessions;
	int 	end;      /* Used to exit the main loop */

	char*	ip;       /* destination ip string */
	int 	port;     /* destination port */
	char*	hostport; /* string ip:port */
	char*	url;
	int	isIPv6;

	char*	authorization;

	int	audioRtp;      /* file descriptor */
	int	audioRtcp;     /* file descriptor */
	int	videoRtp;      /* file descriptor */
	int	videoRtcp;     /* file descriptor */

	int	audioRtpPort;  /* source udp port */
	int	audioRtcpPort; /* source udp port */
	int	videoRtpPort;  /* source udp port */
	int	videoRtcpPort; /* source udp port */

	struct 	MediaStats audioStats;
	struct	MediaStats videoStats;

        /* [17.x NEW]. SIP */
	char*   local_ctrl_ip; /* source IPv4 address string used by SIP */
	uint16_t local_ctrl_port; /* source port used by SIP */
	int	cseqm[MAX_METHODS]; /* SIP differentiates CSeq  by sequence number plus Method Sec 20.16 */
	int     in_a_dialog;   /* SIP has a dialog going T/F */
	char    src_tag[20];   /* SIP random source tag value. Fixed when in a dialog. Hopefully only one dialog per time. */
	char    peer_tag[20];  /* SIP random tag value received from peer. */
	char    call_id[100];  /* SIP random call_id value when in a dialog. Hopefully only one call-id per time. */
	char    branch_id[100];/* SIP random branch_id last transaction. Hopefully only on transaction per time. */
	/* SDP */
	char    session_id[64];/* SDP for SIP sessionID */
};


/*
 *   [v2.0] updates this code to better handle parsing of Authentication Headers and parameters.
 *     Some specifications cited below:
 *
 *   RTSP 1.0 (RFC2326) via HTTP1.0 (RFC 2068) and
 *   RTSP 2.0 (RFC7826) both say:
 *   that the WWW-Authenticate field-value "might
 *   contain more than one challenge", or that there may
 *   may be "more than one WWW-Authenticate
 *   header field is provided, the contents of a challenge itself can
 *   contain a comma-separated list of authentication parameters".
 *
 *   This code is being updated to search for header-value accordingly.
 *   [Remark] fixes issue in original code when a camera 
 *     supports Digest in first header and Basic in second header, 
 *     and searching for Basic failed because original code 
 *     only looked in first header.
 *
 * Parsing Authentication Data
 * RFC7826 (RTSP2.0)
 *   RTSP Authentication is specified in HTTP1.1 Auth [RFC7235].
 *   Its usage depends on the used authentication schemes, such as 
 *   - Digest [RFC7616]
 *     The value of the header field can include parameters list:
 *     realm, domain, nonce, opaque, stale, algorithm, qop, charset, userhash
 *   - Basic  [RFC7617]
 *     o  The authentication parameter 'realm' is REQUIRED ([RFC7235], Section 2.2).
 *     o  The authentication parameter 'charset' is OPTIONAL (see Section 2.1).
 *     The only parameter supported in this code is 'realm'.
 *   - The full list of Auth Schemes: 
 *          https://www.iana.org/assignments/http-authschemes/http-authschemes.xhtml
 *
 * RFC7235 (HTTP1.1 Auth) 
 *  Uses Augmented Backus-Naur Form (ABNF) defined in [RFC5234] 
 *      with a list extension, defined in Section 7 of HTTP1.1 [RFC7230],
 *   Two definitions in [RFC7235]: section 4.1, and Appendix C
 *   WWW-Authenticate = 1#challenge  //ABNF Notation
 *   WWW-Authenticate = *( "," OWS ) challenge *( OWS "," [ OWS challenge ] )
 *     "*" <default=0>*<default=infinity>,so * means 0 to inifinity of, 
 *         1* means at least 1 to infinity to, *1 means at least 0 to 1 of
 *     '#' similar to "*" but the operator is a comma separated list. 1# means 1 to infinity of
 *     "/" means OR
 *     OWS = *( SP / HTAB ) from RFC7230 (also says "zero or more linear whitespace octets")
 *         so contradiction as LWS in RFC5234 also includes CRLF. 
 *     ()  Elements enclosed in parentheses are treated as a single element,
 *         whose contents are strictly ordered
 *     []  Square brackets enclose an optional element sequence: [foo bar] same as ( 0*1(foo bar) )
 *
 *     challenge   = auth-scheme [ 1*SP ( token68 / #auth-param ) ]
 *     '1*SP' means at least one space (space = 0x20).
 *     token68 = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"="
 *     auth-param = token BWS "=" BWS ( token / quoted-string )
 *     token = 1*tchar
 *        tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*"
 *               / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
 *               / DIGIT / ALPHA
 *     BWS = OWS that shouldn't really be there but allowed for.
 * 
 */ 


/*
 * [v2.0] Parse for Authentication Scheme
 * 
 * This routine is very rudimentary in that it assumes
 * the authentication scheme is at the beginning of the
 * "input" string and ends with 1 or more SP.
 * It then returns (as an arg) what should be the location of the start of the 
 * authentication parameters for that scheme which
 * should be after all the 1 or more SPs.
 *
 */
static int parse_auth_scheme(char *input, char *scheme, char **rest) {
    int i = 0;
    while (input[i] && !isspace((unsigned char)input[i]) && i < MAX_TOKEN - 1) {
        scheme[i] = input[i];
        i++;
    }
    scheme[i] = '\0';

    // Skip spaces after scheme
    while (input[i] && isspace((unsigned char)input[i])) i++;

    *rest = &input[i];
    return 0;
}

/*
 * [v2.0] For future use. 
 *        An Auth scheme can use a token instead of parameters (key/value pairs)
 * Try parsing/verifying presence of a token68
 */
static int is_token68(char *str) {
    for (int i = 0; str[i]; i++) {
        if (!isalnum((unsigned char)str[i]) && str[i] != '+' && str[i] != '/' && str[i] != '=')
            return 0;
    }
    return 1;
}

/*
 * [v2.0]  Parse Authentication Parameters into key/value pairs.
 * 
 * Given the start of a string containing one or more authentication parameters
 * in a comma separated list (within a WWW-Authenticate header), find
 * and separate into an array of "key" and an array of "value" where each
 * array element is a pointer to a string for that key or value.
 * Return these arrays (as args).
 *
 * A comma-separated list may continue on not with a parameter but
 * instead with another Authentication Scheme (with its own parameters)
 * which is denoted by lack of "=". So return (as an arg) 
 * with the starting location of this additional Scheme (if none, return NULL).
 * Note: Each key/value string is allocated from memory, so needs to be freed later.
 * 
 */
static int parse_auth_params(char *str,char *param_key[], char *param_val[], int *param_count, char **more_auths) {
    char key[MAX_TOKEN], value[MAX_TOKEN];
    char *p = str;
    int param_kv_count=0;

  //*more_auths = '\0';
    *more_auths = NULL;

    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',') p++;  // OWS and comma

        // Parse key
        int i = 0;
        while (*p && *p != '=' && *p !=' ' && i < MAX_TOKEN - 1 && *p != '\0') {  //Key could have BWS, but we won't support.
            key[i++] = *p++;
        }
        key[i] = '\0';
        param_key[param_kv_count] = ast_strdup(key);  //does a malloc
        if (param_key[param_kv_count] == NULL)
        {
	    ast_log(LOG_ERROR,"ast_strdup param_key failure.\n");
            return -1;
        }
      //printf("Key: %s ",key);
        if (*p != '=') {
          //printf("Key not followed by '='. May be finished or may be another auth-scheme.\n");
            //Backup 
          //printf("Removing last Key with length %li\n",strlen(key));
            *more_auths = (p-strlen(key));
            key[i-strlen(key)] = '\0';
            break;
        }
        p++;  // skip '='

        // Parse value
        i = 0;
        if (*p == '"') {
            p++;  // skip opening quote
            while (*p && i < MAX_TOKEN - 1) {
                if (*p == '"' && *(p-1) != '\\'){  //if quote not escaped, then done
                    break;
                }
                value[i++] = *p++;
            }
            if (*p == '"') p++;  // skip closing quote
        } else {
            while (*p && *p != ',' && i < MAX_TOKEN - 1) {
                value[i++] = *p++;
            }
        }
        value[i] = '\0';
        param_val[param_kv_count] = ast_strdup(value); //does a malloc
        if (param_val[param_kv_count] == NULL)
        {
	    ast_log(LOG_ERROR,"ast_strdup param_val failure.\n");
            return -1;
        }
      //printf("Value: %s\n",value);
      //printf("Auth-Param: %s = %s\n", key, value);

        param_kv_count++;
    }
  //printf("Num of Auth Parm K-V pairs %i\n",param_kv_count);
    *param_count = param_kv_count;
    
    return 0;
}

/* [17.x NEW] Digest Auth Data */
struct DigestAuthData
{
	char nonce[64];
	char nc[64];
	char cnonce[64];
	char qop[16];
	char uri[64];
	char rx_realm[32];
	char opaque[64];
	char algorithm[64]; //[v2.0] adder
};

/* [v2.0] adder - Basic Auth Data */
struct BasicAuthData
{
	char rx_realm[128];
};


/*
 * [v2.0] Check for Presence of a Specific Authentication Scheme.
 *
 * Given a buffer string containing an entire RTSP/SIP message, 
 * parse the message looking for one or more Authentication Schemes 
 * (in a WWW-Authenticate header) until a match on a specific
 * scheme is found, and then have that scheme parsed for its authentication parameters
 * and then separate the parameters into an array of "key" and an array of "value" where each
 * array element is a pointer to a string for that key or value.
 * These arrays are returned (as args) along with a count. 
 * Note: Each key/value string is allocated from memory, so needs to be freed later.
 */
static int CheckAuthScheme(char *buffer,int bufferLen, char *scheme_to_match, char *auth_paramkey[], char *auth_paramval[], int *auth_paramcount)
{
    char* headers_start;

    char scheme[MAX_TOKEN];
    char *paramkey[MAX_AUTH_KEY_VAL];
    char *paramval[MAX_AUTH_KEY_VAL];
    int  param_count;
    int  i,j;
    int return_code=-10;
    char *auth_start;

    char *rest;       /* rest of the string after Auth Scheme (which are the params) */
    char *more_auths; /* Another Auth Scheme (and its params) found afterwards */

    ast_debug(5,"    Checking Headers for Matching Auth Scheme.\n");
    if( parse_message(buffer, &headers_start) )
    {
        ast_debug(5,"    Could not parse RTSP/SIP message\n");
        return_code= -1;
    }
    else
    {
        HeaderStructList headers;
    
        if (parse_headers(headers_start, &headers) == 0) //returns with 2xarray of parsed header key,value pairs
        {
            ast_debug(5,"  ---Parsing Headers---\n");
            for (i = 0; i < headers.count; ++i) {
                ast_debug(5,"    %s = %s\n", headers.headers[i].field_name, headers.headers[i].field_value);
                if(strstr(headers.headers[i].field_name,"WWW-Authenticate")!=NULL)
                {
                    ast_debug(5,"    Found a WWW-Authenticate Header\n" );

                    // Get a trimmed copy of the auth header value
                    char *auth_start_trimmed = get_trimmed_string(headers.headers[i].field_value);
                    if (!auth_start_trimmed) {
                        ast_debug(5,"    Failed to allocate memory for trimmed auth string\n");
                        continue;
                    }
                    auth_start = auth_start_trimmed;
                    ast_debug(6,"      Auth start string:\n%s\n",auth_start);
                    do {
                        more_auths = NULL;
                        parse_auth_scheme(auth_start, scheme, &rest);
                        ast_debug(6,"    Found an Auth-Scheme: %s\n", scheme);
                        if(strcmp(scheme, scheme_to_match) == 0)
                        {
                            ast_debug(5,"    Found matching Auth-Scheme: %s\n", scheme_to_match);
                            return_code= 0;
                        }         

                        if (*rest == '\0') {
                            ast_debug(5,"    No parameters or token68 found.\n");
                            auth_paramkey[0] = ast_strdup("None"); //does a malloc
                            auth_paramval[0] = ast_strdup("None"); //does a malloc
                            *auth_paramcount=1;
                        } else if (is_token68(rest)) {
                            ast_debug(5,"  Token68: %s\n", rest);
                            //Have not tested token68!!
                            auth_paramkey[0] = ast_strdup("Token68"); //does a malloc
                            auth_paramval[0] = ast_strdup(rest); //does a malloc
                            *auth_paramcount=1;
                        } else {
                            ast_debug(5,"  ---Parsing Auth-Params---\n");
                            if( parse_auth_params(rest,paramkey,paramval,&param_count, &more_auths) == 0)
                            {
                              //printf("Param Count: %d\n", param_count);
                                for ( j = 0; j < param_count; j++) {
                                    ast_debug(5,"  Paramkey[%d]: %s    Paramval[%d]: %s\n", j,paramkey[j], j, paramval[j]);
                                    if( return_code == 0)
                                    {
                                        auth_paramkey[j] = ast_strdup(paramkey[j]); //does a malloc
                                        auth_paramval[j] = ast_strdup(paramval[j]); //does a malloc
                                        *auth_paramcount = param_count;
                                    }
                                    ast_free(paramkey[j]);
                                    ast_free(paramval[j]);
                                }
                            }
                            ast_debug(5,"  ---End Parsing Auth-Params---\n");
                        }
                        if (more_auths != NULL)
                        {
                            ast_debug(6,"  more auths after comma-sep list: %s\n",more_auths);
                            auth_start = more_auths;
                        }

                    }
                    while(more_auths != NULL);
                    
                    // Free the allocated trimmed string
                    ast_free(auth_start_trimmed);
                }
            }
            ast_debug(5,"  ---End Parsing Headers---\n");

        } else {
	    ast_log(LOG_WARNING,"No RTSP/SIP headers found.\n");
            return_code= -2;
        }
    }
    return return_code;
}

/* 
 * [v2.0] Check WWW-Authenticate Headers for Basic Authentication scheme and get/convert any parameters 
 */
static int GetAuthSchemeBasic(char *buffer,int bufferLen, struct BasicAuthData *basic_data)
{
    int return_code = -1;
    char *auth_paramkey[MAX_AUTH_KEY_VAL];
    char *auth_paramval[MAX_AUTH_KEY_VAL];
    int auth_paramcount;
    int pi; //auth parameter index

    basic_data->rx_realm[0]='\0';

    ast_debug(5,"\n");
    ast_debug(5,"GetAuthSchemeBasic()\n");
    if (CheckAuthScheme(buffer,bufferLen,"Basic", auth_paramkey, auth_paramval, &auth_paramcount) == 0 )
    {
        ast_debug(5,"    - GetAuthSchemeBasic: Found WWW-Authenticate Method of Basic\n");
        return_code = 0;
        if( auth_paramcount == 0)
        {
            ast_debug(5,"    GetAuthSchemeBasic: Did not to find any params for Basic authentication\n");
        }
        else
        {
            ast_debug(5,"  --- Auth Key/Value pairs/struct ---\n");
            for ( pi = 0; pi < auth_paramcount; pi++) {
	        ast_debug(5,"  AuthParamkey[%d]: %s, AuthParamval[%d]: %s\n", pi,auth_paramkey[pi], pi, auth_paramval[pi]);
                if( strcmp(auth_paramkey[pi],"realm") == 0 )
                {
                    strcpy(basic_data->rx_realm, auth_paramval[pi]);
	            ast_debug(5,"  basic_data->rx_realm: %s\n",basic_data->rx_realm);
                } 
                /* Always free the malloc for the Auth param key/value arrays.*/
                ast_free(auth_paramkey[pi]);
                ast_free(auth_paramval[pi]);
            }
            ast_debug(5,"  --- End Auth Key/Value pairs/struct ---\n");
        }
    }
    ast_debug(5,"End of GetAuthSchemeBasic()\n");
    return return_code;
}

/* 
 * [v2.0] Check WWW-Authenticate Headers for Digest Authentication scheme and get/convert any parameters 
 */
static int GetAuthSchemeDigest(char *buffer,int bufferLen, struct DigestAuthData *digest_data)
{
    int return_code = -1;
    char *auth_paramkey[MAX_AUTH_KEY_VAL];
    char *auth_paramval[MAX_AUTH_KEY_VAL];
    int auth_paramcount;
    int pi; //auth parameter index

    digest_data->nonce[0]='\0';
    digest_data->nc[0]='\0';
    digest_data->cnonce[0]='\0';
    digest_data->qop[0]='\0';
    digest_data->uri[0]='\0';
    digest_data->rx_realm[0]='\0';
    digest_data->opaque[0]='\0';
    digest_data->algorithm[0]='\0';

    ast_debug(5,"\n");
    ast_debug(5,"GetAuthSchemeDigest()\n");
    if (CheckAuthScheme(buffer,bufferLen,"Digest", auth_paramkey, auth_paramval, &auth_paramcount) == 0 )
    {
        ast_debug(5,"    - GetAuthSchemeDigest: Found WWW-Authenticate Method of Digest\n");
        return_code = 0;
        if( auth_paramcount == 0)
        {
            ast_debug(5,"    - GetAuthSchemeDigest: Did not to find any params for Digest authentication\n");
        }
        else
        {
            ast_debug(5,"    --- Auth Key/Value pairs/struct ---\n");
            for ( pi = 0; pi < auth_paramcount; pi++) 
            {
	        ast_debug(5,"    AuthParamkey[%d]: %s, AuthParamval[%d]: %s\n", pi,auth_paramkey[pi], pi, auth_paramval[pi]);

                if( strcmp(auth_paramkey[pi],"realm") == 0 )
                {
                    strcpy(digest_data->rx_realm, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->rx_realm: %s\n",digest_data->rx_realm);
                } 
                if( strcmp(auth_paramkey[pi],"nonce") == 0 )
                {
                    strcpy(digest_data->nonce, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->nonce: %s\n",digest_data->nonce);
                } 
                if( strcmp(auth_paramkey[pi],"nc") == 0 )
                {
                    strcpy(digest_data->nc, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->nc: %s\n",digest_data->nc);
                } 
                if( strcmp(auth_paramkey[pi],"cnonce") == 0 )
                {
                    strcpy(digest_data->cnonce, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->cnonce: %s\n",digest_data->cnonce);
                } 
                if( strcmp(auth_paramkey[pi],"qop") == 0 )
                {
                    strcpy(digest_data->qop, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->qop: %s\n",digest_data->qop);
                } 
                if( strcmp(auth_paramkey[pi],"uri") == 0 )
                {
                    strcpy(digest_data->uri, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->uri: %s\n",digest_data->uri);
                } 
                if( strcmp(auth_paramkey[pi],"opaque") == 0 )
                {
                    strcpy(digest_data->opaque, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->opaque: %s\n",digest_data->opaque);
                } 
                if( strcmp(auth_paramkey[pi],"algorithm") == 0 )
                {
                    strcpy(digest_data->algorithm, auth_paramval[pi]);
	            ast_debug(5,"    digest_data->algorithm: %s\n",digest_data->algorithm);
                } 
                /* Always free the malloc for the Auth param key/value arrays.*/
                ast_free(auth_paramkey[pi]);
                ast_free(auth_paramval[pi]);
            }
            ast_debug(5,"    --- End Auth Key/Value pairs/struct ---\n");
        }
    }
    ast_debug(5,"End of GetAuthSchemeDigest()\n");
    return return_code;
}


/*
 * [v1.1] Custom code for Digest Authentication.
 * Computes the response parameters to a challenge for MD5
 *
 * Supports RFC2069 (Digest Access Authentication for HTTP 1.0).
 *
 *   Note: Uses Asterisk's ast_md5_hash() to compute the MD5 hash.
 */
static int auth_digest(char* username, \
                 char* passwd, \
                 char* realm, \
                 char* nonce, \
                 char* uri, \
                 char* method, \
                 char* digest_result) 
{
  
  //char hash_result[64];
    char hash_result[256];

    /*
     * Run a test to make sure MD5 hash is not broke
     */
    char* string_to_compare = "c3fcd3d76192e4007dfb496cca67e13b";
    char* string_to_hash = "abcdefghijklmnopqrstuvwxyz";
    ast_md5_hash(hash_result, string_to_hash);
#ifdef PRINT_MD5HASH_TEST 
    ast_debug(6,"Auth Digest Test Result should be: c3fcd3d76192e4007dfb496cca67e13b\n");
    ast_debug(6,"Auth Digest Test Result actual   : %s\n", hash_result);
#endif
    if (strcmp(string_to_compare,hash_result)!=0){ /* are not equal */
      //printf("Auth Digest Test failed\n"); 
        return -1;
    }
  
    /*
     * Next, compute the digest response to a digest challenge
     * using MD5 hash and an algorithm according to 
     * RFC2069 (Digest Access Authentication for HTTP 1.0).
     *
     *     response-digest     =
     *      <"> < KD ( H(A1), unquoted nonce-value ":" H(A2) > <">
     *
     *     A1             = unquoted username-value ":" unquoted realm-value
     *                                            ":" password
     *     password       = < user's password >
     *     A2             = Method ":" digest-uri-value
     *
     *     where:
     *        KD(secret, data) = H(concat(secret, ":", data))
     *        For the "MD5" algorithm
     *           H(data) = MD5(data)
     *
     */
    char  A1[256];
    char  A2[256];
    char  HA1[256];
    char  HA2[256];
    char  KD_ARGS[513]; /* 512+'\0' */

    sprintf(A1, "%s:%s:%s", username,realm,passwd);
    ast_md5_hash(hash_result, A1);
    sprintf(HA1,"%s", hash_result);
  
    sprintf(A2,"%s:%s", method, uri);
    ast_md5_hash(hash_result, A2);
  
    sprintf(HA2,"%s", hash_result);
    sprintf(KD_ARGS,"%s:%s:%s",HA1,nonce,HA2);
    ast_md5_hash(digest_result, KD_ARGS);

    return 0;
}

/* [17.x NEW]. For SIP */
static int generateSrcTag(struct RtspPlayer *player)
{
	sprintf(player->src_tag,"%08lx", ast_random()); /* Set SIP source Tag */
	return 1;
}

/* [17.x NEW]. For SIP */
static int generateBranch(struct RtspPlayer *player)
{
	sprintf(player->branch_id,"z9hG4bKi-%08lx%08lx%08lx%08lx",random(),random(),random(), random() );
	return 1;
}

/* [17.x NEW]. For SIP */
static int generateCallId(struct RtspPlayer *player)
{
	if(player->local_ctrl_ip == NULL)
		sprintf(player->call_id,"%08lx%08lx%08lx%08lx@foo.bar.com",random(),random(),random(), random() );
	else
		sprintf(player->call_id,"%08lx%08lx%08lx%08lx@%s",random(),random(),random(), random(),player->local_ctrl_ip );
	return 1;
}

/* [17.x NEW]. For SIP */
static int generateSessionId(struct RtspPlayer *player)
{
	sprintf(player->session_id,"158%8ld",random()); /*SDP for SIP RFC3264 Sec 5 requires 64bits; we'll use 32bits */
	return 1;
}

static struct RtspPlayer* RtspPlayerCreate(void)
{
	/* malloc */
     /*	struct RtspPlayer* player = (struct RtspPlayer*) malloc(sizeof(struct RtspPlayer)); OLD */
	struct RtspPlayer* player = (struct RtspPlayer*) ast_malloc(sizeof(struct RtspPlayer)); /* PORT 17.3 */
	/* Initialize */
	player->cseq 		= 1;
	player->session[0]	= NULL;
	player->session[1]	= NULL;
	player->numSessions	= 0;
	player->state 		= RTSP_NONE;
	player->end		= 0;
	player->ip		= NULL;
	player->hostport	= NULL;
	player->isIPv6		= 0;
	player->port		= 0;
	player->url		= NULL;
	player->authorization	= NULL;
	player->fd		= 0; /* Control Protocol (RTSP or SIP) file descriptor */
	player->audioRtp	= 0; /* file descriptor */
	player->audioRtcp	= 0; /* file descriptor */
	player->videoRtp	= 0; /* file descriptor */
	player->videoRtcp	= 0; /* file descriptor */
	player->audioRtpPort	= 0; /* Source udp ports */
	player->audioRtcpPort	= 0; /* Source udp ports */
	player->videoRtpPort	= 0; /* Source udp ports */
	player->videoRtcpPort	= 0; /* Source udp ports */

        /* ADD. SIP */
	player->local_ctrl_ip   = NULL; /* source IPv4 address string*/
	player->local_ctrl_port = 0;  /* source port used by SIP */
	int i;
	for(i=0;i<MAX_METHODS;i++)
		player->cseqm[i] = 1;
	player->in_a_dialog      = 0; /* SIP has a dialog going T/F */
	generateSrcTag(player);       /* Set SIP source Tag */
	generateBranch(player);
	generateCallId(player);
	generateSessionId(player);

	/* Return */
	return player;
}

static void RtspPlayerDestroy(struct RtspPlayer* player)
{
	/* free members*/
     /*	if (player->ip) 	free(player->ip); OLD */
	if (player->ip) 	ast_free(player->ip);
     /*	if (player->hostport) 	free(player->hostport); OLD */
	if (player->hostport) 	ast_free(player->hostport);
     /* if (player->url) 	free(player->url); OLD */
	if (player->url) 	ast_free(player->url);
     /* if (player->session[0])	free(player->session[0]); OLD */
	if (player->session[0])	ast_free(player->session[0]);
     /* if (player->session[1])	free(player->session[1]); OLD */
	if (player->session[1])	ast_free(player->session[1]);
     /* if (player->authorization)	free(player->authorization); OLD */
	if (player->authorization)      ast_free(player->authorization);

     /* ADDED */
	if (player->local_ctrl_ip) ast_free(player->local_ctrl_ip);

	/* free */
     /*	free(player); OLD */
	ast_free(player);
}

static void RtspPlayerBasicAuthorization(struct RtspPlayer* player,char *username,char *password)
{
	char base64[256];
	char clear[256];

	/* Create authorization header */
     /*	player->authorization = malloc(128); OLD */
	player->authorization = ast_malloc(128); /* PORT 17.3 */

	/* Get base 64 from username and password*/
	sprintf(clear,"%s:%s",username,password);

	/* Encode */
     /*	ast_base64encode(base64,clear,strlen(clear),256); OLD */
        ast_base64encode(base64,(const unsigned char*)clear,strlen(clear),256); /* PORT 17.3. Fix compiler warning */

	/* Set heather */
	sprintf(player->authorization, "Authorization: Basic %s",base64);
}

/* [17.x NEW]. 
 * Added Digest Authentication 
 */
static int RtspPlayerDigestAuthorization(struct RtspPlayer *player,char *cfg_username,\
		char *cfg_password, char *cfg_realm, char *nonce, char *nc, \
		char *cnonce, char *qop, char *uri, char* rx_realm, char *method, int isSIP)
{

	/* See if Received realm differs from Configured realm */
	if (strcmp(cfg_realm,rx_realm)!=0){ /* are not equal */
		ast_log(LOG_ERROR,"Received realm %s doesn't match configured realm %s.\n",rx_realm,cfg_realm); 
		return -1;
	}
	/* Create authorization header */
	player->authorization = ast_malloc(256); /* Freed in  RtspPlayerDestroy */

	int  string_len = 0;

	char digest_result[256];
        int result;

        /* [v1.1] change from PJSIP auth code to custom Digest auth code */
        result=auth_digest(cfg_username, \
              cfg_password, \
              rx_realm, \
              nonce, \
              uri, \
              method, \
              digest_result);

	ast_debug(3,"      Player Digest Result: %s\n",digest_result);

	if ( result == -1) {
		ast_log(LOG_ERROR,"MD5 hash computation test failed! Not tested on Big Endian\n");
	}

	if(isSIP){
		/* 
	 	 * RFC 3261 p226: dig-resp: username="string",realm="string",
		 *                         nonce="string",uri="string",response="32LHex",
		 * no one quotes MD5       algorithm=<"MD5"|token>,cnonce="string",opaque="string",
		 * no one quotes qopvalue  qop="qop-value",nc=8Lhex,token=<token|"string'>
		 */

		/* Auth Header should look something like the following:
		 *    Note: comma-separated list (space after comma) ex. sect 20.44, 22.2
		 * Authorization: Digest\r\n  username="my_name", realm="streaming_server", 
		 *   nonce="a50392b361ce351d9a95e73a45a6b133", uri="sip:0002D151D42F@192.168.0.43:5060",
		 *   response="b8325b600081488dde51435dda7e3162", algorithm=MD5\r\n
		 */
		string_len = sprintf(player->authorization,
			"Authorization: Digest " 
			"username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
		cfg_username,rx_realm,nonce,uri,digest_result);
		if(cnonce != NULL)
			string_len += sprintf(player->authorization+string_len,",cnonce\"%s\"",cnonce);
		if(qop != NULL)
			string_len += sprintf(player->authorization+string_len,",qop\"%s\"",qop);
		if(nc != NULL)
			string_len += sprintf(player->authorization+string_len,",nc\"%s\"",nc);
            
	}
	else{  /* RTSP - Use the same full format as SIP */
		string_len = sprintf(player->authorization,
			"Authorization: Digest " 
			"username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
		cfg_username,rx_realm,nonce,uri,digest_result);
		if(cnonce != NULL)
			string_len += sprintf(player->authorization+string_len,", cnonce=\"%s\"",cnonce);
		if(qop != NULL)
			string_len += sprintf(player->authorization+string_len,", qop=\"%s\"",qop);
		if(nc != NULL)
			string_len += sprintf(player->authorization+string_len,", nc=\"%s\"",nc);
		ast_debug(3,"RTSP Digest Authentication applied with full format\n");
	}
	ast_debug(3,"      Player Auth String: \n%s\n",player->authorization);
	return 1;
}

/* 
 * COMMENT: 
 * GetUdpPorts() forces the source ports for RTP/RTCP to be paired odd/even respectively. 
 */ 
static void GetUdpPorts(int *a,int *b,int *p,int *q,int isIPv6)
{
	struct sockaddr *sendAddr;
	int size;
	int len;
	int PF;
	unsigned short *port;

	/* If it is ipv6 */
	if (isIPv6)
	{
		/* Set size*/
		size = sizeof(struct sockaddr_in6);
		/*Create address */
           /*	sendAddr = (struct sockaddr *)malloc(size); OLD */
		sendAddr = (struct sockaddr *)ast_malloc(size);
		/* empty addres */
		memset(sendAddr,0,size);
		/*Set family */
		((struct sockaddr_in6*)sendAddr)->sin6_family = AF_INET6;
		/* Set PF */
		PF = PF_INET6;
		/* Get port */
		port = &(((struct sockaddr_in6 *)sendAddr)->sin6_port);
	} else {
		/* Set size*/
		size = sizeof(struct sockaddr_in);
		/*Create address */
             /*	sendAddr = (struct sockaddr *)malloc(size); OLD */
		sendAddr = (struct sockaddr *)ast_malloc(size);
		/* empty addres */
		memset(sendAddr,0,size);
		/*Set family */
		((struct sockaddr_in *)sendAddr)->sin_family = AF_INET;
		/* Set PF */
		PF = PF_INET;
		/* Get port */
		port = &(((struct sockaddr_in *)sendAddr)->sin_port);
	}

	/* Create sockets */
	*a = socket(PF,SOCK_DGRAM,0);
	bind(*a,sendAddr,size);
	*b = socket(PF,SOCK_DGRAM,0);
	bind(*b,sendAddr,size);

	/* Get socket ports */
	len = size;
     /*	getsockname(*a,sendAddr,&len); OLD */
        getsockname(*a,sendAddr,(socklen_t*)&len); /* PORT17.3. fix compiler warning */

	*p = ntohs(*port);
	len = size;
     /*	getsockname(*b,sendAddr,&len); OLD */
        getsockname(*b,sendAddr,(socklen_t*)&len); /* PORT17.3. fix compiler warning */
	*q = ntohs(*port);

     /*	ast_log(LOG_DEBUG,"-GetUdpPorts [%d,%d]\n",*p,*q); OLD */
	ast_debug(4,"-GetUdpPorts initial [%d,%d]\n",*p,*q); 

	/* Create audio sockets */
	while ( *p%2 || *p+1!=*q )
	{
		/* Close first */
		close(*a);
		/* Move one forward */
		*a = *b;
		*p = *q;
		/* Create new socket */
		*b = socket(PF,SOCK_DGRAM,0); 
		/* Get port */
		if (*p>0)
			*port = htons(*p+1);
		else
			*port = htons(0);
		bind(*b,sendAddr,size);
		len = size;
	     /*	getsockname(*b,sendAddr,&len); OLD */
                getsockname(*b,sendAddr,(socklen_t*)&len); /* PORT17.3. fix complier warning */
		*q = ntohs(*port);

	     /*	ast_log(LOG_DEBUG,"-GetUdpPorts [%d,%d]\n",*p,*q); OLD */
	        ast_debug(4,"-GetUdpPorts loop [%d,%d]\n",*p,*q); 
	}

	ast_debug(3,"-GetUdpPorts final [%d,%d]\n",*p,*q); /* ADDED */

	/* Free Address*/
     /*	free(sendAddr); OLD */
	ast_free(sendAddr);
}

static void SetNonBlocking(int fd)
{
	/* Get flags */
	int flags = fcntl(fd,F_GETFD);

	/* Set socket non-blocking */
	fcntl(fd,F_SETFD,flags | O_NONBLOCK);
}

/* 
 * COMMENT: 
 * The following only sets up the socket structure for destination ip address and port.
 * It does not get the ip address.
 */
static struct sockaddr* GetIPAddr(const char *ip, int port,int isIPv6,int *size,int *PF)
{
	struct sockaddr * sendAddr;

	/* If it is ipv6 */
	if (isIPv6)
	{
		/* Set size*/
		*size = sizeof(struct sockaddr_in6);
		/*Create address */
             /*	sendAddr = (struct sockaddr *)malloc(*size); OLD */
		sendAddr = (struct sockaddr *)ast_malloc(*size);
		/* empty addres */
		memset(sendAddr,0,*size);
		/* Set PF */
		*PF = PF_INET6;
		/*Set family */
		((struct sockaddr_in6 *)sendAddr)->sin6_family = AF_INET6;
		/* Set Address */
		inet_pton(AF_INET6,ip,&((struct sockaddr_in6*)sendAddr)->sin6_addr);
		/* Set port */
		((struct sockaddr_in6 *)sendAddr)->sin6_port = htons(port);
	} else {
		/* Set size*/
		*size = sizeof(struct sockaddr_in);
		/*Create address */
             /* sendAddr = (struct sockaddr *)malloc(*size); OLD */
		sendAddr = (struct sockaddr *)ast_malloc(*size);
		/* empty addres */
		memset(sendAddr,0,*size);
		/*Set family */
		((struct sockaddr_in*)sendAddr)->sin_family = AF_INET;
		/* Set PF */
		*PF = PF_INET;
		/* Set Address */
		((struct sockaddr_in*)sendAddr)->sin_addr.s_addr = inet_addr(ip);
		/* Set port */
		((struct sockaddr_in *)sendAddr)->sin_port = htons(port);
	}

	return sendAddr;
}

static int RtspPlayerConnect(struct RtspPlayer *player, const char *ip, int port,int isIPv6, int isUDP) /*ADDED isUDP */
{
	struct sockaddr * sendAddr;
	int size;
	int PF;

        /* ADDED */
	char local_ip[100];
	uint16_t local_port;
	struct sockaddr_in name;
	socklen_t namelen = sizeof(name);
	int err;

	/* Setup struct for Dest address port */
	sendAddr = GetIPAddr(ip,port,isIPv6,&size,&PF);

	/* open Control socket */
        if(isUDP)  /* ADDED. SIP uses UDP. RTSP uses TCP*/
	    player->fd = socket(PF,SOCK_DGRAM,0);
        else
	    player->fd = socket(PF,SOCK_STREAM,0);


	/* Create/Open audio datagram sockets and ports for RTP and RTCP*/
	GetUdpPorts(&player->audioRtp,&player->audioRtcp,&player->audioRtpPort,&player->audioRtcpPort,isIPv6);

	/* Create/Open video datagram sockets and ports for RTP and RTCP*/
	GetUdpPorts(&player->videoRtp,&player->videoRtcp,&player->videoRtpPort,&player->videoRtcpPort,isIPv6);

	/* Set non blocking */
	SetNonBlocking(player->fd);
	SetNonBlocking(player->audioRtp);
	SetNonBlocking(player->audioRtcp);
	SetNonBlocking(player->videoRtp);
	SetNonBlocking(player->videoRtcp);

	/* Connect */
	if (connect(player->fd,sendAddr,size)<0)
	{
		/* Free mem */
             /* free(sendAddr); OLD */
		ast_free(sendAddr);
		/* Exit */
		return 0;
	}

	/* ADDED. Get local IP and source Port in text format for Control Protocol*/
	err = getsockname(player->fd, (struct sockaddr*) &name, &namelen);
	if(err !=0)
		ast_log(LOG_ERROR,"Could not get local IP address\n");

	const char* p = inet_ntop(AF_INET, &name.sin_addr, local_ip, 100);
	if(p ==NULL)
		ast_log(LOG_ERROR,"Could not convert local IP address\n");
	local_port = ntohs(name.sin_port);
	player->local_ctrl_ip = ast_strdup(local_ip);
	player->local_ctrl_port = local_port;
	ast_debug(3,"Local Ctrl IP: %s, Port: %i\n",player->local_ctrl_ip,player->local_ctrl_port);

	/* Set ip v6 */
	player->isIPv6 = isIPv6;

	/* copy ip & port*/
     /*	player->ip 	= strdup(ip); OLD */
	player->ip 	= ast_strdup(ip);
	player->port 	= port;

	/* create hostport */
     /*	player->hostport = (char*)malloc(strlen(ip)+8); OLD */
	player->hostport = (char*)ast_malloc(strlen(ip)+8);

	/* If it is ipv6 */
	if (isIPv6)
		/* In brackets */	
		sprintf(player->hostport,"[%s]",player->ip);
	else
		/* normal*/
		sprintf(player->hostport,"%s:%d",player->ip,player->port);


	/* Free mem */
     /*	free(sendAddr); OLD */
	ast_free(sendAddr);

	/* conected */
	return 1;
}

static int RtspPlayerAddSession(struct RtspPlayer *player,char *session)
{
	int i;
	char *p;

	/* If max sessions reached */
	if (player->numSessions == 2)
		/* Exit */
		return 0;

	/* Check if it has parameters */
	if ((p=strchr(session,';'))>0)
		/* Remove then */
		*p = 0;

	/* Check if we have that session already */
	for (i=0;i<player->numSessions;i++)
		if (strcmp(player->session[i],session)==0)
		{
			/* Free */
	             /*	free(session); OLD */
			ast_free(session);
			/* exit */
			return 0;
		}
	/* Save */
	player->session[player->numSessions++] = session;

	/* exit */
	return player->numSessions;
	

}

static void RrspPlayerSetAudioTransport(struct RtspPlayer *player,const char* transport)
{
	char *i;
	int port;
	struct sockaddr * addr;
	int size;
	int PF;

	/* Find server port values */
	if (!(i=strstr(transport,"server_port=")))
	{
		/* Log */
	     /*	ast_log(LOG_DEBUG,"Not server found in transport [%s]\n",transport); CHANGE */
		ast_log(LOG_WARNING,"No server found in transport [%s]\n",transport);
		/* Exit */
		return;
	}

	/* Get to the rtcp port */
	if (!(i=strstr(i,"-")))
	{
		/* Log */
	     /*	ast_log(LOG_DEBUG,"Not rtcp found in transport  [%s]\n",transport); CHANGE */
		ast_log(LOG_WARNING,"No rtcp found in transport  [%s]\n",transport);
		/* exit */
		return;
	}	

	/* Get port number */
	port = atoi(i+1);

	/* Get send address */
	addr = GetIPAddr(player->ip,port,player->isIPv6,&size,&PF);

	/* Connect */
	if (connect(player->audioRtcp,addr,size)<0)
		/* Log */
	     /*	ast_log(LOG_DEBUG,"Could not connect audio rtcp port [%s,%d,%d].%s\n", player->ip,port,errno,strerror(errno));CHANGE */
		ast_log(LOG_WARNING,"Could not connect audio rtcp port [%s,%d,%d].%s\n", player->ip,port,errno,strerror(errno));

	/* Free Addr */
     /*	free(addr); OLD */
	ast_free(addr);

}

static void RrspPlayerSetVideoTransport(struct RtspPlayer *player,const char* transport)
{
	char *i;
	int rtp_port,rtcp_port;
	struct sockaddr * addr;
	int size;
	int PF;

	/* Find server port values */
	if (!(i=strstr(transport,"server_port=")))
	{
		/* Log */
	     /*	ast_log(LOG_DEBUG,"Not server found in transport [%s]\n",transport); CHANGE */
		ast_log(LOG_WARNING,"No server found in transport [%s]\n",transport);
		/* Exit */
		return;
	}

	/* Get port number */
	rtp_port = atoi(i+12);

	/* Get to the rtcp port */
	if (!(i=strstr(i,"-")))
	{
		/* Log */
	     /*	ast_log(LOG_DEBUG,"Not rtcp found in transport  [%s]\n",transport); CHANGE */
		ast_log(LOG_WARNING,"No rtcp found in transport  [%s]\n",transport);
		/* exit */
		return;
	}	

	/* Get port number */
	rtcp_port = atoi(i+1);

	/* Get send address */
	addr = GetIPAddr(player->ip,rtp_port,player->isIPv6,&size,&PF);

	/* Connect */
	if (connect(player->videoRtp,addr,size)<0)
		/* Log */
		ast_log(LOG_DEBUG,"Could not connect video rtp port [%s,%d,%d].%s\n", player->ip,rtp_port,errno,strerror(errno));

	/* Get send address */
	addr = GetIPAddr(player->ip,rtcp_port,player->isIPv6,&size,&PF);

	/* Connect */
	if (connect(player->videoRtcp,addr,size)<0)
		/* Log */
		ast_log(LOG_DEBUG,"Could not connect video rtcp port [%s,%d,%d].%s\n", player->ip,rtcp_port,errno,strerror(errno));

	/* Free Addr */
     /*	free(addr); OLD */
	ast_free(addr);
}

static void RtspPlayerClose(struct RtspPlayer *player)
{
	/* Close sockets */
	if (player->fd)		close(player->fd);
	if (player->audioRtp)	close(player->audioRtp);
	if (player->audioRtcp)	close(player->audioRtcp);
	if (player->videoRtp)	close(player->videoRtp);
	if (player->videoRtcp)	close(player->videoRtcp);
}

static int SendRequest(int fd,char *request,int *end)
{
	/* Get request len */
	int len = strlen(request);
	/* Send request */
	if (send(fd,request,len,0)==-1)
	{
		/* If failed connection*/
		if (errno!=EAGAIN)
		{
			/* log */
			ast_log(LOG_ERROR,"Error sending request [%d]\n",errno);
			/* End */
			*end = 0;
		}
		/* exit*/
		return 0;
	}
	/* Return length */
	return len;
}

static int RtspPlayerOptions(struct RtspPlayer *player,const char *url)
{

        char request[1024];

        /* Log */
     /* ast_log(LOG_DEBUG,">RTSP OPTIONS [%s]\n",url); OLD */
        ast_debug(1,"<RTSP OPTIONS [%s]\n",url); //changed [v2.0]

        /* Prepare request */
        snprintf(request,1024,
                        "OPTIONS rtsp://%s%s RTSP/1.0\r\n"
                        "CSeq: %d\r\n"
                        "User-Agent: app_rtsp\r\n"
                        "Session: %s\r\n",
                        player->hostport,url,player->cseq,player->session[player->numSessions-1]);

        /* End request */
        strcat(request,"\r\n");

        /* Send request */
        if (!SendRequest(player->fd,request,&player->end))
                /* exit */
                return 0;
	/* Increase player secuence number for request */
        player->cseq++;
	/* Log */
     /* ast_log(LOG_DEBUG,"<RTSP OPTIONS [%s]\n",url); OLD */
      //ast_debug(1,"<RTSP OPTIONS [%s]\n",url); //changed [v2.0]
	ast_debug(3,"\n%s\n",request); //Added [v2.0]
        return 1;
}

static int RtspPlayerDescribe(struct RtspPlayer *player,const char *url)
{

	char request[1024];

	/* Log */
     /*	ast_log(LOG_DEBUG,">DESCRIBE [%s]\n",url); OLD */
        ast_debug(1,"<DESCRIBE [%s]\n",url);

	/* Prepare request */
	snprintf(request,1024,
			"DESCRIBE rtsp://%s%s RTSP/1.0\r\n"
			"CSeq: %d\r\n"
			"Accept: application/sdp\r\n"
			"User-Agent: app_rtsp\r\n",
			player->hostport,url,player->cseq);

	/* If we are authorized */
	if (player->authorization)
	{
		/* Append header */
		strcat(request,player->authorization);
		/* End line */
		strcat(request,"\r\n");
	} 

	/* End request */
	strcat(request,"\r\n");

	/* Send request */
	ast_debug(3,"\n%s\n",request); //Added [v2.0]
	if (!SendRequest(player->fd,request,&player->end))
		/* exit */
		return 0;
	/* Save url */
     /*	player->url = strdup(url); OLD */
	player->url = ast_strdup(url);
	/* Set state */
	player->state = RTSP_DESCRIBE;
	/* Increase seq */
	player->cseq++;
     /*	ast_log(LOG_DEBUG,"<DESCRIBE [%s]\n",url); OLD */
      //ast_debug(1,"<DESCRIBE [%s]\n",url); //changed [v2.0]
	/* ok */
	return 1;
}


/* [17.x NEW] For SIP */ 
static void SipSpeakerSetAudioTransport(struct RtspPlayer *player, int dst_port)
{
	/* Setup struct for Dest address port */
	struct sockaddr * sendAddr;
	int size;
	int PF;

	/* Get send address */
	sendAddr = GetIPAddr(player->ip,dst_port,player->isIPv6,&size,&PF);

	/* Connect */
	if (connect(player->audioRtp,sendAddr,size)<0)
		ast_log(LOG_DEBUG,"Could not connect SIP audio rtp port [%s,%d,%d].%s\n", \
			player->ip,dst_port,errno,strerror(errno));

	ast_free(sendAddr); 

}

/* [17.x NEW] For SIP */ 
static int SipSpeakerOptions(struct RtspPlayer *player, char *username)
{
	char request[1024];
	int temp;

	/* Log */
     /*	ast_log(LOG_DEBUG,">SIP OPTIONS [%s]\n",username); OLD */
	ast_debug(1,"<SIP OPTIONS [%s]\n",username); //changed [v2.0]

	if (!player->in_a_dialog)
	{
		generateSrcTag(player); /* Set SIP source Tag outside a dialog */
		generateCallId(player); /* Set SIP Call ID outside a dialog*/
	}
	/* generate a new branch (correlation tag) across space/time for all new requests */
	generateBranch(player);

	/* Prepare request */
	snprintf(request,1024,
			"OPTIONS sip:%s@%s:%i SIP/2.0\r\n"
			"To: sip:%s@%s:%i\r\n"
			"From: <sip:%s@%s>;tag=%s\r\n"
			"Via: SIP/2.0/UDP %s:%i;branch=%s;rport\r\n"
			"Call-ID: %s\r\n"
			"Contact: sip:%s@%s:%i\r\n"
			"CSeq: %d OPTIONS\r\n"
			"Max-Forwards: 70\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: 0\r\n",
			username,player->ip,player->port, 		                  /* OPTIONS */
			username,player->ip,player->port,            	                  /* To:     */
			MY_NAME,player->local_ctrl_ip,player->src_tag,                    /* From:   */
			player->local_ctrl_ip,player->local_ctrl_port,player->branch_id,  /* Via:    */
			player->call_id,					          /* CALL-ID */
			MY_NAME,player->local_ctrl_ip,player->local_ctrl_port,            /* Contact:*/
			player->cseq);

	strcat(request,"\r\n");

	/* Send request.  Bypass player->end using temp. */
	if (!SendRequest(player->fd,request,&temp))
		/* exit */
		return 0;

	/* Set state */
	player->state = SIP_STATE_OPTIONS;
	/* Increase seq */
	player->cseqm[OPTIONS]++;
        
	ast_debug(3,"\n%s",request); //changed [v2.0]

     /*	ast_log(LOG_DEBUG,"<SIP OPTIONS [%s]\n",username); OLD */
      //ast_debug(1,"<SIP OPTIONS [%s]\n",username); //changed [v2.0]

	return 1;
}

/* [17.x NEW] For SIP */ 
static int SipSpeakerInvite(struct RtspPlayer *player, char *username, int audioFormat,int retry)
{
	char request[1024];
	char sdp[512];
	int  req_string_len = 0;
	int  sdp_string_len = 0;
	int temp;
	int  rtp_pt, rtp_bw;
	char rtp_pt_name[16];

	/* Log */
	ast_debug(1,"<SIP INVITE [%s]\n",username); //changed [v2.0]

	/* Message Body: SDP . Do this first to compute Content-Length.*/
	switch (audioFormat)
	{
		case AST_FORMAT_ULAW:
			rtp_pt = 0;
			strcpy(rtp_pt_name,"PCMU/8000");
			rtp_bw = 64;
			break;
		case AST_FORMAT_ALAW:
			rtp_pt = 8;
			strcpy(rtp_pt_name,"PCMA/8000");
			rtp_bw = 64;
			break;
		default:
	     	     /* ast_log(LOG_ERROR,"SIP does not support audio Format %lli\n", mimeTypes[audioFormat].format); */
			ast_log(LOG_ERROR,"SIP does not support audio Format %"PRIu64"\n", mimeTypes[audioFormat].format); /* PORT 17.5. Proper way to print */
			return -1;
	}
	generateSessionId(player);
	sdp_string_len = snprintf(sdp,512,
			"v=0\r\n"
			"o=SIP %s 424 IN IP4 %s\r\n" /* <sessionid> <version> (fixed at 424), <netType> <addrType> <addr> */
			"s=SIPUA\r\n"
			"c=IN IP4 %s\r\n"
			"t=0 0\r\n"
			"m=audio %i RTP/AVP %d\r\n"
			"b=AS:%i\r\n"
			"a=rtpmap:%d %s\r\n"
			"a=sendonly\r\n",
			player->session_id,player->local_ctrl_ip, /*o=       */
			player->local_ctrl_ip,                    /*c=       */
			player->audioRtpPort,rtp_pt,              /*m=audio  */
			rtp_bw,                                   /*b=       */
			rtp_pt,rtp_pt_name);                      /*a=rtpmap */

	/* Start Message Header */
	if (!player->in_a_dialog && !retry) 
	{
		/* Set new source Tag if outside a dialog and not part of retry (same From:) Sect8.1.3.5) */
		generateSrcTag(player); 

		/* Set new Call ID if outside a dialog (8.1.1.4) and not part of a retry (Same CallID 8.1.3.5) */
		generateCallId(player); 
	}

	/* generate a new branch (correlation tag) across space/time for all new requests */
	generateBranch(player);

	/* Prepare SIP request */
	req_string_len = snprintf(request,1024,
			"INVITE sip:%s@%s:%i SIP/2.0\r\n"
			"To: <sip:%s@%s:%i>\r\n"
			"From: <sip:%s@%s>;tag=%s\r\n"
			"Via: SIP/2.0/UDP %s:%i;branch=%s;rport\r\n"
			"Call-ID: %s\r\n"
			"Contact: sip:%s@%s:%i\r\n",
			username,player->ip,player->port,                                /* INVITE   */
			username,player->ip,player->port,                                /* To:      */
			MY_NAME,player->local_ctrl_ip,player->src_tag,                   /* From:    */
			player->local_ctrl_ip,player->local_ctrl_port,player->branch_id, /* Via:     */
			player->call_id,                                                 /* Call-ID: */
			MY_NAME,player->local_ctrl_ip,player->local_ctrl_port);          /* Contact: */

	/* If we are authorized */
	if (player->authorization)
	{
		/* Append header */
		req_string_len += sprintf(request+req_string_len,"%s\r\n",player->authorization);
	}
	/* Add other headers */ 
	req_string_len += sprintf(request+req_string_len,"CSeq: %d INVITE\r\n",player->cseqm[INVITE]);
	req_string_len += sprintf(request+req_string_len,"Allow: INVITE, ACK, CANCEL, OPTIONS, BYE, INFO\r\n");
	req_string_len += sprintf(request+req_string_len,"Max-Forwards: 70\r\n");
	req_string_len += sprintf(request+req_string_len,"Content-Type: application/sdp\r\n");
	req_string_len += sprintf(request+req_string_len,"Content-Length: %d\r\n",sdp_string_len);

	/* End Message Header */
	req_string_len += sprintf(request+req_string_len,"\r\n");
	
	/* Message Header + Body */
	sprintf(request+req_string_len,"%s",sdp);

	ast_debug(3,"\n%s",request);

	/* Send request.  Bypass player->end using temp. */
	if (!SendRequest(player->fd,request,&temp))
		/* exit */
		return 0;

	/* Set state */
	player->state = SIP_STATE_INVITE;
	/* Increase seq */
	player->cseqm[INVITE]++;

 
      //ast_debug(1,"<SIP INVITE [%s]\n",username); //changed [v2.0]

	/* ok */
	return 1;
}

static int SipSpeakerAck(struct RtspPlayer *player, char *username, int response_type)
{ 	/* RFC3261 
   	 * Sect 17.1.1.3 For final responses between 300 and 699 
   	 * Sect 13.2.2.4 For 2xx responses
         */

	/* From:    ACK: Same as original request (17.1.1.3). i.e. src tag stays the same
	 * To:      ACK: Same as response (17.1.1.3). Response Handler code sets the peer tag.
	 * Via:     ACK: Same as original request (17.1.1.3). Branch stays the same.
	 * Call-ID: Same for all requests in dialog (8.1.1.4)
	 *          ACK: Same as original request (17.1.1.3) (even outside a dialog), 
	 *               Ex 17.1.1.3 shows same Call-ID 
	 */
        
	int temp;
	char request[1024];

	ast_debug(1,"<SIP ACK [%s]\n",username); //changed [v2.0]

	if(response_type == 2)
	{
		/* Sect 13.2.2.4 
		 * CSeq: ACKing Invite- Must be that of the INVITE being ACK'd. (Let invite code set correctly)
		 * Auth: ACKing Invite- Must have same credentials as the INVITE. 
		 * Offer: If received and not acceptable, generate a valid answer.
		 * Sec 8.1.1.x
		 * Call-ID: Same for all requests in dialog
		 * Branch: Unique across space and time for 2xx response. (8.1.1.7) Contradicts 17.1.1.3.
 		*/
		ast_debug(3,"prepare sip ack for response 2xx\n");
		generateBranch(player);
	}
	else
	{
		/*
		 * Sec 8.1.1.7
		 * Branch: ACK for a non-2xx will have the same Branch-ID as the INVITE response being ACK'd
		 */
		ast_debug(3,"prepare sip ack for response 3xx to 6xx\n");
	} 
	snprintf(request,1024,
			"ACK sip:%s@%s:%i SIP/2.0\r\n"
			"To: <sip:%s@%s:%i>;tag=%s\r\n"
			"From: <sip:%s@%s>;tag=%s\r\n"
			"Via: SIP/2.0/UDP %s:%i;branch=%s;rport\r\n"
			"Call-ID: %s\r\n"
			"CSeq: %d ACK\r\n"
			"Max-Forwards: 70\r\n"
			"Content-Length: 0\r\n",
			username,player->ip,player->port,                                /* ACK   */
			username,player->ip,player->port,player->peer_tag,               /* To:      */
			MY_NAME,player->local_ctrl_ip,player->src_tag,                   /* From:    */
			player->local_ctrl_ip,player->local_ctrl_port,player->branch_id, /* Via:     */
			player->call_id,                                                 /* Call-ID: */
			player->cseqm[ACK] );                                            /* CSeq:    */
        
	/* End request */
	strcat(request,"\r\n");
	ast_debug(3,"\n%s",request); //changed [v2.0]

	/* Send request.  Bypass player->end using temp. */
	if (!SendRequest(player->fd,request,&temp))
		/* exit */
		return 0;

	/* Log */
      //ast_debug(1,"<SIP ACK [%s]\n",username); //changed [v2.0]

	return 1;
}

/* [17.x NEW]. SIP */
static int SipSpeakerBye(struct RtspPlayer *player, char *username )
{
	int temp;
	char request[1024];
	int  req_string_len = 0;

	ast_debug(1,"<SIP BYE [%s]\n",username); //changed [v2.0]

	if (!player->in_a_dialog) { /* If not in a dialog, no need to send a BYE */
              //ast_log(LOG_DEBUG,">SIP BYE [%s]\n",username); //changed v2.0
	        ast_debug(3,"not in a dialog\n"); //added [v2.0]
		return 0;
	}
	/* generate a new branch (correlation tag) across space/time for all new requests */
	generateBranch(player);

	/* Prepare SIP request */
	req_string_len = snprintf(request,1024,
			"BYE sip:%s@%s:%i SIP/2.0\r\n"
			"To: <sip:%s@%s:%i>;tag=%s\r\n"
			"From: <sip:%s@%s>;tag=%s\r\n"
			"Via: SIP/2.0/UDP %s:%i;branch=%s;rport\r\n"
			"Call-ID: %s\r\n",
			username,player->ip,player->port,                                /* BYE      */
			username,player->ip,player->port,player->peer_tag,               /* To:      */
			MY_NAME,player->local_ctrl_ip,player->src_tag,                   /* From:    */
			player->local_ctrl_ip,player->local_ctrl_port,player->branch_id, /* Via:     */
			player->call_id);                                                /* Call-ID: */
	/* Add other headers */
	req_string_len += sprintf(request+req_string_len,"CSeq: %d BYE\r\n",player->cseqm[BYE]);
	req_string_len += sprintf(request+req_string_len,"Max-Forwards: 70\r\n");
	if (player->authorization) /* If we are authorized */
		req_string_len += sprintf(request+req_string_len,"%s\r\n",player->authorization);
	req_string_len += sprintf(request+req_string_len,"Content-Length: 0\r\n");

	/* End Message Header */
	req_string_len += sprintf(request+req_string_len,"\r\n");

	ast_debug(3,"\n%s",request); //added [v2.0]

	/* Send request.  Bypass player->end using temp. */
	if (!SendRequest(player->fd,request,&temp))
		return 0;

      //ast_debug(1,"<SIP BYE [%s]\n",username); //changed [v2.0]
	return 1;
}

static int RtspPlayerSetupAudio(struct RtspPlayer* player,const char *url)
{
	char request[1024];
	char sessionheader[256];

	/* Log */
     /*	ast_log(LOG_DEBUG,"-SETUP AUDIO [%s]\n",url); OLD */
	ast_debug(1,"<RTSP SETUP for audio [%s]\n",url); //added [v2.0]

	/* if it got session */
	if (player->numSessions)
		/* Create header */
		snprintf(sessionheader,256,"Session: %s\r\n",player->session[player->numSessions-1]);
	else
		/* no header */
		sessionheader[0] = 0;

	/* If it's absolute */
	if (strncmp(url,"rtsp://",7)==0)
	{
		/* Prepare request */
		snprintf(request,1024,
				"SETUP %s RTSP/1.0\r\n"
				"Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n"
				"CSeq: %d\r\n"
				"User-Agent: app_rtsp\r\n"
				"%s",
				url,player->audioRtpPort,player->audioRtcpPort,player->cseq,sessionheader);
	} else {
		/* Prepare request */
		snprintf(request,1024,
				"SETUP rtsp://%s%s/%s RTSP/1.0\r\n"
				"Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n"
				"CSeq: %d\r\n"
				"User-Agent: app_rtsp\r\n"
				"%s",
				player->hostport,player->url,url,player->audioRtpPort,player->audioRtcpPort,player->cseq,sessionheader);
	}

	/* If we are authorized */
	if (player->authorization)
	{
		/* Append header */
		strcat(request,player->authorization);
		/* End line */
		strcat(request,"\r\n");
	}
	/* End request */
	strcat(request,"\r\n");

	/* Send request */
	ast_debug(3,"\n%s\n",request); //Added [v2.0]
	if (!SendRequest(player->fd,request,&player->end))
		/* exit */
		return 0;
	/* Set state */
	player->state = RTSP_SETUP_AUDIO;
	/* Increase seq */
	player->cseq++;
	/* ok */
	return 1;
}

static int RtspPlayerSetupVideo(struct RtspPlayer* player,const char *url)
{
	char request[1024];
	char sessionheader[256];

	/* Log */
	ast_log(LOG_DEBUG,"-SETUP VIDEO [%s]\n",url);

	/* if it got session */
	if (player->numSessions)
		/* Create header */
		snprintf(sessionheader,256,"Session: %s\r\n",player->session[player->numSessions-1]);
	else
		/* no header */
		sessionheader[0] = 0;

	/* If it's absolute */
	if (strncmp(url,"rtsp://",7)==0)
	{
		/* Prepare request */
		snprintf(request,1024,
				"SETUP %s RTSP/1.0\r\n"
				"Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n"
				"CSeq: %d\r\n"
				"User-Agent: app_rtsp\r\n"
				"%s",
				url,player->videoRtpPort,player->videoRtcpPort,player->cseq,sessionheader);
	} else {
		/* Prepare request */
		snprintf(request,1024,
				"SETUP rtsp://%s%s/%s RTSP/1.0\r\n"
				"Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n"
				"CSeq: %d\r\n"
				"User-Agent: app_rtsp\r\n"
				"%s",
				player->hostport,player->url,url,player->videoRtpPort,player->videoRtcpPort,player->cseq,sessionheader);
	}

	/* If we are authorized */
	if (player->authorization)
	{
		/* Append header */
		strcat(request,player->authorization);
		/* End line */
		strcat(request,"\r\n");
	}
	/* End request */
	strcat(request,"\r\n");

	/* Send request */
	if (!SendRequest(player->fd,request,&player->end))
		/* exit */
		return 0;
	/* Set state */
	player->state = RTSP_SETUP_VIDEO;
	/* Increase seq */
	player->cseq++;
	/* ok */
	return 1;
}

static int RtspPlayerPlay(struct RtspPlayer* player)
{
	char request[1024];
	int i;

	/* Log */
     /*	ast_log(LOG_DEBUG,"-PLAY [%s]\n",player->url); OLD */
	ast_debug(1,"<RTSP PLAY for audio [%s]\n",player->url); //added [v2.0]

	/* if not session */
	if (!player->numSessions)
		/* exit*/
		return 0;

	/* For each request pipeline */
	for (i=0;i<player->numSessions;i++)
	{
		/* Prepare request */
		snprintf(request,1024,
				"PLAY rtsp://%s%s RTSP/1.0\r\n"
				"CSeq: %d\r\n"
				"User-Agent: app_rtsp\r\n"
				"Session: %s\r\n",
				player->hostport,player->url,player->cseq,player->session[i]);

		/* If we are authorized */
		if (player->authorization)
		{
			/* Append header */
			strcat(request,player->authorization);
			/* End line */
			strcat(request,"\r\n");
		}
		/* End request */
		strcat(request,"\r\n");

		/* Send request */
	        ast_debug(3,"\n%s\n",request); //Added [v2.0]
		if (!SendRequest(player->fd,request,&player->end))
			/* exit */
			return 0;
		/* Increase seq */
		player->cseq++;
	}
	/* Set state */
	player->state = RTSP_PLAY;
	/* ok */
	return 1;
}

static int RtspPlayerTeardown(struct RtspPlayer* player)
{
	char request[1024];
	int i;

	/* Log */
     /*	ast_log(LOG_DEBUG,"-TEARDOWN\n"); OLD */
	ast_debug(1,"<RTSP TEARDOWN\n"); //changed [v2.0]

	/* if not session */
	if (!player->numSessions)
		/* exit*/
		return 0;

	/* For each request pipeline */
	for (i=0;i<player->numSessions;i++)
	{
		/* Prepare request */
		snprintf(request,1024,
				"TEARDOWN rtsp://%s%s RTSP/1.0\r\n"
				"CSeq: %d\r\n"
				"User-Agent: app_rtsp\r\n"
				"Session: %s\r\n",
				player->hostport,player->url,player->cseq,player->session[i]);

		/* If we are authorized */
		if (player->authorization)
		{
			/* Append header */
			strcat(request,player->authorization);
			/* End line */
			strcat(request,"\r\n");
		}
		/* End request */
		strcat(request,"\r\n");
		/* Send request */
		ast_debug(3, "\n%s\n",request); //Added [v2.0]
		if (!SendRequest(player->fd,request,&player->end))
			/* exit */
			return 0;
		/* Increase seq */
		player->cseq++;
	}
	/* Set state */
	player->state = RTSP_RELEASED;
	/* ok */
	return 1;
}

#define RTSP_TUNNEL_CONNECTING 	0
#define RTSP_TUNNEL_NEGOTIATION 1
#define RTSP_TUNNEL_RTP 	2


struct SDPFormat
{
	int 			payload;
     /*	int			format;	OLD */
	uint64_t		format;		/* PORT 17.3 bit of AST_FORMAT_xxx is ULL */
        struct ast_format	*new_format; 	/* PORT 17.3 add new format  */
	char*			control;
};

struct SDPMedia
{
	struct SDPFormat** formats;
	int 		   num;
     /*	int 		   all; OLD */
	uint64_t 	   all; 		/* PORT 17.3 bit list of AST_FORMAT_xxx is ULL */
	uint16_t	   peer_media_port; 	/* [17.x NEW]. SIP Peers tcp/udp port for receiving media */
};

struct SDPContent
{
	struct SDPMedia* audio;
	struct SDPMedia* video;
};

static struct SDPMedia* CreateMedia(char *buffer,int bufferLen)
{
	int num = 0;
	int i = 0;
	struct SDPMedia* media = NULL;;

	/* Count number of spaces*/
	for (i=0;i<bufferLen;i++)
		/* If it's a withespace */
		if (buffer[i]==' ')
			/* Another one */
			num++;

	/* if no media */
	if (num<3)
		/* Exit */
		return NULL;

	/* Allocate */
     /* media = (struct SDPMedia*) malloc(sizeof(struct SDPMedia)); OLD */
	media = (struct SDPMedia*) ast_malloc(sizeof(struct SDPMedia));

	/* Get number of formats */
	media->num = num - 2; 

	/* Allocate */
     /*	media->formats = (struct SDPFormat**) malloc(media->num); OLD */
     /*	media->formats = (struct SDPFormat**) ast_malloc(media->num);  Alloc the array would only alloc num bytes */
	media->formats = (struct SDPFormat**) ast_malloc(sizeof(struct SDPFormat**) * media->num); /* Alloc the array */

	/* Set all formats to nothing */
	media->all = 0;

	/* ADDED. SIP. Set peer media tcp/udp port to nothing */
	media->peer_media_port = 0;


	/* For each format */
	for (i=0;i<media->num;i++)
	{
		/* Allocate format */
	     /*	media->formats[i] = (struct SDPFormat*) malloc(media->num); OLD. BUG TOO */
	     /*	media->formats[i] = (struct SDPFormat*) ast_malloc(sizeof(struct SDPFormat)); PORT 17.3 */
		media->formats[i] = (struct SDPFormat*) ast_malloc(sizeof(*(media->formats[i]))); /*PORT 17.5 */
		/* Init params */
		media->formats[i]->payload 	= -1;
		media->formats[i]->format 	= 0;
		media->formats[i]->control 	= NULL;
	}

	/* log */
     /*	ast_log(LOG_DEBUG,"-creating media [%d,%s]\n",media->num,strndup(buffer,bufferLen)); OLD */
	ast_debug(2,"-creating media [%d,%s]\n",media->num,strndupa(buffer,bufferLen)); /* PORT 17.3 Avoid mem leak;use strndupa instead */

	/* Return media */
	return media;
}

static void DestroyMedia(struct SDPMedia* media)
{
	int i = 0;

	/* Free format */
	for (i=0;i<media->num;i++)
	{
		/* Free control */
		if (media->formats[i]->control) 
	             /*	free(media->formats[i]->control); OLD */
			ast_free(media->formats[i]->control);
		/* Free format*/
	     /*	free(media->formats[i]); OLD */
		ast_free(media->formats[i]);
	}
	/* Free format */
     /*	free(media->formats); OLD */
	ast_free(media->formats);
	/* Free media */
     /* free(media); OLD */
	ast_free(media);
}

static struct SDPContent* CreateSDP(char *buffer,int bufferLen, int sip_enable)
{
	struct SDPContent* sdp = NULL;
	struct SDPMedia* media = NULL;;
	char *i = buffer;
	char *j = NULL;
	char *k = NULL; /* New. SIP */
	char *ini;
	char *end;
	int n = 0;
	int f = 0;

	ast_debug(4,"SDPContent bufferLen %i buffer:\n%s",bufferLen, buffer); /* ADDED */

	/* Malloc */
     /*	sdp = (struct SDPContent*) malloc(sizeof(struct SDPContent)); OLD */
	sdp = (struct SDPContent*) ast_malloc(sizeof(struct SDPContent));

	/* NO audio and video */
	sdp->audio = NULL;
	sdp->video = NULL;

	/* Read each line */
     /*	while ( (j=strstr(i,"\n")) != NULL && (j<buffer+bufferLen))  PORT 17.3. Picked up from port to 11.x.x */
	while ( (j=strstr(i,"\n")) != NULL) 
	{
		/* if it's not enougth data */
		if (j-i<=1)
			goto next;

		/* If previous is a \r" */
		if (j[-1]=='\r')
			/* Decrease end */
			j--;

		/* log */
	     /*	ast_log(LOG_DEBUG,"-line [%s]\n",strndup(i,j-i)); OLD */
		ast_debug(3,"-line [%s]\n",strndupa(i,j-i)); /* Avoid mem leak;use strndupa instead */

		/* Check header */
		if (strncmp(i,"m=",2)==0) 
		{
			/* media */
			if (strncmp(i+2,"video",5)==0)
			{
				/* create video */
				sdp->video = CreateMedia(i,j-i);
				/* set current media */
				media = sdp->video;
			} else if (strncmp(i+2,"audio",5)==0) {
				/* create audio */
				sdp->audio = CreateMedia(i,j-i);
				/* set current media */
				media = sdp->audio;
				/* ADDED. SIP Get the Peer's tcp/udp port. RFC 2327 p20
				 * Ex. m=audio 49170/2 RTP/AVP 31. 49170 is the port. /2 or /(anything) is not supported
				 * Only parse peer port when SIP is enabled since it's only used for SIP functionality
				 */
				if (sip_enable) {
					sdp->audio->peer_media_port = (uint16_t) strtol(i+8, &k, 10);
					if(sdp->audio->peer_media_port == 0)
						ast_log(LOG_WARNING,"    peer rtp port is not provided\n");
					else{
						ast_debug(3,"      peer rtp port: %i\n",sdp->audio->peer_media_port);
						if (strncmp(k-1,"RTP",3)==0) {
							ast_log(LOG_ERROR,"Peer RTP transport is not RTP\n");
							sdp->audio->peer_media_port = 0;
						}
					}
				}
			} else 
				/* no media */
				media = NULL;
			/* reset formats */
			n = 0;
		} else if (strncmp(i,"a=rtpmap:",9)==0){
			/* if not in media */
			if (!media)
				goto next;
			/* If more than formats */
			if (n==media->num)
				goto next;
			/* get ini */
			for (ini=i;ini<j;ini++)
				/* if it's a space */
				if (*ini==' ')
					break;
			/* skip space*/
			if (++ini>=j)
				goto next;
			/* get end */
			for (end=ini;end<j;end++)
				/* if it's a space */
				if (*end=='/')
					break;
			/* Check formats */
			for (f = 0; f < sizeof(mimeTypes)/sizeof(mimeTypes[0]); ++f) 
				/* If the string is in it */
				if (strncasecmp(ini,mimeTypes[f].name,end-ini) == 0) 
				{
					/* Set type */
					media->formats[n]->format = mimeTypes[f].format;
					/* PORT 17.3 add new Asterisk format.
					 * bifield2format() takes arg something like AST_FORMAT_ULAW
					 * and returns ast_format *ast_format_ulaw 
					 */

					media->formats[n]->new_format = ast_format_compatibility_bitfield2format( mimeTypes[f].format);
					if(media->formats[n]->new_format == NULL) /*ADDED */
						ast_debug(3,"      added new format to list is NULL\n"); /* ADDED  */
					else
					     /* ast_debug(3,"      added format %llx to list \n",mimeTypes[f].format);  ADDED  */
						ast_debug(3,"      added format %"PRIx64" to list \n",mimeTypes[f].format); /* PORT17.5 Proper way to print */

					/* Set payload */
					media->formats[n]->payload = atoi(i+9);
					/* Append to all formats */
					media->all |= media->formats[n]->format;
					/* Inc medias */
					n++;
					/* Exit */
					//break;
				}
			
		} else if (strncmp(i,"a=control:",10)==0){
			/* if not in media */
			if (!media)
				goto next;
			/* If more than formats */
			if (n>media->num)
				goto next;
			/* If it's previous to the ftmp */
			if (n==0)
			{
				/* Set control for all */
				for ( f=0; f<media->num; f++)
					/* Set */
				     /*	media->formats[f]->control = strndup(i+10,j-i-10); OLD */
					media->formats[f]->control = ast_strndup(i+10,j-i-10);
			} else  {
				/* if already had control */
				if (media->formats[n-1]->control)
					/* Free it */
			             /*	free(media->formats[n-1]->control); OLD. BUG TOO*/
					ast_free(media->formats[n-1]);
				/* Get new control */
			     /*	media->formats[n-1]->control = strndup(i+10,j-i-10); OLD */
			 	media->formats[n-1]->control = ast_strndup(i+10,j-i-10);
			}
		}
next:
		/* if it's a \r */
		if (j[0]=='\r')
			/* skip \r\n to next line */
			i = j+2;
		else
			/* skip \n to next line */
			i = j+1;
	}

	/* Return sdp */
	return sdp;
}

static void DestroySDP(struct SDPContent* sdp)
{
	/* Free medias */
	if (sdp->audio) DestroyMedia(sdp->audio);
	if (sdp->video) DestroyMedia(sdp->video);
	/* Free */
     /* free(sdp); OLD */
	ast_free(sdp);
}


static int HasHeader(char *buffer,int bufferLen,char *header)
{
	int len;
	char *i;

	/* Get length */
	len = strlen(header);

	/* If no header*/
	if (!len)
		/* Exit */
		return 0;

	/* Get Header */
	i = strcasestr(buffer,header);

	/* If not found or not \r\n first*/
	if (i<buffer+2)
		/* Exit */
		return 0;

	/* If it's not in this request */
	if (i-buffer>bufferLen)
		/* Exit */
		return 0;

	/* Check for \r\n */
	if (i[-2]!='\r' || i[-1]!='\n')
		/* Exit */
		return 0;

	/* Check for ": " */
	if (i[len]!=':' || i[len+1]!=' ')
		/* Exit */
		return 0;

	/* Return value start */
	return (i-buffer)+len+2;
}

static int GetResponseCode(char *buffer,int bufferLen, int isSIP) /* ADDED. Differentiate RTSP vs. SIP */
{
	/* check length */
	if (bufferLen<12)
		return -1;
	if (isSIP)
		return atoi(buffer+8);
	else
		return atoi(buffer+9);
}

static int GetHeaderValueInt(char *buffer,int bufferLen,char *header)
{
	int i;	 

	/* Get start */
	if (!(i=HasHeader(buffer,bufferLen,header)))
		/* Exit */
		return 0;

	/* Return value */
	return atoi(buffer+i);
}

#if 0 /* PORT 17.3 compiler warning about not being used so remove this. */
static long GetHeaderValueLong(char *buffer,int bufferLen,char *header)
{
	int i;	 

	/* Get start */
	if (!(i=HasHeader(buffer,bufferLen,header)))
		/* Exit */
		return 0;

	/* Return value */
	return atol(buffer+i);
}
#endif

static char* GetHeaderValue(char *buffer,int bufferLen,char *header)
{
	int i;	 
	char *j;

	/* Get start */
	if (!(i=HasHeader(buffer,bufferLen,header)))
		/* Exit */
		return 0;
	/* Get end */
	if (!(j=strstr(buffer+i,"\r\n")))
		/* Exit */
		return 0;

	/* Return value */
     /*	return strndup(buffer+i,(j-buffer)-i); OLD */
	return ast_strndup(buffer+i,(j-buffer)-i);
}

static int CheckHeaderValue(char *buffer,int bufferLen,char *header,char*value)
{
	int i;	 

    /*	ast_debug(6,"Looking for value %s in Header %s bufferLen %i in buffer:\n%s\n",value,header,bufferLen,buffer);  DEBUG */
	/* Get start */
	if (!(i=HasHeader(buffer,bufferLen,header)))
	{
		ast_debug(4,"No Header Found! \n"); /* ADDED */
		return 0; /* Exit */
	}
	/* Return value */
	return (strncasecmp(buffer+i,value,strlen(value))==0);
}

#ifdef OLD_AUTH_SCHEME
/* [17.x NEW] for Digest Authentication */
static int GetAuthHeaderData(struct RtspPlayer *player, char *buffer,int bufferLen ,struct DigestAuthData *digest_data)
{
	char *www_header;
	char *i,*j;

	if ( (www_header=GetHeaderValue(buffer,bufferLen,"WWW-Authenticate")) == 0) /* non-0 returns alloc'd memory */
	{
		ast_debug(3,"Could not find any data in header WWW-Authenticate:\n");
		return -1;
	}
	else
	{
		ast_debug(3,"WWW-Authenticate: %s\n",www_header);

		/* Get "realm" value within WWW-Authenticate: header. Note value will be in quotes */
		if (!(i=strstr(www_header,"realm=")))
			ast_debug(3,"Could not find realm= in WWW-Authentication header\n");
		else
		{
			i+=7;/* +7 advances beyond 'realm="' as value starts after first quote */
			j=i;
			while(j[0] != '\0'){ /* go to end of realm value */
				if(*j == ','|| *j == '\r')
					break;
				digest_data->rx_realm[j-i] = *j; 
				j++;
			}
                        /* [v1.1] Asterisk compiler complains when -1 is rightmost in an array */
                        //digest_data->rx_realm[j-i-1]='\0'; /* rewind 1; change last '"' to a termination */      
                        digest_data->rx_realm[j-1-i]='\0'; /* rewind 1; change last '"' to a termination */

			ast_debug(3,"-rx_realm=%s\n",digest_data->rx_realm);
		}
		/* Get "nonce=" value within WWW-Authenticate: header. Note: value will be in quotes*/
		if (!(i=strstr(www_header,"nonce=")))
			ast_debug(3,"Could not find nonce= in WWW-Authentication header\n");
		else
		{
			i+=7;/* +7 advances beyond 'nonce="' as value starts after first quote */
			j=i;
			while(j[0] != '\0'){ /* go to end of nonce value */
				if(*j == ','|| *j == '\r')
					break;
				digest_data->nonce[j-i] = *j; 
				j++;
			}
			digest_data->nonce[j-i-1]='\0'; /* rewind 1; change last '"' to a termination */
			ast_debug(3,"-nonce=%s\n",digest_data->nonce);
		}
 
		ast_free(www_header);
	}

	return 1;
}
#endif

/* [17.x NEW] For SIP */ 
static int SipSetPeerTag(struct RtspPlayer *player, char *buffer,int bufferLen)
{
	char *to_header;
	char *i,*j;

	if (!player->in_a_dialog)
	{
		/* update peer Tag from the received To: header */
		if ( (to_header=GetHeaderValue(buffer,bufferLen,"To")) == 0) /* non-0 returns alloc'd memory */
			ast_debug(3,"Could not find To: header\n");
		else
		{
			/* Find tag value within To: header*/
			if (!(i=strstr(to_header,"tag=")))
				ast_debug(3,"Could not find tag= in To: header [%s]\n",to_header);
			else
			{   /* get tag value */
				i+=4;/* +4 advances beyond 'tag=' */
				j=i;
				while(j[0] != '\0') /* go to end of tag value */
				{
					if(j[0] == ' '|| j[0] == '\r')
						break;
					player->peer_tag[j-i] = j[0]; 
					j++;
				}
				player->peer_tag[j-i]='\0'; /* change last ' ' or '\r' to a termination */
				ast_debug(3,"tag=%s\n",player->peer_tag);
			}
			ast_free(to_header);
		}
	}
	return 1;
}

/* [17.x NEW] SIP */
static int SipSpeakerReply(struct RtspPlayer *player, char *buffer, int bufferLen,\
                          char *username, const char *peer_ip, int peer_port, char *request)
{ 
	/* RFC3261 */
	char reply[1024];
	int  reply_string_len = 0;
	char *tmp_header;
	char *param_front,*param_back;
	int  param_count=0;
	int  temp;
	int  something2send=0;

	ast_debug(1,">SIP Reply [%s]\n",username);

	/* RFC3261 Sect 15.1.2: Check if in a dialog 
	 *   Note: Technically suppose to check against matching 
	 *   tags and Call-ID but will skip that.
	 */
	if (player->in_a_dialog)
	{
		/* Prepare SIP 200 reply */
		reply_string_len = snprintf(reply,1024, "SIP/2.0 200 OK\r\n");

		/* RFC 3261 Section 8.2.6.2 The following reply headers must match that of the request:
		 *     From, Call-ID and CSeq headers 
		 *     Via header values (and preserve the order received)
		 *     To header (If tag present, otherwise match URI and add tag) 
		 * p219 has an example.
 		 */
		if ( (tmp_header=GetHeaderValue(buffer,bufferLen,"To")) == 0){ /* non-0 returns alloc'd memory */
			ast_debug(3,"Could not find To: header\n");
		}
		else {
			ast_debug(3,"-To: header %s\n",tmp_header);
			reply_string_len += sprintf(reply+reply_string_len,"To: %s\r\n",tmp_header);
			ast_free(tmp_header);
		}

		if ( (tmp_header=GetHeaderValue(buffer,bufferLen,"From")) == 0){ /* non-0 returns alloc'd memory */
			ast_debug(3,"Could not find From: header\n");
		}
		else {
			ast_debug(3,"-From: header %s\n",tmp_header);
			reply_string_len += sprintf(reply+reply_string_len,"From: %s\r\n",tmp_header);
			ast_free(tmp_header);
		}

		if ( (tmp_header=GetHeaderValue(buffer,bufferLen,"Via")) == 0){ /* non-0 returns alloc'd memory */
			ast_debug(3,"Could not find Via: header\n");
		}
		else {
			ast_debug(3,"-Via: header %s\n",tmp_header);

			param_front = strtok_r(tmp_header,";",&param_back);
			if(param_front != NULL)
				reply_string_len += sprintf(reply+reply_string_len,"Via: %s",param_front);
			while(param_front ){ /* Continue parsing until to end of Via Header */
				ast_debug(3,"-Via param: %s\n",param_front);
				param_count++;
				if (strncmp(param_front,"branch=",7)==0) {
					reply_string_len += sprintf(reply+reply_string_len,";%s",param_front);
				}
				/* RFC 3581 adds an extension for symmetric routing using "rport" in Via */
				if (strncmp(param_front,"rport",5)==0) {
					reply_string_len += sprintf(reply+reply_string_len,\
								";rport=%i;received=%s", 
								peer_port,peer_ip); 
				}
				param_front = strtok_r(NULL,";",&param_back);
			}
			if(param_count ==0) 
				ast_log(LOG_ERROR,"Via: header missing branch parameter.\n");
			reply_string_len += sprintf(reply+reply_string_len,"\r\n");
			ast_free(tmp_header);
		}

		if ( (tmp_header=GetHeaderValue(buffer,bufferLen,"Call-ID")) == 0){ /* non-0 returns alloc'd memory */
			ast_debug(3,"Could not find Call-ID: header\n");
		}
		else {
			ast_debug(3,"-Call-ID: header %s\n",tmp_header);
			reply_string_len += sprintf(reply+reply_string_len,"Call-ID: %s\r\n",tmp_header);
			ast_free(tmp_header);
		}

		if ( (tmp_header=GetHeaderValue(buffer,bufferLen,"Cseq")) == 0){ /* non-0 returns alloc'd memory */
			ast_debug(3,"Could not find Cseq: header\n");
		}
		else {
			ast_debug(3,"-Cseq: header %s\n",tmp_header);
			reply_string_len += sprintf(reply+reply_string_len,"Cseq: %s\r\n",tmp_header);
			ast_free(tmp_header);
		}
		reply_string_len += sprintf(reply+reply_string_len,"Content-Length: 0\r\n");
		strcat(reply,"\r\n");

		if (strncmp(buffer,"BYE",3)==0) {
			ast_debug(3,"-bye response: in_a_dialog = 0\n");
			player->in_a_dialog = 0; /* Remove call from dialog */
			something2send=1;
		}else if (strncmp(buffer,"INFO",4)==0) {
			ast_debug(3,"-info response done.\n");
			something2send=1;
		}else if(strncmp(request,"CANCEL",6)==0) {
			ast_debug(3,"TO BE DONE: Handle CANCEL response.\n");
		}
	}else{
		/* else, technically is suppose to send a 481 response, but will skip */
		ast_debug(3,"-not in a dialog. Skip sending sip 481 reply\n");
	}  


	if(something2send){
		ast_debug(3,"-sending sip reply:\n%s",reply);
		/* Send request.  Bypass player->end using temp. */
		if (!SendRequest(player->fd,reply,&temp))
			/* exit */
			return 0;
	}
	else {
		ast_debug(3,"-sip reply nothing to send\n");
	} 
	ast_debug(1,"<sip reply [%s]\n",username);
	return 1;
}

static int RecvResponse(int fd,char *buffer,int *bufferLen,int bufferSize,int *end)
{
	/* if error or closed */
	errno = 0;
	/* Read into buffer */
	int len = recv(fd,buffer,bufferSize-*bufferLen,0);

     /*	if (!len>0) OLD */
	if (!(len > 0)) /*PORT17.3. Fix compiler warning */
	{
		/* If failed connection*/
		if ((errno!=EAGAIN && errno!=EWOULDBLOCK) || !len)
		{
			/* log */
			ast_log(LOG_ERROR,"Error receiving response [%d,%d].%s\n",len,errno,strerror(errno));
			/* End */
			*end = 1;
		}
		/* exit*/
		return 0;
	} 
	/* Increase buffer length */
	*bufferLen += len;
	/* Finalize as string */
	buffer[*bufferLen] = 0; /* COMMENT buffer can be string or RTP binary byte */
	/* Return len */
	return len;
}

static int GetResponseLen(char *buffer)
{
	char *i;
	/* Search end of response */
	if ((i=strstr(buffer,"\r\n\r\n"))==NULL)
		/*Exit*/
		return 0;
	/* Get msg leng */
	return i-buffer+4;
}


static int main_loop(struct ast_channel *chan,char *ip, int rtsp_port, char *url,char *username,char *password,int isIPv6,int sip_enable, char *sip_realm, int sip_port)
{
	struct ast_frame *f = NULL;
     /*	struct ast_frame *sendFrame = NULL; OLD */
	struct ast_frame sendFrame; /* PORT 17.5 make this a real ast_frame struct. No longer malloc. */
	uint8_t FrameBuffer[AST_FRIENDLY_OFFSET + PKT_PAYLOAD];/* PORT 17.5 make a real buffer instead of alloc'd (See app_fax.c) */

	int infds[10]; /* CHANGE. from 5 to 10 to accomdate SIP */
	int num_infds=5; /* ADDED for use with SIP */
	int outfd;

	char buffer[16384];
	int  bufferSize = 16383; /* One less for finall \0 */
	int  bufferLen = 0;
	int  recvLen = 0; /* ADDED */
	int  responseCode = 0;
	int  responseLen = 0;
	int  contentLength = 0;
     /* char *rtpBuffer; OLD */
	uint8_t *rtpBuffer; /* PORT17.5 model this after app_fax.c */
	char rtcpBuffer[PKT_PAYLOAD];
	int  rtpSize = PKT_PAYLOAD;
	int  rtcpSize = PKT_PAYLOAD;
	int  rtpLen = 0;
	int  rtcpLen = 0;
	char *session;
	char *transport;
	char *range;
	char *j;
	char src[128];
	int  res = 0;

	struct SDPContent* sdp = NULL;
	struct SDPContent* sip_sdp = NULL; /* ADDED */
	char *audioControl = NULL;
	char *videoControl = NULL;
	int audioFormat = 0;
	int videoFormat = 0;
	struct ast_format *audioNewFormat = NULL; /* PORT 17.3. Track using new media format */
	struct ast_format *videoNewFormat = NULL; /* PORT 17.3. Track using new media format */
	int audioType = 0;
	int videoType = 0;
	unsigned int lastVideo = 0;
	unsigned int lastAudio = 0;

	int duration = 0;
	int elapsed = 0;
	int ms = 10000;
	int i = 0;
	int temp; /* ADDED. SIP. */
	int enable_sip_tx = 0; /* ADDED. SIP. */
	uint16_t pre_enable_vf_tx_count = 0; /*ADDED. SIP */
	uint16_t post_enable_vf_tx_count = 0; /*ADDED. SIP */
	uint16_t sip_tx_error_count = 0; /*ADDED. SIP */
	struct RtspPlayer *player;
	struct RtpHeader *rtp;
	struct RtpHeader *sip_rtp; /*ADDED. SIP */
	uint32_t sip_prev_samples=0; /* ADDED. SIP */
	struct Rtcp rtcp;
	struct timeval tv = {0,0};
	struct timeval rtcptv = {0,0};

	struct RtspPlayer *sip_speaker = NULL;/* sip will make use of RTSP data structures */

	/* log */
     /*	ast_log(LOG_WARNING,">rtsp_sip main loop\n");    was "rtsp play" */
	ast_log(LOG_NOTICE,">rtsp-sip main loop\n");

	/* Set random src for AST_FRAME debugging */
	sprintf(src,"rtsp_play%08lx", ast_random());

	/* Create RTSP player */
	player = RtspPlayerCreate();

	/* if error */
	if (!player)
	{
		/* log */
		ast_log(LOG_ERROR,"Couldn't create RTSP player\n");
		/* exit */
		return 0;
	}

	/* ADDED Create SIP Speaker */
	if(sip_enable){
		sip_speaker = RtspPlayerCreate(); /* SIP will re-use the RTSP data structures for its own use*/
		if (!sip_speaker)
		{
			/* log */
			ast_log(LOG_ERROR,"Couldn't create SIP Speaker\n");
			/* exit */
			return 0;
		}
	}

	/* Connect player */
	if (!RtspPlayerConnect(player,ip,rtsp_port,isIPv6,0))
	{
		/* log */
		ast_log(LOG_ERROR,"Couldn't connect RTSP to %s:%d\n",ip,rtsp_port);
		/* end */
		goto rtsp_play_clean;
	}

	/* ADDED Connect sip speaker */
	if(sip_enable){
		if (!RtspPlayerConnect(sip_speaker,ip,sip_port,isIPv6,1))
		{
			/* log */
			ast_log(LOG_ERROR,"Couldn't connect SIP Speaker to %s:%d\n",ip,sip_port);
			/* end */
			goto rtsp_play_clean;
		}
	}

	/* Set arrays */
	infds[0] = player->fd; 
	infds[1] = player->audioRtp;
	infds[2] = player->videoRtp;
	infds[3] = player->audioRtcp;
	infds[4] = player->videoRtcp;

	/* ADDED more arrays for sip speaker */
	if(sip_enable){
		infds[5] = sip_speaker->fd; 
		infds[6] = sip_speaker->audioRtp;
		infds[7] = sip_speaker->videoRtp;
		infds[8] = sip_speaker->audioRtcp;
		infds[9] = sip_speaker->videoRtcp;
		num_infds += 5;
	}

	/* Send RTSP REQUEST */
	if (!RtspPlayerDescribe(player,url))
	{
		/* log */
		ast_log(LOG_ERROR,"Couldn't handle DESCRIBE in %s\n",url);
		/* end */
		goto rtsp_play_end;
	}

	/* ADDed. Send SIP OPTIONS */
	if(sip_enable){
		if (!SipSpeakerOptions(sip_speaker,username))
		{
			/* log */
			ast_log(LOG_ERROR,"Couldn't formulate/send SIP Options\n");
			/* end */
			goto rtsp_play_end;
		}
	}

	/* malloc frame & data */
     /*	sendFrame = (struct ast_frame *) malloc(PKT_SIZE); OLD */
     /*	sendFrame = (struct ast_frame *) ast_malloc(PKT_SIZE);  PORT 17.3. New malloc scheme. OLD */

	/* Set data pointer */
     /*	rtpBuffer = (unsigned char*)sendFrame + PKT_OFFSET; OLD */
     /*	rtpBuffer = (char*)(sendFrame + PKT_OFFSET);    PORT 17.3. fix compiler warning; signedness of sendFrame */
	rtpBuffer = (uint8_t*)(FrameBuffer + AST_FRIENDLY_OFFSET);/* PORT 17.5. Restructuring sendFrame */

	/* log */
     /*	ast_log(LOG_DEBUG,"-rtsp play loop [%d]\n",duration); OLD */
	ast_debug(2,"-rtsp play loop [%d]\n",duration);

	/* Loop */
	while(!player->end)
	{
		/* No output */
		outfd = -1;
		/* If the playback has started */
		if (!ast_tvzero(tv))
		{
			/* Get playback time */
			elapsed = ast_tvdiff_ms(ast_tvnow(),tv); 
			/* Check how much time have we been playing */
			if (elapsed>=duration)
			{
				/* log */
			     /*	ast_log(LOG_DEBUG,"Playback finished\n"); OLD */
				ast_debug(2,"Playback finished\n");
				/* exit */
				player->end = 1;
				/* Exit */
				break;
			} else {
				/* Set timeout to remaining time*/
				ms = duration-elapsed;
			}
		} else {
			/* 4 seconds timeout */
			ms = 4000;
		}

		/* PORT17.3
		 * ast_waitfor_nandfds can return NULL if timedout, so tweaking the logic to handle it.
		 * returns NULL if channel has nothing, outfd < 0 if no active infds 
		 *    (note: active channel overrides active fd). ms returns=0 if no active infds.
		 */
		/* Read from channel and fds */
		outfd = -1;
		errno = 0;
		struct ast_channel *rchan;
	     /*	if (ast_waitfor_nandfds(&chan,1,infds,10,NULL,&outfd,&ms))  CHANGE fd num from 5 to 10 */
		rchan = ast_waitfor_nandfds(&chan,1,infds,num_infds,NULL,&outfd,&ms); /* CHANGE Handle Null return. var num of fds */
		if(rchan == NULL && outfd <0 && ms){
			if (errno == 0 || errno == EINTR)
				ast_log(LOG_WARNING, "ast_waitfor_nandfds() failed (%s)\n", strerror(errno));
		}
		if(rchan !=NULL && outfd <0) /* Channel active */
		{
			/* Read frame */
			f = ast_read(chan);

			/* If failed */
			if (!f) {
				ast_log(LOG_ERROR, "ast_read() failed. Bail out!\n"); /* ADDED */
				/* exit */
				break;
			}	
			/* If it's a control channel */
			if (f->frametype == AST_FRAME_CONTROL) 
			{
				/* Check for hangup */

				/* PORT 17.3
				 * from frame.h
				 * struct ast_frame *f
				 *   struct ast_frame_subclass subclass 
				 *     int integer
				 *     struct ast_format *format
				 *     unsigned int frame_ending
				 * Most of the app_xxx code uses: f->subclass.integer == AST_CONTROL_HANGUP.
				 * So apparently subclass.integer is to be used with 
				 * enum ast_control_frame_type{AST_CONTROL_HANGUP}
				 */
			     /*	if (f->subclass == AST_CONTROL_HANGUP) OLD */
				if (f->subclass.integer == AST_CONTROL_HANGUP) /* PORT 17.3 */
				{
					/* log */
				     /*	ast_log(LOG_DEBUG,"-Hangup\n"); OLD*/
					ast_debug(2,"-Hangup\n");
					/* exit */
					player->end = 1;
				}
				
			 /* If it's a dtmf */
			} else if (f->frametype == AST_FRAME_DTMF) {
				char dtmf[2];
				/* Get dtmf number */

				/* PORT 17.3 app_meetme.c uses this same subclass.integer
				 * if (f->frametype == AST_FRAME_DTMF)  
				 *         dtmfstr[0] = f->subclass.integer;
				 *         dtmfstr[1] = '\0';
				 */
			     /*	dtmf[0] = f->subclass; OLD */
				dtmf[0] = f->subclass.integer; /* PORT 17.3 */
				dtmf[1] = 0;

				/* Check for dtmf extension in context */

				/* PORT 17.3                                                          
				 * chan->context has been replaced with                                
				 *   const char *ast_channel_context(const struct ast_channel *chan)   
				 *   which returns chan->context                                       
				 */

			     /*	if (ast_exists_extension(chan, chan->context, dtmf, 1, NULL))  OLD  */
				if (ast_exists_extension(chan, ast_channel_context(chan), dtmf, 1, NULL)) { /* PORT 17.3 */
					/* Set extension to jump */
				     /*	res = f->subclass; was */
					res = f->subclass.integer;
					/* Free frame */
					ast_frfree(f);
					/* exit */
					goto rstp_play_stop;
				}
			} else if (f->frametype == AST_FRAME_VOICE && sip_enable ) { /*ADDED. SIP.*/
				int no_room_err =- 1;

				if(enable_sip_tx == 0) /* Start Voice Tx after SIP INVITE is OK'd */
					pre_enable_vf_tx_count++; /* count num of Frames tossed before SIP INVITE is OK'd */
				else {
					post_enable_vf_tx_count++;/* count num of Frames sent after SIP INVITE is OK'd */

					/* check to see if AST FRAME has enough room for RTP Header */
					if(f->offset >= sizeof(struct RtpHeader)){ 
						no_room_err = 0;

						sip_rtp = f->data.ptr - sizeof(struct RtpHeader); /* rtp header starts here */
						sip_rtp->version = 2;
						sip_rtp->p=0;
						sip_rtp->x=0;
						sip_rtp->cc=0;
						/* Set Marker on first frame only */
						if( post_enable_vf_tx_count == 1)
							sip_rtp->m=1;
						else
							sip_rtp->m=0;
						sip_rtp->pt= sip_sdp->audio->formats[0]->payload; /* Payload Type */

						/*RFC3550 p14 says start seq num w. random number.
						 * pre_enable_vf_tx_count is somewhat random as the number of packets tossed
						 * before SIP INVITE is OK'd is just that. RTP Header:Seq num is 16b 
						 */
						sip_rtp->seq = htons(pre_enable_vf_tx_count+post_enable_vf_tx_count); 

						/* RFC3550 p14.  RTP Header:Timestamp is 32b.
						 * For fixed rate codecs, TS increments by number of samples since last time.
						 * Assume for now that the audio codes are fixed-rate (8Khz).
						 */
						sip_rtp->ts=htonl(sip_prev_samples);

						/* RFC3550 p16. SSRC Should be random. Do the same thing 
						 * that MediaStatsRR() did. 
						 */
					     /* sip_rtp->ssrc=htonl(((unsigned int)&sip_speaker->audioStats)); */
						sip_rtp->ssrc= htonl((uint32_t)random()); /* PORT 17.5 fix compiler complaint */

						sip_prev_samples = sip_prev_samples + f->samples;

						if( post_enable_vf_tx_count == 1){
							ast_debug(3,"-vf_frame datalen:%i\n",f->datalen);
							ast_debug(3,"-vf_frame samples:%i\n",f->samples);
							ast_debug(3,"-vf_frame offset:%i\n",f->offset);
							ast_debug(3,"-Offset room error check:%i\n",no_room_err);
						}

						/* Send rtp packet */
						int num_bytes_sent;
						errno = 0;
						num_bytes_sent = send(sip_speaker->audioRtp, sip_rtp,\
								      sizeof(struct RtpHeader)+f->datalen, 0);
						if(num_bytes_sent == -1)
							sip_tx_error_count++;
					}

				} 
			}

			/* free frame */
			ast_frfree(f);
		} else if (outfd==player->fd) { /* outfd >0 */
			/* Depending on state */	
			switch (player->state)
			{
				case RTSP_DESCRIBE:
					/* log */
				     /*	ast_log(LOG_DEBUG,"-Receiving describe\n"); OLD */
					ast_debug(2,"-rx describe response\n");
					/* Read into buffer */
					if (!RecvResponse(player->fd,buffer,&bufferLen,bufferSize,&player->end))
						break;
			              //ast_debug(5,"bufferLen: %i\n%s",bufferLen,buffer);
					ast_debug(3, "\n%s\n",buffer); 

					/* Check for response code */
					responseCode = GetResponseCode(buffer,bufferLen,0);

				     /*	ast_log(LOG_DEBUG,"-Describe response code [%d]\n",responseCode); OLD */
					ast_debug(3,"-describe response code [%d]\n",responseCode);

					/* Check unathorized */
					if (responseCode==401)
					{
						/* Check athentication method */

                                                /* [v2.0]  Adding new way of detecting authentication method */
					        ast_debug(3,"  describe 401 Processing\n");
					        ast_debug(3,"    - Checking for Auth Method of Basic\n");
                                                struct BasicAuthData basic_data;
                                                if (GetAuthSchemeBasic(buffer,bufferLen,&basic_data) == 0 )
                                                {
					            ast_debug(3,"    - Found Auth Method of Basic\n");

						    /* Create Basic authentication header */
						    RtspPlayerBasicAuthorization(player,username,password);
						    /* Send again the describe */
						    RtspPlayerDescribe(player,url);
						    /* Enter loop again */
						    break;
                                                }
                                                else
                                                {
					            ast_debug(3,"    - No Auth Method of Basic\n");
					            ast_debug(3,"    - Checking for Auth Method of Digest\n");

                                                    struct DigestAuthData digest_data;
                                                    if (GetAuthSchemeDigest(buffer,bufferLen,&digest_data) == 0 )
                                                    {
					                ast_debug(3,"    - Found Auth Method of Digest\n");
							char *nc = NULL;
							char *cnonce = NULL;
							char *qop = NULL;
							char uri[256];
							sprintf(uri,"rtsp://%s%s", player->hostport, url);
							char *method = "DESCRIBE";
 
							ast_debug(5,"  Challenge Response Data- rx_realm: %s nonce: %s uri %s",\
								digest_data.rx_realm, digest_data.nonce, uri); 
                                              
							if (RtspPlayerDigestAuthorization(player,username,\
									password, digest_data.rx_realm,\
									digest_data.nonce, nc, cnonce, qop, uri, \
									digest_data.rx_realm, method, 0) > 0)
							{
								/* Send again the describe */
								RtspPlayerDescribe(player,url);
								/* Enter loop again */
								break;
							}
							else
							{
								ast_log(LOG_ERROR,"Failed to create digest authorization\n");
								/* End */
								player->end = 1;
								/* Exit */
								break;
							}
                                                    }
                                                    else
                                                    {
					                ast_debug(3,"    - No Auth Method of Digest\n");
						        /* Error */
						        ast_log(LOG_ERROR,"-No Basic or Digest Authentication found for RTSP.\n");	
						        /* End */
						        player->end = 1;
						        /* Exit */
						        break;
                                                    }
                                                }
#ifdef OLD_AUTH_SCHEME
						/* 
						 * PORT 17.3.  The Basic Realm header format may be device dependent.
						 * Original code did not work for my cameras.
						 */
					      //if (CheckHeaderValue(buffer,bufferLen,"WWW-Authenticate","Basic realm=\"/\"")) */
				                if (CheckHeaderValue(buffer,bufferLen,"WWW-Authenticate","Basic realm="))
						{
							/* Create Basic authentication header */
							RtspPlayerBasicAuthorization(player,username,password);
							/* Send again the describe */
							RtspPlayerDescribe(player,url);
							/* Enter loop again */
							break;
						} else {
							/* Error */
					              //ast_log(LOG_ERROR,"-No Authenticate header found\n");	
							ast_log(LOG_ERROR,"-No Basic Authentication found for RTSP (Digest Auth not yet supported).\n");	
							/* End */
							player->end = 1;
							/* Exit */
							break;
						}
#endif
					}

					/* On any other erro code */
					if (responseCode<200 || responseCode>299)
					{
						/* End */
						player->end = 1;
						/* Exit */
						break;
					}

					/* If not reading content */
					if (contentLength==0)
					{
						/* Search end of response */
						if ( (responseLen=GetResponseLen(buffer)) == 0 )
							/*Exit*/
							break;

						ast_debug(5, "ResponseLen: %i\n",responseLen); /*tjl*/
						/* Does it have content */
						contentLength = GetHeaderValueInt(buffer,responseLen,"Content-Length");	
						ast_debug(5, "contentLength: %i\n",contentLength); /*tjl*/
						/* Is it sdp */
						if (!CheckHeaderValue(buffer,responseLen,"Content-Type","application/sdp"))
						{
							/* log */
							ast_log(LOG_ERROR,"Content-Type unknown\n");
							/* End */
							player->end = 1;
							/* Exit */
							break;
						}
						/* Get new length */
						bufferLen -= responseLen;

						/* Move Message Body (data) to begining of buffer.*/
				            /*	memcpy(buffer,buffer+responseLen,bufferLen); OLD BUGGY  */
						memmove(buffer,buffer+responseLen,bufferLen);
					}
					
					/* If there is not enough data */	
					if (bufferLen<contentLength) 
						/* break */
						break;

					/* Create SDP */
#ifdef TEST_BUFFER
					sdp = CreateSDP(buffer,bufferLen,sip_enable); 
#else
					sdp = CreateSDP(buffer,contentLength,sip_enable);
#endif
					/* Get new length */
					bufferLen -= contentLength;
					/* Move data to begining */
				    /*	memcpy(buffer,buffer+responseLen,bufferLen); OLD BUGGY */
					memmove(buffer,buffer+responseLen,bufferLen);
					/* Reset content */
					contentLength = 0;

					/* If not sdp */
					if (!sdp)
					{
						/* log */
						ast_log(LOG_ERROR,"Couldn't parse SDP\n");
						/* end */
						player->end = 1;
						/* exit */
						break;
					}
					ast_debug(4,"Successfully parsed SDP\n"); /* ADDED */

					/* PORT TO 17.3     
					 * Formats of media has been restructured to be ast_format_cap instead of bit list:
					 * - chan->nativeformats has been replaced with
					 *     ast_channel_nativeformats(const struct ast_channel *chan)  @channel_internal.c
					 *     which returns chan->nativeformats (ptr to struct ast_format_cap) 
					 * - Can convert to strings using ast_format_cap_get_names(). 
					 *     See app_dumpchan.c for example.
					 *   Note: ast_str_alloca uses memory from the stack.
					 */
				     /*	ast_log(LOG_DEBUG,"-Finding compatible codecs [%x]\n", chan->nativeformats); OLD */
					struct ast_str *format_buf = ast_str_alloca(AST_FORMAT_CAP_NAMES_LEN);
					ast_debug(4,"-Finding compatible codecs [%s]\n", \
						  ast_format_cap_get_names(ast_channel_nativeformats(chan), &format_buf));
					/* Get best audio track */
					if (0)//sdp->audio) // FIXME disable audio from camera for now
					{
						/* Avoid overwritten */

						/* PORT 17.3 
						 * Formats of media have been restructured to be ast_format 
						 * instead of a bit in a bit list. 
						 * Formats are contained in a list of capabilities 
						 *     ast_format_cap instead of bit list.
						 *  Note: Capabilities: ast_format_cap is a vector structure 
						 *     list of formats and pref order .
						 * - Go through the sdp->audio->all bitlist and create a ast_format for each
						 *     and add to ast_format_cap list.
						 * - ast_translator_best_choice() has been rewritten 
						 *     for this scheme (and it added other things too ).
						 * Reference some of the code from channel.c for this.
						 */
						struct ast_format_cap *sdp_cap;
						struct ast_format_cap *chan_native_cap;

						/* COMMENT on memory:
						 * RAII_VAR w. ao2_cleanup: allocs memory 
						 * but when out of scope will self-destruct 
						 */
						RAII_VAR(struct ast_format *, sdp_fmt, NULL, ao2_cleanup);
						RAII_VAR(struct ast_format *, best_sdp_fmt, NULL, ao2_cleanup);
						RAII_VAR(struct ast_format *, best_native_fmt, NULL, ao2_cleanup);

						/* COMMENT on memory
						 * ast_format_cap_alloc allocs memory
						 * and is freed w. ao2_cleanup 
						 */
						sdp_cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT); 

                                                /* 
						 * PORT 17.3, 
						 * go thru entire SDP->audio->formats 
						 * (instead of SDP->audio-all bitlist).  
						 */
						for (i=0;i<sdp->audio->num;i++)
						{
							sdp_fmt = sdp->audio->formats[i]->new_format;
							/* append to sdp capability list */
							ast_format_cap_append(sdp_cap,sdp_fmt,0); 
						}
						/* PORT 17.3, get the best sdp media from the sdp learned media list. */
						best_sdp_fmt = ast_format_cap_get_best_by_type(sdp_cap, AST_MEDIA_TYPE_AUDIO);

						/* 
						 * PORT 17.3 
						 * - ast_channel_nativeformats(const struct ast_channel *chan)  
						 *   (See channel_internal.c as an example).
						 *   returns pointer to ast_format_cap. (See @format_cap.c line54 )
						 * I have no idea about ORing AMRNB.  So will remove for now.
						 */
					     /*	int best = chan->nativeformats | AST_FORMAT_AMRNB; OLD */
						/* COMMENT: ast_channel_nativeformats() returns chan->nativeformats */
						chan_native_cap = ast_channel_nativeformats(chan); 

						/* ADD. Not likely to happen but keep as a check*/
						if(ast_format_cap_empty(chan_native_cap)) 
							ast_debug(1, "No native codec for audio on channel %s\n",\
								  ast_channel_name(chan)); 

						/* Get best codec format for audio */

						/* PORT 17.3 Reworked this to accomodate new format structures 
						 * and new capabilities structures. 
						 * Assume that the legacy code intent is that given media 
						 * capabilities of the channel, and media capabilities 
						 * learned from SDP, determine what is the best media for the channel.
						 *
						 * New API is  ast_translator_best_choice(
						 *                 struct ast_format_cap *dst_cap, 
						 *                 struct ast_format_cap *src_cap,
						 *                 struct ast_format **dst_fmt_out,
						 *                 struct ast_format **src_fmt_out)               
						 *
						 * Assume the source is sdp, and destination is the channel.
						 * dst_cap: ast_channel_nativeformats is already a struct ast_format_cap 
						 * src_cap: sdp->audio->all is an int which 
						 *          legacy sdp used as a bit list of AST_FORMAT_xxx 
						 *          but this was reworked above for new media capabilities: sdp_cap.
						 * src_fmt_out:  finally, we'll use the best sdp format as the source format, 
						 * dst_cap: let the algorithm determine the best 
						 *          destination format: best native format.
						 */
					     /*	ast_translator_best_choice(&best, &sdp->audio->all); OLD */
						ast_translator_best_choice(chan_native_cap, sdp_cap, &best_native_fmt, &sdp_fmt);

					     /*	ast_log(LOG_DEBUG,"-Best codec for audio [%x]\n", best); OLD */
						ast_debug(4, "-Best codec for audio on channel %s is format %s\n", 
							ast_channel_name(chan), ast_format_get_name(best_native_fmt));
                                                
						ao2_cleanup(sdp_cap); /* free up sdp_cap allocd memory */

						/* Get first matching format */
						for (i=0;i<sdp->audio->num;i++)
						{
							/* log */
						     /*	ast_log(LOG_DEBUG,"-audio [%x,%d,%s]\n",\
						       	        sdp->audio->formats[i]->format,\
						       	        sdp->audio->formats[i]->payload ,
						       	        sdp->audio->formats[i]->control); OLD */
							/* PORT 17.5 fix for format now unsigned long long */
							ast_debug(4,"-audio [%"PRIx64",%d,%s]\n",\
								sdp->audio->formats[i]->format,\
							       	sdp->audio->formats[i]->payload,\
								sdp->audio->formats[i]->control);

							/* if we have that */
							/* PORT 17.3 Use new media format */
						     /* if (sdp->audio->formats[i]->format == best) OLD */
							if(ast_format_cmp(sdp->audio->formats[i]->new_format, \
									best_native_fmt) == AST_FORMAT_CMP_EQUAL )
							{
								/* Store type */
								audioType = sdp->audio->formats[i]->payload;
								/* PORT 17.3 compiler warns audioType not used, so use it */
								if(audioType){ 
									ast_debug(1, "-audioType is %i\n", audioType );
								}
								/* Store format */
								audioFormat = sdp->audio->formats[i]->format;
								audioNewFormat = sdp->audio->formats[i]->new_format; /* PORT 17.3 add */
								/* Store control */
								audioControl = sdp->audio->formats[i]->control;
								/* Found best codec */
								ast_debug(4,"-Found best audio codec\n");
								/* Got a valid one */
								break;
							}
						}
					}

					/* Get best video track */
					if (sdp->video)
						/* Get first matching format */
						for (i=0;i<sdp->video->num;i++)
						{
							/* log */
						    /*	ast_log(LOG_DEBUG,"-video [%x,%d,%s]\n",\
						      	        sdp->video->formats[i]->format,\
						      	        sdp->video->formats[i]->payload,\
						      	        sdp->video->formats[i]->control); OLD */
							/* PORT 17.3 fix for format now unsigned long long */
							ast_debug(4,"-video [%"PRIu64",%d,%s]\n",\
								sdp->video->formats[i]->format,\
								sdp->video->formats[i]->payload ,\
								sdp->video->formats[i]->control); 

							/* if we have that */
							/* PORT 17.3
							 * It appears here it is taking the list of rtsp 
							 * learned media and comparing with the media 
							 * supported natively by the channel and the 
							 * first one it finds a match on, 
							 * will stop the search and declare it the best.
							 * We'll do something similar by checking 
							 * to see if rtsp learned media is in the 
							 * capabilities list of the channel:
							 *    Returning NULL if not  
							 *    Otherwise returns a compatible media.
							 */
						     /*	if (sdp->video->formats[i]->format & chan->nativeformats) OLD */
							struct ast_format *video_compat_format;
							video_compat_format = \
								ast_format_cap_get_compatible_format(ast_channel_nativeformats(chan),\
									sdp->video->formats[i]->new_format);
							if (video_compat_format)
							{
								/* Store type */
								videoType = sdp->video->formats[i]->payload;
								/* PORT 17.3 compiler warns videoType not used, so use it */
								if(videoType){ 
									ast_debug(1, "-videoType is %i\n", videoType );
                                                                }
								/* Store format */
								videoFormat = sdp->video->formats[i]->format;
								//videoNewFormat = video_compat_format; /* PORT 17.3 */
								videoNewFormat = sdp->video->formats[i]->new_format; /* PORT 17.3 */
								/* Store control */
								videoControl = sdp->video->formats[i]->control;
								/* Found best codec */
								ast_debug(4,"Found best video codec\n");
								/* Got a valid one */
								break;
							}
							else /*ADD */
								ast_log(LOG_WARNING,\
									"No compatible format found for Video on channel\n");
						}

					/* Log formats */
				   /*	ast_log(LOG_DEBUG,"-Set write format [%x,%x,%x]\n",\
				    	 	audioFormat | videoFormat, audioFormat, videoFormat); OLD */
					ast_debug(4,"-Set write format [%x,%x,%x]\n",\
						audioFormat | videoFormat, audioFormat, videoFormat);

					/* Set write format */
					/* PORT 17.3 move ast_set_write_format further down and use new formats */
				     /*	ast_set_write_format(chan, audioFormat | videoFormat);	OLD. */ 

					ast_debug(3, "-Set write format on channel %s:\n",ast_channel_name(chan)); /*ADD*/

					/* if audio track */
					if (audioControl)
					{
						/* Open audio */
					        /* Set write format. PORT17.3 Moved from above to here */
						ast_debug(1, "  for %s\n ",ast_format_get_name(audioNewFormat)); /*ADD*/
						ast_set_write_format(chan, audioNewFormat);
						RtspPlayerSetupAudio(player,audioControl);
					} else if (videoControl) {
						/* Open video */
						/* Set write format. PORT 17.3 Moved from above to here to use new format*/
						/*ADD. if there is no compatible video format then skip write*/
						if(videoNewFormat){ 
							ast_debug(1, "  for %s\n ",ast_format_get_name(videoNewFormat)); /*ADD*/
							ast_set_write_format(chan, videoNewFormat);
				 			RtspPlayerSetupVideo(player,videoControl);
						}
					} else {
						/* log */
						ast_log(LOG_ERROR,"No media found\n");
						/* end */
						player->end = 1;
					}
					break;
				case RTSP_SETUP_AUDIO:
					/* log */
				     /*	ast_log(LOG_DEBUG,"-Recv audio response\n"); OLD */
					ast_debug(2,"-rx rtsp setup for audio response\n");
					/* Read into buffer */
					if (!RecvResponse(player->fd,buffer,&bufferLen,bufferSize,&player->end))
						break;
					ast_debug(3, "\n%s\n",buffer); //Added [v2.0]
					/* Search end of response */
					if ( (responseLen=GetResponseLen(buffer)) == 0 )
						/*Exit*/
						break;

					/* Does it have content */
					if (GetHeaderValueInt(buffer,responseLen,"Content-Length"))
					{
						/* log */
						ast_log(LOG_ERROR,"Content length not expected\n");
						/* Uh? */
						player->end = 1;
						/* break */
						break;
					}
					/* Get session */
					if ( (session=GetHeaderValue(buffer,responseLen,"Session")) == 0)
					{
						/* log */
						ast_log(LOG_ERROR,"No session [%s]\n",buffer);
						/* Uh? */
						player->end = 1;
						/* break */
						break;
					}
					/* Append session to player */
					RtspPlayerAddSession(player,session);
					/* Get transport value to obtain rtcp ports */
					if ((transport=GetHeaderValue(buffer,responseLen,"Transport")) == 0)
					{
						/* log */
						ast_log(LOG_ERROR,"No transport [%s]\n",buffer);
						/* Uh? */
						player->end = 1;
						/* break */
						break;
					}
					/* Process transport */
					RrspPlayerSetAudioTransport(player,transport);
					/* Free string */
			             /*	free(transport); was */
					ast_free(transport);
					/* Get new length */
					bufferLen -= responseLen;
					/* Move data to begining */
				     /*	memcpy(buffer,buffer+responseLen,bufferLen); OLD */
					memmove(buffer,buffer+responseLen,bufferLen);
					/* If video control */
					if (videoControl){
						/* Set up video */
						/* ADD. If there is no compatible video format then skip video setup*/
						if(videoNewFormat) 
			 				RtspPlayerSetupVideo(player,videoControl);
					}
					else 
					{
						/* play */
						RtspPlayerPlay(player);

						/* 
						 * ADDed. 
						 * RTSP is now playing so get SIP going.
						 * We also know which audio codec to use (see audioFormat). 
						 */

						/* Send SIP unauthorized INVITE */
						if(sip_enable){
							if (!SipSpeakerInvite(sip_speaker,username,audioFormat,0))
							{
								ast_log(LOG_ERROR,"Couldn't formulate/send INVITE\n");
		                                        	/* Nothing else to do, simply don't do any more SIP stuff */
							}
						}
					}
					break;
				case RTSP_SETUP_VIDEO:
					/* log PORT17.3 adder*/
				     /*	ast_log(LOG_DEBUG,"-Recv video response\n"); OLD */
					ast_debug(2,"-Recv video response\n");

					/* Read into buffer */
					if (!RecvResponse(player->fd,buffer,&bufferLen,bufferSize,&player->end))
						break;
					/* Search end of response */
					if ( (responseLen=GetResponseLen(buffer)) == 0 )
						/*Exit*/
						break;

					/* Does it have content */
					if (GetHeaderValueInt(buffer,responseLen,"Content-Length"))
					{
						/* log */
						ast_log(LOG_ERROR,"No content length\n");
						/* Uh? */
						player->end = 1;
						/* break */
						break;
					}
					/* Get session if we don't have already one*/
					if ( (session=GetHeaderValue(buffer,responseLen,"Session")) == 0)
					{
						/* log */
						ast_log(LOG_ERROR,"No session [%s]\n",buffer);
						/* Uh? */
						player->end = 1;
						/* break */
						break;
					}
					
					/* Append session to player */
					RtspPlayerAddSession(player,session);
					/* Get transport value to obtain rtcp ports */
					if ((transport=GetHeaderValue(buffer,responseLen,"Transport")) == 0)
					{
						/* log */
						ast_log(LOG_ERROR,"No transport [%s]\n",buffer);
						/* Uh? */
						player->end = 1;
						/* break */
						break;
					}
					/* Process transport */
					RrspPlayerSetVideoTransport(player,transport);
					/* Free string */
				     /*	free(transport); OLD */
					ast_free(transport);
					/* Get new length */
					bufferLen -= responseLen;
					/* Move data to begining */
				     /*	memcpy(buffer,buffer+responseLen,bufferLen); OLD */
					memmove(buffer,buffer+responseLen,bufferLen);
					//send to first (even) server port (RTP) 8000 0000 0000 0000 0000 0000
					//FIXME this is needed to start stream, but what should this really be?
					short rtp_start[] = {0x0080,0x0000,0x0000,0x0000,0x0000,0x0000};
					send(player->videoRtp, &rtp_start, sizeof(rtp_start), 0);
					/* Create rtcp packet */
					MediaStatsRR(&player->videoStats,&rtcp);
					/* Send packet */
					send(player->videoRtcp, &rtcp, sizeof(rtcp), 0);
					/* Play */
					RtspPlayerPlay(player);
					break;
				case RTSP_PLAY:
					/* Read into buffer */
					ast_debug(2,"-rx rtsp play response\n");
					if (!RecvResponse(player->fd,buffer,&bufferLen,bufferSize,&player->end))
						break;
					ast_debug(3, "\n%s\n",buffer); //Added [v2.0]
					/* Search end of response */
					if ( (responseLen=GetResponseLen(buffer)) == 0 )
						/*Exit*/
						break;
					/* Get range */
					if ( (range=GetHeaderValue(buffer,responseLen,"Range")) == 0)
					{
						/* No end of stream */
						duration = -1;
					} else {
						/* Get end part */
						j = strchr(range,'-');
						/* Check format */
						if (j)
							/* Get duration */
							duration = atof(j+1)*1000;  
						else 
							/* No end of stream */
							duration = -1;
						/* Free string */
					     /*	free(range); was */
						ast_free(range);
					}
					/* If the video has end */
					if (duration>0)
						/* Init counter */
						tv = ast_tvnow();
					/* log */
				     /*	ast_log(LOG_DEBUG,"-Started playback [%d]\n",duration); OLD */
					ast_debug(2,"-Started playback [%d]\n",duration);
					/* Get new length */
					bufferLen -= responseLen;
					/* Move data to begining */
				     /*	memcpy(buffer,buffer+responseLen,bufferLen); OLD */
					memmove(buffer,buffer+responseLen,bufferLen);
					/* Init media stats */
					MediaStatsReset(&player->audioStats);
					MediaStatsReset(&player->videoStats);
					/* Set playing state */
					player->state = RTSP_PLAYING;
					break;
				case RTSP_PLAYING:
					/* Read into buffer */
					if (!RecvResponse(player->fd,buffer,&bufferLen,bufferSize,&player->end))
						break;
					break;
			}
		} else if ((outfd==player->audioRtp) ||  (outfd==player->videoRtp) ) { /* outfd >0 */
			/* Set length */
			rtpLen = 0;

			/* Clean frame */
		     /*	memset(sendFrame,0,sizeof(struct ast_frame) + rtpSize); OLD */
			memset(&sendFrame,0,sizeof(struct ast_frame)); /* PORT17.5 Restructuring sendFrame */
			memset(FrameBuffer,0,AST_FRIENDLY_OFFSET+PKT_PAYLOAD); /* PORT17.5 Restructuring sendFrame */


			/* Read rtp packet */
			if (!RecvResponse(outfd,(char*)rtpBuffer,&rtpLen,rtpSize,&player->end))
			{
				/* log */
			     /*	ast_log(LOG_DEBUG,"-Error reading rtp from [%d]\n",outfd); CHANGE */
				ast_log(LOG_WARNING,"-Error reading rtp from [%d]\n",outfd);
				/* exit*/
				break;
			}

			/* If not got enough data */
			if (rtpLen<12)
				/*exit*/
				break;

			/* Get headers */
			rtp = (struct RtpHeader*)rtpBuffer;

			/* Set data ini */
		     /*	int ini = sizeof(struct RtpHeader)-4; CHANGE. Compensate for removing csrc from RtpHeader */
			int ini = sizeof(struct RtpHeader);

			/* Increase length */
			ini += rtp->cc;

			/* Get timestamp */
			unsigned int ts = ntohl(rtp->ts);
			 
			/* Set frame data */
		     /*	AST_FRAME_SET_BUFFER(sendFrame,rtpBuffer,ini,rtpLen-ini); OLD */
			AST_FRAME_SET_BUFFER(&sendFrame,FrameBuffer,AST_FRIENDLY_OFFSET+ini,rtpLen-ini);
		     /*	sendFrame->src = strdup(src); OLD */
		     /*	sendFrame->src = ast_strdup(src); New way but why dup this as src is already an array */
			sendFrame.src = src; /* PORT17.3, removed strdup. PORT17.5 restructure sendFrame */

			/* Depending on socket */
			if (outfd==player->audioRtp) {
				/* Set type */
			     /*	sendFrame->frametype = AST_FRAME_VOICE; OLD */
				sendFrame.frametype = AST_FRAME_VOICE; /* PORT17.5 restructure sendFrame */

				/* 
				 * PORT 17.3.  Rework to accomodate the new media format.
				 * subclass.integer for VOICE is codec in AST_FORMAT_*
				 * subclass.format for VOICE is new media format in struct ast_format
				 * PORT 17.5 restructure sendFrame
				 */
			     /*	sendFrame->subclass =  audioFormat; OLD */
				sendFrame.subclass.integer =  audioFormat;
				sendFrame.subclass.format =  audioNewFormat;
				/* Set number of samples */
				if (lastAudio)
					/* Set number of samples */
				     /*	sendFrame->samples = ts-lastAudio; OLD */
					sendFrame.samples = ts-lastAudio;
				else
					/* Set number of samples to 160 */
				     /*	sendFrame->samples = 160; OLD */
					sendFrame.samples = 160;
				/* Save ts */
				lastAudio = ts;
				/* Set stats */
				MediaStatsUpdate(&player->audioStats,ts,ntohs(rtp->seq),ntohl(rtp->ssrc));
			} else {
				/* Set type */
			     /*	sendFrame->frametype = AST_FRAME_VIDEO; OLD */
				sendFrame.frametype = AST_FRAME_VIDEO;/* PORT 17.5 restructure sendFrame */

				/* PORT 17.3.  Rework to accomodate the new media format 
				 * subclass.integer for VIDEO is codec in AST_FORMAT_*
				 * subclass.format for VIDEO is new media format in struct ast_format
				 * PORT 17.5 restructure sendFrame
				 */
			     /*	sendFrame->subclass = videoFormat; OLD */
				sendFrame.subclass.integer =  videoFormat;
				sendFrame.subclass.format =  videoNewFormat;
				/* If not the first */
				if (lastVideo)
					/* Set number of samples */
				     /*	sendFrame->samples = ts-lastVideo; OLD */
					sendFrame.samples = ts-lastVideo; /* PORT 17.5 restructure sendFrame */
				else
					/* Set number of samples to 0 */
				     /*	sendFrame->samples = 0; OLD */
					sendFrame.samples = 0; /* PORT 17.5 restructure sendFrame */
				/* Save ts */
				lastVideo = ts;
				/* Set mark */
	 			/*	
				 * PORT 17.3 sendFrame->subclass is OLD. app_rtsp.new removed this line.
				 * This is trying to indicate the RTP frame has marker bit set (or not).
				 * I can not find anything, other than perhaps AST_CONTROL_SRCUPDATE
				 * that indicates an RTP frame maker being set and in channel.c 
				 * appears to not used.  Will skip for now.
				 */
			     /*	sendFrame->subclass |= rtp->m; OLD */
				sendFrame.subclass.frame_ending = rtp->m;

				/* Set stats */
				MediaStatsUpdate(&player->videoStats,ts,ntohs(rtp->seq),ntohl(rtp->ssrc));
			}

			/* Reset */
		     /*	sendFrame->delivery.tv_usec = 0; OLD */
			sendFrame.delivery.tv_usec = 0; /* PORT 17.5 restructure sendFrame */
		     /*	sendFrame->delivery.tv_sec = 0; OLD */
			sendFrame.delivery.tv_sec = 0;/* PORT 17.5 restructure sendFrame */
			/* Don't free the frame outside */
		     /*	sendFrame->mallocd = 0; OLD */
			sendFrame.mallocd = 0; /* PORT 17.5 restructure sendFrame */
			/* Send frame */
		     /*	ast_write(chan,sendFrame); OLD */
			ast_write(chan,&sendFrame); /* PORT 17.5 restructure sendFrame */

			/* PORT17.3 ADD. Fix memory-leak. Free mem from strdup(src) */
		     /*	ast_free((void*)sendFrame->src); PORT 17.5 No longer using strdup*/

		} else if ((outfd==player->audioRtcp) || (outfd==player->videoRtcp)) { /* outfd >0 */
			/* Set length */
			rtcpLen = 0;
			i = 0;
			
			/* Read rtcp packet */
			if (!RecvResponse(outfd,rtcpBuffer,&rtcpLen,rtcpSize,&player->end))
			{
				/* log */
			     /*	ast_log(LOG_DEBUG,"-Error reading rtcp from [%d]\n",outfd); CHANGE*/
				ast_log(LOG_WARNING,"-Error reading rtcp from [%d]\n",outfd);
				/* exit*/
				break;
			}

			/* Process rtcp packets */
			while(i<rtcpLen)
			{
				/* Get packet */
				struct Rtcp *rtcpRecv = (struct Rtcp*)(rtcpBuffer+i);
				/* Increase pointer */
				i += (ntohs(rtcpRecv->common.length)+1)*4;
				/* Check for bye */
				if (rtcpRecv->common.pt == RTCP_BYE)
				{
					/* End playback */
					player->end = 1;
					/* exit */	
					break;
				}
			}
			/* Send corresponding report */
			if (outfd==player->audioRtcp) {
				/* Create rtcp packet */
				MediaStatsRR(&player->audioStats,&rtcp);
				/* Reset media */
				MediaStatsReset(&player->audioStats);
				/* Send packet */
     				send(player->audioRtcp, &rtcp, (ntohs(rtcp.common.length)+1)*4, 0);
				/* log */
			    /*	ast_log(LOG_DEBUG,"-Sent rtcp audio report [%d]\n",errno); OLD */
				ast_debug(2,"-sent rtcp audio report [%d]\n",errno); 
			} else {
				/* Create rtcp packet */
				MediaStatsRR(&player->videoStats,&rtcp);
				/* Reset media */
				MediaStatsReset(&player->videoStats);
				/* Send packet */
     				send(player->videoRtcp, &rtcp, (ntohs(rtcp.common.length)+1)*4, 0);
				/* log */
			     /*	ast_log(LOG_DEBUG,"-Sent rtcp video report [%d]\n",errno); OLD */
				ast_debug(2,"-sent rtcp video report [%d]\n",errno); 
			}
		/* ADDED. SIP States */
		} else if (outfd==sip_speaker->fd) { /* outfd >0 */
			/* Depending on state */	
			switch (sip_speaker->state)
			{
			    	case SIP_STATE_OPTIONS:
			               //ast_debug(5,"-Receiving sip options\n");
					/* Read into buffer. ignore player->end by using temp*/
					if (!(recvLen=RecvResponse(sip_speaker->fd,buffer,&bufferLen,bufferSize,&temp)))
					{
						break; /* switch-case */
					}
					ast_debug(3, "-rx sip options response \n%s\n",buffer); 
					/* Check for response code */
					responseCode = GetResponseCode(buffer,bufferLen,1);

					ast_debug(3,"-sip options response code [%d]\n",responseCode);
					/* done with SIP message */
					bufferLen =0;
					break;
			    	case SIP_STATE_INVITE:
			        	ast_debug(3,"-rx sip invite response\n");

					/* Read into buffer. ignore player->end by using temp*/
					bufferLen =0; /* TEMP */
					if (!RecvResponse(sip_speaker->fd,buffer,&bufferLen,bufferSize,&temp))
						break;/* switch-case */
					ast_debug(3, "\n%s\n",buffer); 

					/* Check for response code */
					responseCode = GetResponseCode(buffer,bufferLen,1);
					ast_debug(3,"-sip invite response code [%d]\n",responseCode);

					if (responseCode>=100 && responseCode<=199)
					{   /* Provisional Response codes */
						switch(responseCode){
							case 100:
								ast_debug(3,"-sip invite response: 100 Trying\n");
								break;
							case 180:
								ast_debug(3,"-sip invite response: 180 Ringing\n");
								break;
							default:
								ast_debug(3,"-sip invite response: Unsupported 1xx Provisional response code\n");
						}
					}
					else if (responseCode>=200 && responseCode<=299)
					{
						if( SipSetPeerTag(sip_speaker,buffer,bufferLen) == -1)
							ast_debug(3,"SIP: Setting Peer Tag had a Failure.\n");

						/* RFC3261 13.1 2xx responses to a INVITE: session established, dialog is created */
						sip_speaker->in_a_dialog = 1; /* Set this after getting Peer Tag */

						/* RFC3261 2xx responses, an ACK is generated */
						/* RFC3261 17.1.1.3 ACK Cseq is to be same as last Cseq INVITE */
						sip_speaker->cseqm[ACK] = sip_speaker->cseqm[INVITE] - 1; 
						if (!SipSpeakerAck(sip_speaker,username,2))
						{
							ast_log(LOG_ERROR,"Couldn't formulate/send SIP ACK\n");
							/* Nothing else to do, simply don't do any more SIP stuff */
						}

						switch(responseCode){
							case 200:
								ast_debug(3,"-rx sip invite response: 200 OK\n");
								/* Search end of SIP Message Header */
								if ( (responseLen=GetResponseLen(buffer)) == 0 )
									break; /* switch-case */

								ast_debug(5, "ResponseLen: %i\n",responseLen); /*tjl*/
								contentLength = GetHeaderValueInt(buffer,responseLen,"Content-Length");	
								if (!CheckHeaderValue(buffer,responseLen,"Content-Type","application/sdp"))
								{
									ast_log(LOG_ERROR,"SIP: Content-Type unknown\n");
									break;/* switch-case */
								}
								/* Shift Message Data (i.e. SDP data) to beginning of buffer */
								bufferLen -= responseLen; 
								memmove(buffer,buffer+responseLen,bufferLen+1);/* ADDED +1 preserves string term*/

								/* If there is not enough room for data in the buffer */	
								if (bufferLen<contentLength) {
									ast_log(LOG_WARNING,"SIP: Message Data too big to fit!!\n");
									break; /* switch-case */
								}
								sip_sdp = CreateSDP(buffer,contentLength,1);
					   
								if (!sip_sdp)
								{
									ast_log(LOG_ERROR,"Couldn't parse sip SDP\n");
									break; /* switch-case */
								}
								ast_debug(3,"Successfully parsed sip SDP\n"); 

								/* Check SDP data to ensure only one codec in "Answer" */
								if( sip_sdp->audio->num != 1){
									ast_log(LOG_ERROR,"SIP: Peer Answers with more than 1 codec\n");
								}
								else{
									/* Check SDP data to ensure the "Answer" codec matches "Offer" */
									if(sip_sdp->audio->formats[0]->format != audioFormat){
										ast_log(LOG_ERROR,"SIP: Peer Answers with mismatched codec\n");
									}
									ast_debug(3,"sip tx codec: %x\n",audioFormat);
									/* Prepare to start sending Voice Frames */
									enable_sip_tx = 1;
									sip_prev_samples=0;
									SipSpeakerSetAudioTransport(sip_speaker,sip_sdp->audio->peer_media_port);
								}
								break;
							default:
								ast_debug(3,"Not Processing SIP 2xx Successful response code\n");
						}
						sip_speaker->state = SIP_STATE_NONE;
					}
					else if (responseCode>=400 && responseCode<=499)
					{  
						/* 4xx Request Failure */ 
						/* RFC3261 13.1 For final responses between 300 and 699, the ACK processing is done */
 						/* Especially need to ACK a 401, otherwise peer will resend a few times */

						/* Check/Get peer's tag as maybe first/new one peer sends */
						if( SipSetPeerTag(sip_speaker,buffer,bufferLen) == -1)
							ast_debug(3,"SIP: Getting Peer Tag had a Failure.\n");

						/* RFC3261 17.1.1.3 ACK Cseq is to be same as last Cseq INVITE */
						sip_speaker->cseqm[ACK] = sip_speaker->cseqm[INVITE] - 1; 
						if (!SipSpeakerAck(sip_speaker,username,4))
						{
							ast_log(LOG_ERROR,"Couldn't formulate/send SIP ACK\n");
							/* Nothing else to do, simply don't do any more SIP stuff */
						}
						switch(responseCode){
							case 400:
								sip_speaker->state = SIP_STATE_NONE;
								ast_log(LOG_ERROR,"SIP: 400 Bad Request. Not Processing.\n");
								break;
							case 401:
					                       ast_debug(3,"  sip invite 401 processing\n");

                                                                /* [v2.0]  Adding new way of detecting authentication method */
					                        ast_debug(3,"    - Checking for Auth Method of Basic\n");

                                                                struct BasicAuthData basic_data;

                                                                if (GetAuthSchemeBasic(buffer,bufferLen,&basic_data) == 0 )
                                                                {
					                            ast_debug(3,"    - Found Auth Method of Basic\n");
						                    ast_log(LOG_WARNING,"SIP Code does not yet support Basic Auth\n");
                                                                }
                                                                else
                                                                {
					                            ast_debug(3,"    - No Auth Method of Basic\n");
					                            ast_debug(3,"    - Checking for Auth Method of Digest\n");

                                                                    struct DigestAuthData digest_data;

                                                                    if (GetAuthSchemeDigest(buffer,bufferLen,&digest_data) == 0 )
                                                                    {
					                                ast_debug(3,"    - Found Auth Method of Digest\n");
									char *nc = NULL;
									char *cnonce = NULL;
									char *qop = NULL;
									char uri[64];
									sprintf(uri,"sip:%s@%s:%i", username,sip_speaker->ip,sip_port);
									char *method = "INVITE";
 
									ast_debug(5,"  Challenge Response Data- rx_realm: %s nonce: %s uri %s",\
										digest_data.rx_realm, digest_data.nonce,uri); 
                                              
									RtspPlayerDigestAuthorization(sip_speaker,username,\
											password, sip_realm,\
											digest_data.nonce, nc, cnonce, qop, uri, \
											digest_data.rx_realm, method, 1);

									/* Try Invite again w. Auth */
									if(sip_speaker->cseqm[INVITE] == 3)   
										ast_debug(3,"  Too many INVITEs \n");
									else
									{
										if (!SipSpeakerInvite(sip_speaker,username,audioFormat,1))
										{
											ast_log(LOG_ERROR,"SIP: Couldn't formulate/send INVITE\n");
											/* Nothing else to do, simply don't do any more SIP stuff */
										}
									}
                                                                    }
                                                                    else
                                                                    {
					                                ast_debug(3,"    - No Auth Method of Digest\n");
							                ast_log(LOG_ERROR,"No Basic/Digest Authentication header/data present\n");
                                                                    }
                                                                }

#ifdef OLD_AUTH_SCHEME
								if (CheckHeaderValue(buffer,bufferLen,"WWW-Authenticate","Basic realm="))
								{
									ast_log(LOG_WARNING,"SIP Code does not yet support Basic Auth\n");
								}
								else if (CheckHeaderValue(buffer,bufferLen,"WWW-Authenticate","Digest"))
								{
									struct DigestAuthData digest_data;

									if(GetAuthHeaderData(sip_speaker,buffer,bufferLen,&digest_data) == -1)
									{
									    ast_log(LOG_ERROR,"SIP: WWW-Authenticate header missing\n");
									}
									char *nc = NULL;
									char *cnonce = NULL;
									char *qop = NULL;
									char uri[64];
									sprintf(uri,"sip:%s@%s:%i", username,sip_speaker->ip,sip_port);
									char *method = "INVITE";
 
									ast_debug(5,"  GetChallenge Response- rx_realm: %s nonce: %s uri %s",\
										digest_data.rx_realm, digest_data.nonce,uri); 
                                              
									RtspPlayerDigestAuthorization(sip_speaker,username,\
											password, sip_realm,\
											digest_data.nonce, nc, cnonce, qop, uri, \
											digest_data.rx_realm, method, 1);

									/* Try Invite again w. Auth */
									if(sip_speaker->cseqm[INVITE] == 3)   
										ast_debug(3,"Too many INVITEs \n");
									else
									{
										if (!SipSpeakerInvite(sip_speaker,username,audioFormat,1))
										{
											ast_log(LOG_ERROR,"SIP: Couldn't formulate/send INVITE\n");
											/* Nothing else to do, simply don't do any more SIP stuff */
										}
									}
								}
#endif
								break;
							case 420:
								sip_speaker->state = SIP_STATE_NONE;
								ast_debug(3,"420 Bad Extension. Not Processing.\n");
								break;
							default:
								sip_speaker->state = SIP_STATE_NONE;
								ast_debug(3,"SIP Not Processing these 4xx Request Failure response codes\n");
						}
					}
					else 
					{
						ast_debug(3,"SIP Not Processing 5xx Server Failure nor 6xx Global Failures response code\n");
						sip_speaker->state = SIP_STATE_NONE;
					}

					/* done with received SIP message */
					bufferLen =0;
					break;
			    	case SIP_STATE_NONE:
					/* Get received SIP message */
					bufferLen =0;
					if (!RecvResponse(sip_speaker->fd,buffer,&bufferLen,bufferSize,&temp))
					{
						ast_log(LOG_ERROR,"SIP: failed to read unsolicted request buffer.\n"); 
						break;
					}
					ast_debug(3,"-sip rx req from peer\n%s",buffer); 
					if (strncmp(buffer,"BYE",3)==0) {
						ast_debug(1,">BYE\n"); 
						/* Send OK back to peer */
						if( SipSpeakerReply(sip_speaker,buffer,bufferLen,username,ip,sip_port,"BYE")==1)
							enable_sip_tx=0;
					      //ast_debug(1,"<BYE\n"); //changed [v2.0]
						}
					else if (strncmp(buffer,"INFO",4)==0) {
						ast_debug(1,">INFO\n"); 
						/* Send OK back to peer */
						if( SipSpeakerReply(sip_speaker,buffer,bufferLen,username,ip,sip_port,"BYE")==1)
							ast_debug(3,"send OK\n");
			                	//ast_debug(1,"<INFO\n"); //change [v2.0]
					}
					else if (strncmp(buffer,"CANCEL",5)==0) {
						ast_debug(1,">CANCEL\n"); 
				              //ast_debug(1,"<CANCEL\n"); //change [v2.0]
					}
					else {
						ast_log(LOG_ERROR,"Unsupported SIP Request receive");
					}

					/* done with received SIP message */
					bufferLen =0;
					break;
			}/* end of SIP States */
		} else if (rchan == NULL && outfd <0 && ms==0 && player->state!=RTSP_PLAYING) {
			/* log */
			ast_log(LOG_ERROR,"-timedout and not connected [%d]",outfd);
			/* Exit f timedout and not conected*/
			player->end = 1;
		} 

		/* If the playback has started */
		if (player->state==RTSP_PLAYING) 
		{
			/* If is not the first one */
			if (!ast_tvzero(rtcptv))
			{
				/* Check Rtcp timeout */
				if (ast_tvdiff_ms(ast_tvnow(),rtcptv)>10000)
				{
					/* If got audio */
					if (player->audioRtcp>0)
					{
						/* Create rtcp packet */
						MediaStatsRR(&player->audioStats,&rtcp);
						/* Reset media */
						MediaStatsReset(&player->audioStats);
						/* Send packet */
						send(player->audioRtcp, &rtcp, (ntohs(rtcp.common.length)+1)*4, 0);
						/* log */
					     /*	ast_log(LOG_DEBUG,"-Sent rtcp audio report [%d]\n",errno); OLD */
						ast_debug(2,"-sent rtcp audio report [%d]\n",errno); 
					}
					/* If got audio */
					if (player->videoRtcp>0)
					{
						/* Create rtcp packet */
						MediaStatsRR(&player->videoStats,&rtcp);
						/* Reset media */
						MediaStatsReset(&player->videoStats);
						/* Send packet */
						send(player->videoRtcp, &rtcp, (ntohs(rtcp.common.length)+1)*4, 0);
						/* log */
					     /*	ast_log(LOG_DEBUG,"-Sent rtcp video report [%d]\n",errno); OLD */
						ast_debug(2,"-sent rtcp video report [%d]\n",errno); 
					}
					/* Send OPTIONS */
                                        RtspPlayerOptions(player,url);
					/* log */
				     /*	ast_log(LOG_DEBUG,"-Sending OPTIONS and reseting RTCP timer\n"); OLD */
					ast_debug(2,"-sending options and resetting RTCP timer\n");
					/* Reset timeout value */
					rtcptv = ast_tvnow();
				}
			} else {
				/* log */
			     /*	ast_log(LOG_DEBUG,"-Init RTCP timer\n"); */
				ast_debug(2,"-init RTCP timer\n");
				/* Init timeout value */
				rtcptv = ast_tvnow();
			}
		}
	}

rstp_play_stop:

	/* log */
     /*	ast_log(LOG_DEBUG,"-rtsp_play end loop [%d]\n",res); OLD */
	ast_debug(2,"-rtsp_play end loop [%d]\n",res);

	/* Send rtsp teardown if something was setup */
	if (player->state>RTSP_DESCRIBE)
		/* Teardown */
		RtspPlayerTeardown(player);

	/* Send SIP BYE if in a dialog */
	if (sip_enable) {
		if (sip_speaker->in_a_dialog){
			ms = 500;
			int result;
			SipSpeakerBye(sip_speaker,username);
			result=ast_wait_for_input(sip_speaker->fd,ms); /*Wait for response */
			if(result>0){
				ast_debug(3,"rx bye response\n");
				bufferLen =0; /* TEMP */
				if (!RecvResponse(sip_speaker->fd,buffer,&bufferLen,bufferSize,&temp))
					ast_debug(3,"Couldn't get BYE response from buffer\n");
				else{
					ast_debug(3, "\n%s\n",buffer);
					/* Check for response code */
					responseCode = GetResponseCode(buffer,bufferLen,1);
					ast_debug(3,"-SIP Bye response code [%d]\n",responseCode);

					if (responseCode==401){
						SipSetPeerTag(sip_speaker,buffer,bufferLen);
						sip_speaker->cseqm[ACK] = sip_speaker->cseqm[BYE] - 1;
						SipSpeakerAck(sip_speaker,username,4);

                                                /* [v2.0]  Adding new way of detecting authentication method */
					        ast_debug(3,"  sip bye 401 Processing\n");
					        ast_debug(3,"    - Checking for Auth Method of Basic\n");
                                                struct BasicAuthData basic_data;
                                                if (GetAuthSchemeBasic(buffer,bufferLen,&basic_data) == 0 )
                                                {
					            ast_debug(3,"    - Found Auth Method of Basic\n");
						    ast_log(LOG_WARNING,"SIP Code does not yet support Basic Auth\n");
                                                }
                                                else
                                                {
					            ast_debug(5,"    - No Auth Method of Basic\n");
					            ast_debug(5,"    - Checking for Auth Method of Digest\n");

                                                    struct DigestAuthData digest_data;
                                                    if (GetAuthSchemeDigest(buffer,bufferLen,&digest_data) == 0 )
                                                    {
					                ast_debug(3,"    - Found Auth Method of Digest\n");
							char *nc = NULL;
							char *cnonce = NULL;
							char *qop = NULL;
							char uri[64];
							sprintf(uri,"sip:%s@%s:%i", username,sip_speaker->ip,sip_port);
							char *method = "BYE";

							ast_debug(5,"  input data for challenge response- rx_realm: %s nonce: %s uri %s",\
								digest_data.rx_realm, digest_data.nonce,uri);

							RtspPlayerDigestAuthorization(sip_speaker,username,password, sip_realm,\
									digest_data.nonce, nc, cnonce, qop, uri, \
									digest_data.rx_realm, method, 1);

							/* Try Bye again w. Auth */
							SipSpeakerBye(sip_speaker,username);
                                                    }
                                                    else
                                                    {
					                ast_debug(3,"    - No Auth Method of Digest\n");
							ast_log(LOG_ERROR,"No Basic/Digest Authentication header/data present\n");
                                                    }
                                                }

#ifdef OLD_AUTH_SCHEME
				                if (CheckHeaderValue(buffer,bufferLen,"WWW-Authenticate","Basic realm="))
						{
							ast_log(LOG_WARNING,"SIP Code does not yet support Basic Auth\n");
						}
						else if (CheckHeaderValue(buffer,bufferLen,"WWW-Authenticate","Digest"))
						{
							struct DigestAuthData digest_data;
							if(GetAuthHeaderData(sip_speaker,buffer,bufferLen,&digest_data) == -1)
							{
								ast_log(LOG_ERROR,"WWW-Authenticate header missing\n");
							}
							char *nc = NULL;
							char *cnonce = NULL;
							char *qop = NULL;
							char uri[64];
							sprintf(uri,"sip:%s@%s:%i", username,sip_speaker->ip,sip_port);
							char *method = "BYE";

							ast_debug(5,"  GetChallenge Response- rx_realm: %s nonce: %s uri %s",\
								digest_data.rx_realm, digest_data.nonce,uri);

							RtspPlayerDigestAuthorization(sip_speaker,username,password, sip_realm,\
									digest_data.nonce, nc, cnonce, qop, uri, \
									digest_data.rx_realm, method, 1);

							/* Try Bye again w. Auth */
							SipSpeakerBye(sip_speaker,username);
						}
#endif
					}
				}
			}
			sip_speaker->in_a_dialog=0;
		}
        }

	if (sip_sdp && sip_enable)
		DestroySDP(sip_sdp);
	ast_debug(3,"-sip tx vf count pre:%i post:%i error:%i\n",pre_enable_vf_tx_count,post_enable_vf_tx_count,sip_tx_error_count);
	/*
	 * PORT 17.5 restructure sendFrame. No longer malloc'd */
	/* Free frame */
     /*	if (sendFrame) OLD */
		/* Free memory */
	     /*	free(sendFrame); OLD */
	     /*	ast_free(sendFrame); * PORT17.3 */

	/* If ther was a sdp */
	if (sdp)
		/* Destroy it */
		DestroySDP(sdp);

rtsp_play_clean:
	/* Close sockets */
	RtspPlayerClose(player);
	if(sip_enable)
		RtspPlayerClose(sip_speaker);

rtsp_play_end:
	/* Destroy player */
	RtspPlayerDestroy(player);
	if(sip_enable)
		RtspPlayerDestroy(sip_speaker);

	/* log */
	ast_log(LOG_NOTICE,"<rtsp-sip main loop\n");

	/* Exit */	
	return res;
}

static int rtsp_tunnel(struct ast_channel *chan,char *ip, int port, char *url)
{
	struct sockaddr_in sendAddr;
	struct ast_frame *f;

	int infds[1];
	int outfd;
	int rtsp;

	int state = RTSP_TUNNEL_CONNECTING;
	char request[1024];
	char buffer[16384];
     /*	char *i;  PORT 17.3 compiler warning not used, so removed */
	int  bufferSize = 16383; /* One less for finall \0 */
	int  bufferLen = 0;
	int  responseLen = 0;
	int  contentLength = 0;

        struct SDPContent* sdp = NULL; /* PORT 17.3: init to NULL to remove compiler warning */

	int  isSDP;

	int end = 0;
	int ms = 10000;
	int flags;


	/* open socket */
	rtsp = socket(PF_INET,SOCK_STREAM,0);

	/* empty addres */
	memset(&sendAddr,0,sizeof(struct sockaddr_in));

	/* Set data */
	sendAddr.sin_family	 = AF_INET;
	sendAddr.sin_addr.s_addr = INADDR_ANY;
	sendAddr.sin_addr.s_addr = inet_addr(ip);
	sendAddr.sin_port	 = htons(port);

	/* Get flags */
	flags = fcntl(rtsp,F_GETFD);

	/* Set socket non-blocking */
	fcntl(rtsp,F_SETFD,flags | O_NONBLOCK);

	/* Connect */
	if (connect(rtsp,(struct sockaddr *)&sendAddr,sizeof(sendAddr))<0)
		/* Exit */
		return 0;

	/* Prepare request */
	snprintf(request,1024,"GET %s HTTP/1.0\r\nUser-Agent: app_rtsp\r\n Accept: application/x-rtsp-tunnelled\r\nPragma: no-cache\r\nCache-Control: no-cache\r\n\r\n",url);


	/* Set arrays */
	infds[0] = rtsp;

	/* Loop */
	while(!end)
	{
		/* No output */
		outfd = -1;
		/* Read from channels and fd*/
		if (ast_waitfor_nandfds(&chan,1,infds,1,NULL,&outfd,&ms))
		{
			/* Read frame */
			f = ast_read(chan);

			/* If failed */
			if (!f) 
				/* exit */
				break;
			
			/* If it's a control channel */
			if (f->frametype == AST_FRAME_CONTROL) 
				/* Check for hangup */
			     /*	if (f->subclass == AST_CONTROL_HANGUP) OLD */
				if (f->subclass.integer == AST_CONTROL_HANGUP)
					/* exit */
					end = 1;
			/* free frame */
			ast_frfree(f);
		} else if (outfd==rtsp) {
			/* Depending on state */	
			switch (state)
			{
				case RTSP_TUNNEL_CONNECTING:
					/* Send request */
					if (!SendRequest(rtsp,request,&end))
						/* exit*/
						break;
					/* It has been opened and sent*/
					state = RTSP_TUNNEL_NEGOTIATION;	
					break;
				case RTSP_TUNNEL_NEGOTIATION:
					/* Read into buffer */
					if (!RecvResponse(rtsp,buffer,&bufferLen,bufferSize,&end))
						break;
					/* Process */
					while (1)
					{	
						/* If not reading content */
						if (contentLength==0)
						{
							/* Search end of response */
							if ( (responseLen=GetResponseLen(buffer)) == 0 )
								/*Exit*/
								break;
							/* Does it have content */
							contentLength = GetHeaderValueInt(buffer,responseLen,"Content-Length");	
							/* Is it sdp */
							if (CheckHeaderValue(buffer,responseLen,"Content-Type","application/sdp"))
								/* SDP */
								isSDP = 1;
							else
								/* NO SDP*/
								isSDP = 0;
							/* If we have the sdp already */
							if (sdp && HasHeader(buffer,responseLen,"RTP-Info"))
								/* RTP */
								state = RTSP_TUNNEL_RTP;
							/* Get new length */
							bufferLen -= responseLen;
							/* Move data to begining */
						     /*	memcpy(buffer,buffer+responseLen,bufferLen); OLD */
							memmove(buffer,buffer+responseLen,bufferLen);
			
						/* If there is enough data */	
						} else if (bufferLen>=contentLength) {
							/* If it's the sdp */
							if (isSDP)
								/* Create SDP */
								sdp = CreateSDP(buffer,contentLength,0);
							/* Get new length */
							bufferLen -= contentLength;
							/* Move data to begining */
						     /*	memcpy(buffer,buffer+contentLength,bufferLen); OLD */
							memmove(buffer,buffer+contentLength,bufferLen);
							/* Reset content */
							contentLength = 0;
						} else
							break;
					}
					break;
				case RTSP_TUNNEL_RTP:
					break;
			}
		} else if (state==RTSP_TUNNEL_CONNECTING) 
			/* Exit f timedout and not conected*/
			end = 1;
	}

	/* If there was a sdp */
	if (sdp)
		/* Destroy it */
		DestroySDP(sdp);

	/* Close socket */
	close(rtsp);

	/* Exit */	
	return 0;
}

/* static int app_rtsp_sip(struct ast_channel *chan, void *data) OLD. */
static int app_rtsp_sip(struct ast_channel *chan, const char *data) /* PORT 17.3 REVISED the type for data */
{
	struct ast_module_user *u;
	char *uri;
	char *ip;
	char *hostport;
	char *url;
	char *i;
	char *username;
	char *password;
	int  rtsp_port=0;
	int  res=0;
	int  isIPv6=0;

	int sip_enable;
	char *sip_realm;
	int sip_port;


	/* [17.x NEW]. 
	 * Get arguments instead via macros. See example: app_dial.c 
	 * Note: app.h says to use ast_app_separate_args()
	 * but app_dial.c didn't, so won't here either.
	 *
	 * Add sip parameters: realm string and sip port.
	 *  Note: Terminology-wise, uri vs. url, this original code refers to an
	 *        rtsp uri as rtsp://rtsp-url. 
	 */
	char *parse;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(rtsp_uri);
		AST_APP_ARG(sip_enable);
		AST_APP_ARG(sip_realm);
		AST_APP_ARG(sip_port);
	);
	parse = ast_strdupa(data ?: "");
	AST_STANDARD_APP_ARGS(args, parse);

	ast_debug(3,"ARGs: RTSP URI %s. SIP Realm %s SIP Listen Port %s\n",args.rtsp_uri,args.sip_realm,args.sip_port); /*tjl*/

	/* [17.x NEW]. See if there are any args for sip realm */
	if(!args.sip_realm)
		sip_enable = 0; 
	else
		sip_enable = atoi(args.sip_enable);

	if(!args.sip_realm)
		sip_realm = "None"; 
	else
		sip_realm = args.sip_realm;

	/* [17.x NEW]. See if there are any args for sip port */
	if(!args.sip_port)
		args.sip_port = "5060"; 

	sip_port = atoi(args.sip_port);

	/* Get data */
     /*	uri = (char*)data; OLD */
	uri = args.rtsp_uri; /* PORT 17.3 new way */

	/* Get proto part */
	if ((i=strstr(uri,"://"))==NULL)
	{
		ast_log(LOG_ERROR,"RTSP ERROR: Invalid rtsp uri %s\n",uri);
		return 0;
	}

	/* Increase rtsp_url */
	url = i + 3; 

	/* Check for username and password */
	if ((i=strstr(url,"@"))!=NULL)
	{
		/* Get user and password info */
	     /*	username = strndup(url,i-url); was */
		username = ast_strndup(url,i-url);
		/* Remove from rtsp_url */
		url = i + 1;

		/* Check for password */
		if ((i=strstr(username,":"))!=NULL)
		{
			/* Get username */
			i[0] = 0;
			/* Get password */
			password = i + 1;
		} else {
			/* No password */
			password = NULL;
		}
	} else {
		/* No username or password */
		username = NULL;
		password = NULL;
	}

	/* Get server part */
	if ((i=strstr(url,"/"))!=NULL)
	{
		/* Assign server */
	     /*	hostport = strndup(url,i-url); was */
		hostport = ast_strndup(url,i-url);
		/* Get rtsp url */
		url = i;
	} else {
		/* all is server */
	     /*	ip = strdup(url); OLD. */
	     /*	ip = ast_strdup(url); This is overwritten below so not needed.Not freed either */
		/* Get root */	
		hostport = "/";
	}

	/* Get the ip */
	ip = hostport;

	/* Check if it is ipv6 */
	if (ip[0]=='[')
	{
		/* Is ipv6*/
		isIPv6 = 1;
		/* Skip first */
		ip++;
		/* Find closing bracket */
		i=strstr(ip,"]");
		/* Remove from server */
		i[0]=0;
		/* check if there is a rtsp_port after */
		if (i[1]==':')
			/* Get port */
			rtsp_port = atoi(i+2);
	} else {
		/* Get port */
		if ((i=strstr(ip,":"))!=NULL)
		{
			/* Get port */
			rtsp_port = atoi(i+1);
			/* Remove from server */
			i[0] = 0;
		}
	}

	ast_debug(3,"IP: %s RTSP port: %i Username: %s Passwd: %s URL_Path: %s isIPv6: %i, SIP Enable: %i, SIP Realm: %s, port: %i\n",ip,rtsp_port,username,password,url,isIPv6,sip_enable, sip_realm,sip_port);

	/* 
	 * PORT17.3
	 * Other apps do not call ast_module_user_add().
	 * Asterisk starts this module by calling pbx_exec()@pbx_app.c which calls
	 * __ast_module_user_add(); executes the module and on module return calls
	 * __ast_module_user_remove().  It would seem these calls in this module are unnecessary
	 * but not sure, so will leave in place for now.
	 */
	/* Lock module */
      	u = ast_module_user_add(chan);

	/* Depending on protocol */
	if (strncmp(uri,"http",4)==0) {
		/* if no port */
		if (!rtsp_port)
			/* Default */
			rtsp_port = 80;
		/* Play */
		res = rtsp_tunnel(chan,ip,rtsp_port,url);

	} else if (strncmp(uri,"rtsp",4)==0) {
		/* if no port */
		if (!rtsp_port)
			/* Default */
			rtsp_port = 554;
		/* Play */
		res = main_loop(chan,ip,rtsp_port,url,username,password,isIPv6,sip_enable,sip_realm,sip_port); /* name change */

	} else
		ast_log(LOG_ERROR,"RTSP ERROR: Unknown protocol in rtsp uri %s\n",uri);
	
	/* Unlock module*/
       	ast_module_user_remove(u);

	/* Free ip */
     /*	free(hostport); OLD */
	ast_free(hostport); /* PORT 17.3 */
	/* Free username */
	if (username)
	     /*	free(username); OLD */
		ast_free(username); /* PORT 17.3 */

	/* Exit */
	return res;
}


static int unload_module(void)
{
	int res;

	/* 
	 * PORT17.3. UnRegister as an xml app. (old way works too) 
	 */
     /*	res = ast_unregister_application(name_rtsp_sip); OLD */
        res = ast_unregister_application(app); /* PORT 17.3 */

	ast_module_user_hangup_all();

	return res;
}

static int load_module(void)
{
	/* 
	 * PORT17.3. New way: Register as an xml app. (old way works too) 
	 */
	int res;
	res = ast_register_application_xml(app, app_rtsp_sip);
	return res;

   /*	return ast_register_application(name_rtsp_sip, app_rtsp_sip, syn_rtsp_sip, des_rtsp_sip); OLD */
}

/* 
 * PORT17.3 
 * Update the following to the new way
 * (Old way works as well) 
 */
/* AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "RTSP-SIP applications"); */
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "RTSP-SIP Application",
	.support_level = AST_MODULE_SUPPORT_UNKNOWN,  /* PORT17.3. Old was  AST_MODULE_SUPPORT_CORE */
	.load = load_module,
	.unload = unload_module,
);
