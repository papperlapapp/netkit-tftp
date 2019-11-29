/*
 * Copyright (c) 1983 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * From: @(#)tftp.c	5.10 (Berkeley) 3/1/91
 */
char tftp_rcsid[] = "$Id: tftp.c,v 1.10 2000/07/22 19:06:29 dholland Exp $";

/* Many bug fixes are from Jim Guyton <guyton@rand-unix> */

/*
 * TFTP User Program -- Protocol Machines
 */
#include "tftpsubs.h"
#include "../version.h"

#include <errno.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NDEBUG
const int debug = 1;
#endif

extern struct sockaddr_storage s_inn; /* filled in by main */
extern socklen_t s_inn_len;
extern int f; /* the opened socket */
extern int trace;
extern int verbose;
extern int rexmtval;
extern int maxtimeout;
extern sigjmp_buf toplevel;
void sendfile(int fd, char *name, char *modestr);
void recvfile(int fd, char *name, char *modestr);

static struct sockaddr_storage from; /* most recent remote address */
static socklen_t fromlen;

static char ackbuf[PKTSIZE];
static int timeout;
static sigjmp_buf timeoutbuf;

static int makerequest(int request, char *name, struct tftphdr *tp, char *mode);
static void nak(int error);
static void tpacket(const char *s, struct tftphdr *tp, int n);
static void startclock(void);
static void stopclock(void);
static void printstats(const char *direction, unsigned long amount);

static void timer(int signum)
{
    (void)signum;

    timeout += rexmtval;
    if (timeout >= maxtimeout) {
        printf("Transfer timed out.\n");
        siglongjmp(toplevel, -1);
    }
    siglongjmp(timeoutbuf, 1);
}

/*
 * Send the requested file.
 */
void sendfile(int fd, char *name, char *mode)
{
    register struct tftphdr *ap; /* data and ack packets */
    struct tftphdr *dp;
    volatile int size = 0;
    volatile u_int16_t block = 0;
    int n;
    volatile unsigned long amount = 0;
    volatile int convert; /* true if doing nl->crlf conversion */
    FILE *file;
    volatile int firsttrip = 1;

    startclock();  /* start stat's clock */
    dp = r_init(); /* reset fillbuf/read-ahead code */
    ap = (struct tftphdr *)ackbuf;
    file = fdopen(fd, "r");
    convert = !strcmp(mode, "netascii");

    memcpy(&from, &s_inn, sizeof(from));
    fromlen = s_inn_len;

    mysignal(SIGALRM, timer);
    do {
        if (firsttrip) {
            size = makerequest(WRQ, name, dp, mode) - 4;
        } else {
            /*      size = read(fd, dp->th_data, SEGSIZE);   */
            size = readit(file, &dp, convert);
            if (size < 0) {
                nak(errno + 100);
                break;
            }
            dp->th_opcode = htons((u_short)DATA);
            dp->th_block = htons((u_short)block);
        }
        timeout = 0;
        (void)sigsetjmp(timeoutbuf, 1);
    send_data:
        if (trace) {
            tpacket("sent", dp, size + 4);
        }

        n = sendto(f, dp, size + 4, 0, (struct sockaddr *)&from, fromlen);
        if (n != size + 4) {
            perror("tftp: sendto");
            goto abort;
        }

#ifndef NDEBUG
        // NOTE: test retransmit 2 time each odd block only! CK
        if ((debug &&  ((block % 2) == 1))) {
            (void)sendto(f, dp, size + 4, 0, (struct sockaddr *)&from, fromlen);
            (void)sendto(f, dp, size + 4, 0, (struct sockaddr *)&from, fromlen);
        }
#endif

        read_ahead(file, convert);
        for (;;) {
            alarm(rexmtval);
            do {
                fromlen = sizeof(from);
                n = recvfrom(f, ackbuf, sizeof(ackbuf), 0,
                             (struct sockaddr *)&from, &fromlen);
            } while (n <= 0);
            alarm(0);
            if (n < 0) {
                perror("tftp: recvfrom");
                goto abort;
            }

            if (trace)
                tpacket("received", ap, n);
            /* should verify packet came from server */
            ap->th_opcode = ntohs(ap->th_opcode);
            ap->th_block = ntohs(ap->th_block);
            if (ap->th_opcode == ERROR) {
                printf("Error code %d: %s\n", ap->th_code, ap->th_msg);
                goto abort;
            }
            if (ap->th_opcode == ACK) {
                if (ap->th_block == block) {
                    break;
                }
                /* On an error, try to synchronize
                 * both sides.
                 */
                synchnet(f, trace);
                if (ap->th_block == (block - 1)) {
                    goto send_data;
                }
            }
        }
        if (firsttrip) {
            firsttrip = 0;
        } else {
            amount += size;
            if (size != SEGSIZE) {
                break;
            }
        }
        block++;
    } while (1);
abort:
    fclose(file);
    stopclock();
    if (amount > 0)
        printstats("Sent", amount);
    initsock(from.ss_family); /* Synchronize address family. */
}

/*
 * Receive a file.
 */
void recvfile(int fd, char *name, char *mode)
{
    register struct tftphdr *ap;
    struct tftphdr *dp;
    volatile int size = 0;
    volatile u_int16_t block = 1;
    int n;
    volatile unsigned long amount = 0;
    volatile int firsttrip = 1;
    FILE *file;
    volatile int convert; /* true if converting crlf -> lf */

    startclock();
    dp = w_init();
    ap = (struct tftphdr *)ackbuf;
    file = fdopen(fd, "w");
    convert = !strcmp(mode, "netascii");

    memcpy(&from, &s_inn, sizeof(from));
    fromlen = s_inn_len;

    mysignal(SIGALRM, timer);
    do {
        if (firsttrip) {
            size = makerequest(RRQ, name, ap, mode);
            firsttrip = 0;
        } else {
            ap->th_opcode = htons((u_short)ACK);
            ap->th_block = htons((u_short)(block));
            size = 4;
            block++;
        }
        timeout = 0;
        (void)sigsetjmp(timeoutbuf, 1);
    send_ack:
        if (trace)
            tpacket("sent", ap, size);

        n = sendto(f, ackbuf, size, 0, (struct sockaddr *)&from, fromlen);
        if (n != size) {
            alarm(0);
            perror("tftp: sendto");
            goto abort;
        }
        write_behind(file, convert);
        for (;;) {
            alarm(rexmtval);
            do {
                fromlen = sizeof(from);
                n = recvfrom(f, dp, PKTSIZE, 0, (struct sockaddr *)&from,
                             &fromlen);
            } while (n <= 0);
            alarm(0);
            if (n < 0) {
                perror("tftp: recvfrom");
                goto abort;
            }

            if (trace)
                tpacket("received", dp, n);
            /* should verify client address */
            dp->th_opcode = ntohs(dp->th_opcode);
            dp->th_block = ntohs(dp->th_block);
            if (dp->th_opcode == ERROR) {
                printf("Error code %d: %s\n", dp->th_code, dp->th_msg);
                goto abort;
            }
            if (dp->th_opcode == DATA) {
                if (dp->th_block == block) {
                    break; /* have next packet */
                }
                /* On an error, try to synchronize
                 * both sides.
                 */
                synchnet(f, trace);
                if (dp->th_block == (block - 1)) {
                    goto send_ack; /* resend ack */
                }
            }
        }
        /*      size = write(fd, dp->th_data, n - 4); */
        size = writeit(file, &dp, n - 4, convert);
        if (size < 0) {
            nak(errno + 100);
            break;
        }
        amount += size;
    } while (size == SEGSIZE);

    ap->th_opcode = htons((u_short)ACK); /* has seen err msg */
    ap->th_block = htons((u_short)block);
    (void)sendto(f, ackbuf, 4, 0, (struct sockaddr *)&from, fromlen);

abort:
    write_behind(file, convert); /* flush last buffer */
    fclose(file);
    stopclock();
    if (amount > 0)
        printstats("Received", amount);
    initsock(from.ss_family); /* Synchronize address family. */
}

int makerequest(int request, char *name, struct tftphdr *tp, char *mode)
{
    register char *cp;
    int len;

    tp->th_opcode = htons((u_short)request);
    cp = tp->th_stuff;
    len = strlen(name);
    memcpy(cp, name, len);
    cp += len;
    *cp++ = '\0';
    len = strlen(mode);
    memcpy(cp, mode, len);
    cp += len;
    *cp++ = '\0';
    return (cp - (char *)tp);
}

struct errmsg
{
    int e_code;
    const char *e_msg;
} errmsgs[] = {{EUNDEF, "Undefined error code"},
               {ENOTFOUND, "File not found"},
               {EACCESS, "Access violation"},
               {ENOSPACE, "Disk full or allocation exceeded"},
               {EBADOP, "Illegal TFTP operation"},
               {EBADID, "Unknown transfer ID"},
               {EEXISTS, "File already exists"},
               {ENOUSER, "No such user"},
               {-1, 0}};

/*
 * Send a nak packet (error message).  Error code passed in is one of the
 * standard TFTP codes, or a UNIX errno offset by 100.
 */
void nak(int error)
{
    register struct errmsg *pe;
    register struct tftphdr *tp;
    int length;

    tp = (struct tftphdr *)ackbuf;
    tp->th_opcode = htons((u_short)ERROR);
    tp->th_code = htons((u_short)error);
    for (pe = errmsgs; pe->e_code >= 0; pe++)
        if (pe->e_code == error)
            break;
    if (pe->e_code < 0) {
        pe->e_msg = strerror(error - 100);
        tp->th_code = EUNDEF;
    }
    strcpy(tp->th_msg, pe->e_msg);
    length = strlen(pe->e_msg) + 4;
    if (trace)
        tpacket("sent", tp, length);
    if (sendto(f, ackbuf, length, 0, (struct sockaddr *)&from, fromlen) !=
        length)
        perror("nak");
}

static void tpacket(const char *s, struct tftphdr *tp, int n)
{
    static const char *opcodes[] = {"#0", "RRQ", "WRQ", "DATA", "ACK", "ERROR"};
    register char *cp, *file;
    u_short op = ntohs(tp->th_opcode);

    if (op < RRQ || op > ERROR)
        printf("%s opcode=%x ", s, op);
    else
        printf("%s %s ", s, opcodes[op]);
    switch (op) {

    case RRQ:
    case WRQ:
        // DeadStores! n -= 2;
        file = cp = tp->th_stuff;
        cp = cp + strlen(cp);
        printf("<file=%s, mode=%s>\n", file, cp + 1);
        break;

    case DATA:
        printf("<block=%u, %d bytes>\n", ntohs(tp->th_block), n - 4);
        break;

    case ACK:
        printf("<block=%u>\n", ntohs(tp->th_block));
        break;

    case ERROR:
        printf("<code=%d, msg=%s>\n", ntohs(tp->th_code), tp->th_msg);
        break;
    }
}

struct timeval tstart;
struct timeval tstop;
struct timezone zone;

void startclock(void) { gettimeofday(&tstart, &zone); }

void stopclock(void) { gettimeofday(&tstop, &zone); }

void printstats(const char *direction, unsigned long amount)
{
    double delta;
    /* compute delta in 1/10's second units */
    delta = ((tstop.tv_sec * 10.) + (tstop.tv_usec / 100000)) -
            ((tstart.tv_sec * 10.) + (tstart.tv_usec / 100000));
    delta = delta / 10.; /* back to seconds */
    printf("%s %ld bytes in %.1f seconds", direction, amount, delta);
    if (verbose)
        printf(" [%.0f bits/sec]", (amount * 8.) / delta);
    putchar('\n');
}
