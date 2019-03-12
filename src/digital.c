/*
 * ---------------------------------------------------------------------
 *  Lazy - play a CD and print actual track informations into stdout
 * ---------------------------------------------------------------------
 *
 * File: digital.c
 *
 * Author: Lucas Correia Villa Real <lucasvr@gobolinux.org>
 *
 * ---------------------------------------------------------------------
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 * ---------------------------------------------------------------------
 */

#include "playcd.h"
#include "digital.h"
#include "utils.h"
#include "dsp.h"

#include "benrpi.h"

/**
 *  digital cd play mode
 */
int read_cdtrack(int track_no, int total_tracks, int cdrom_fd)
{
        int bytes, i, q;
        int *p2, *p;
        int buf1[cdrom_info.ibufsize], buf2[cdrom_info.ibufsize];
        int length_min, length_sec;
        struct CDROM_MSF_STRUCT *track;
        HERE();
        /*
         * listen for the SIGINT signal for skipping tracks and for the
         * SIGQUIT signal to leave the program
         */
       // if (track_no == (total_tracks - 1))
                signal(SIGINT, suicide);
        //else
         //       signal(SIGINT, skip_track);
        //signal(SIGQUIT, suicide);

        /* print some information for the 'track_no' track */
        track = calculate_offsets(track_no, &length_min, &length_sec);

        track_no++;
        if (track_no < cdrom_info.min || track_no > cdrom_info.max)
                return -1;

        track_no -= cdrom_info.min;
        bytes = 0;
        p1 = buf1;
        p2 = buf2;
        q = 0;

        read_cdaudio(cdrom_info.starts[track_no], cdrom_info.blocks, (char *) p1, cdrom_fd);
        playing = 1;
        /* main loop */
        for (i = cdrom_info.starts[track_no] + cdrom_info.bufstep;
                        (i < cdrom_info.starts[track_no + 1]) && (GLOBAL_TIME != ABORT_CONSTANT);
                        i += cdrom_info.bufstep) {
                while (TAKE_A_BREAK)
                        usleep(100000);
                /* jitter correction */
                q = 0;
                if (cdrom_info.starts[track_no + 1] > i + 1)
                        do {
                                if ((i + cdrom_info.blocks) < cdrom_info.starts[track_no + 1]) {
                                        read_cdaudio(i, cdrom_info.blocks, (char *) p2, cdrom_fd);
                                } else {
                                        read_cdaudio(i, cdrom_info.starts[track_no + 1] - i, (char *) p1, cdrom_fd);
                                }
                        } while (((cdrom_info.read_buffer_size = cd_jc(p1, p2)) == -1) && (q++ < cdrom_info.retrys)
                                        && ++cdrom_info.retries);
                else
                        cdrom_info.read_buffer_size = 0;

                if (cdrom_info.read_buffer_size == -1) {
                        cdrom_info.read_buffer_size = cdrom_info.bufstep * CD_FRAMESIZE_RAW;
#ifdef DEBUG
                        printf (" > [read_cdtrack] :: jitter error near block %d\n",i);
#endif
                }

                if (bytes + cdrom_info.read_buffer_size> (cdrom_info.starts[track_no + 1]
                                - cdrom_info.starts[track_no]) * CD_FRAMESIZE_RAW)
                        cdrom_info.read_buffer_size = (cdrom_info.starts[track_no + 1] - cdrom_info.starts[track_no])
                                        * CD_FRAMESIZE_RAW - bytes;

                if (cdrom_info.read_buffer_size > 0) {
                        write(audio_fd, p1, cdrom_info.read_buffer_size);
                        bytes += cdrom_info.read_buffer_size;
                } else
                        break;

                p = p1;
                p1 = p2;
                p2 = p;
        }

        /* dump last bytes */
        if (bytes < (cdrom_info.starts[track_no + 1] - cdrom_info.starts[track_no]) * CD_FRAMESIZE_RAW) {
                cdrom_info.read_buffer_size = (cdrom_info.starts[track_no + 1] - cdrom_info.starts[track_no])
                                * CD_FRAMESIZE_RAW - bytes;
                bytes += cdrom_info.read_buffer_size;
                write(audio_fd, p1, cdrom_info.read_buffer_size);
        }

        GLOBAL_TIME = 0;
     //   DOUT("->free: %s:%d\n", __FILE__, __LINE__);
        free(track);
     //   DOUT("<-free: %s:%d\n", __FILE__, __LINE__);
        return 0;
}

/*
 * read_cdaudio - reads the audio from the CD (digital input) and put it into the dsp buffer
 */
void read_cdaudio(int lba, int num, char *buf, int cdrom_fd)
{
        struct CDROM_READ_AUDIO ra;

        /* read 'n' bytes from the audio buffer into the 'noise' buffer directly */

        ra.addr.lba = lba;
        ra.addr_format = CDROM_LBA;
        ra.nframes = num;
        ra.buf = (unsigned char *) buf;
        if (ioctl(cdrom_fd, CDROMREADAUDIO, &ra))
                return;

}

/* 
 * cd_jc - do the jitter correction
 */
int cd_jc(p1, p2)
        int *p1, *p2;
{
        int n, d;

        n = 0;
        if (!cdrom_info.overlap)
                return (cdrom_info.bufstep * CD_FRAMESIZE_RAW);

        do
                d = cd_jc1(p1, p2 + n);
        while ((d == -1) && (n++ < cdrom_info.ofs));

        if (--n < 0)
                n = 0;
        if (d == -1)
                return (d);
        else
                return (d - n * sizeof(int));
}

/*
 * cd_jc1 - looks for offset in p1 where can find a subset of p2
 */
int cd_jc1(int *p1, int *p2)
{
        int *p = p1 + cdrom_info.ibufsize - IFRAMESIZE - 1;
        int n = 0;

        while (n < IFRAMESIZE * cdrom_info.overlap && *p == *--p)
                n++;

        if (n >= IFRAMESIZE * cdrom_info.overlap) {
                /* jitter correction is useless on silence */
                n = cdrom_info.bufstep * CD_FRAMESIZE_RAW;
        } else {
                /* jitter correction */
#ifdef DEBUG
                printf ("jitter correction\n");
#endif
                n = 0;
                p = p1 + cdrom_info.ibufsize - cdrom_info.keylen / sizeof(int) - 1;
                while ((n < IFRAMESIZE * (1 + cdrom_info.overlap)) && memcmp(p, p2, cdrom_info.keylen)) {
                        p--;
                        n++;
                }
                if (n >= IFRAMESIZE * (1 + cdrom_info.overlap)) {
                        /* no match */
                        return (-1);
                }
                n = sizeof(int) * (p - p1);
        }
        return (n);
}

int calculate_cdrom_globals(int cdrom_fd)
{
        struct CDROM_TOC_HEADER th;
        struct CDROM_TOC_ENTRY te;

        /* set buffering information */
        cdrom_info.keylen = 12;
        cdrom_info.ofs = 12;
        cdrom_info.retrys = 10; /* max jitter correction retries - defaults to 40 */
        cdrom_info.retries = 0; /* number of corrections made */
        cdrom_info.overlap = 0;
        cdrom_info.blocks = 8;		// 8
        cdrom_info.bufsize = CD_FRAMESIZE_RAW * cdrom_info.blocks;
        cdrom_info.ibufsize = cdrom_info.bufsize / sizeof(int);
        cdrom_info.bufstep = cdrom_info.blocks - cdrom_info.overlap;

        /*	printf ("----------------------------------\n");
         printf ("bufsize = %d * %d = %d\n", CD_FRAMESIZE_RAW, cdrom_info.blocks, cdrom_info.bufsize);
         printf ("ibufsize = %d / %d = %d <-----\n", cdrom_info.bufsize, sizeof(int), cdrom_info.ibufsize);
         printf ("bufstep = %d - %d = %d\n", cdrom_info.blocks, cdrom_info.overlap, cdrom_info.bufstep);
         printf ("----------------------------------\n"); */
        HERE();
        /* get info from the cdrom_fd */
#if defined(__linux__) || defined(sun)
        ioctl(cdrom_fd, CDROMREADTOCHDR, &th);
        cdrom_info.min = th.cdth_trk0;
        cdrom_info.max = th.cdth_trk1;
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        ioctl (cdrom_fd, CDIOREADTOCHEADER, &th);
        cdrom_info.min = th.starting_track;
        cdrom_info.max = th.ending_track;
#endif

        if (cdrom_info.max - cdrom_info.min < 2) {
                printf("no cd...\n");
                return 1;
        }

        HERE(); //dieser failed. warum? versucht der unendlich speicher zu allocieren wenn keine cd da is?? "ohne cd" test
        printf("gonna alloc: %d\n", cdrom_info.max - cdrom_info.min + 2);
        cdrom_info.starts = (int *) malloc((cdrom_info.max - cdrom_info.min + 2) * sizeof(int));
        if (!cdrom_info.starts) {
                /*err_quit*/printf("malloc: %s %X\n", strerror(errno), cdrom_info.starts);
                HERE();
                return 2;
        }

        cdrom_info.types = (char *) malloc((cdrom_info.max - cdrom_info.min + 2) * sizeof(char));
        if (!cdrom_info.types) {
                /*err_quit*/printf("malloc: %s\n", strerror(errno));
                HERE();
                return 3;
        }
        /* read the TOC entry */
        specific_read_toc_entry(cdrom_fd, &te);
        HERE();
        /* read the LEADOUT TOC entry */
        specific_read_leadout(cdrom_fd, &te);
        return 0;
}

void specific_read_toc_entry(int cdrom_fd, struct CDROM_TOC_ENTRY *te)
{
        int i;
#if defined(__linux__) || defined(sun)
        for (i = cdrom_info.min; i <= cdrom_info.max; i++) {
                /* read the TOC entry for every track */
                te->cdte_track = i;
                te->cdte_format = CDROM_LBA;
                if (ioctl(cdrom_fd, CDROMREADTOCENTRY, te))
                        err_quit("CDROMREADTOCENTRY: %s", strerror(errno));

                cdrom_info.starts[i - cdrom_info.min] = te->cdte_addr.lba;
                cdrom_info.types[i - cdrom_info.min] = te->cdte_ctrl & CDROM_DATA_TRACK;
        }
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        for (i = cdrom_info.min; i <= cdrom_info.max; i++)
        {
                /* read the TOC entry for every track */
                te->track = (unsigned char ) i;
                te->address_format = CD_LBA_FORMAT;

                if (ioctl (cdrom_fd, CDIOREADTOCENTRY, te))
                err_quit ("CDIOREADTOCENTRY: %s", strerror (errno));

                cdrom_info.starts[i - cdrom_info.min] = te->entry.addr.lba;
                cdrom_info.types[i - cdrom_info.min] = te->entry.control & CDROM_DATA_TRACK;
        }
#endif
}

void specific_read_leadout(int cdrom_fd, struct CDROM_TOC_ENTRY *te)
{
#if defined(__linux__) || defined(sun)
        te->cdte_track = CDROM_LEADOUT;
        te->cdte_format = CDROM_LBA;
        if (ioctl(cdrom_fd, CDROMREADTOCENTRY, te))
                err_quit("CDROMREADTOCENTRY: %s", strerror(errno));

        cdrom_info.starts[cdrom_info.max - cdrom_info.min + 1] = te->cdte_addr.lba;
        cdrom_info.types[cdrom_info.max - cdrom_info.min + 1] = te->cdte_ctrl & CDROM_DATA_TRACK;
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        te->track = CDROM_LEADOUT;
        te->address_format = CD_LBA_FORMAT;
        if (ioctl (cdrom_fd, CDIOREADTOCENTRY, te))
        err_quit ("CDIOREADTOCENTRY: %s", strerror (errno));

        cdrom_info.starts[cdrom_info.max - cdrom_info.min + 1] = te->entry.addr.lba;
        cdrom_info.types[cdrom_info.max - cdrom_info.min + 1] = te->entry.control & CDROM_DATA_TRACK;
#endif
}
