/*
 * ---------------------------------------------------------------------
 *  Lazy - play a CD and print actual track informations into stdout
 * ---------------------------------------------------------------------
 *
 * File: playcd.c
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

#include "wrappers.h"
#include "cddb_get.h"
#include "random.h"
#include "digital.h"
#include "dsp.h"
#include "playcd.h"

#include "pthread.h"
#include "poll.h"
//#include "benlcd.h"
#include <stddef.h>

//#include <inttypes.h>
#include <math.h>
//#include <stdio.h>
#include <time.h>

#include "benrpi.h"

#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <execinfo.h>

int track = 0;

// make this global, because its bad style and i'm lazy
int xSong;
int cd_tracks;
int song_list[100];

#define BENS_STUFF 1
#ifndef DOUT
#define DOUT(...) fprintf(stdout, __VA_ARGS__)
//#define DOUT(...)
#endif
pthread_t lcdThread;
pthread_t pollThread;
pthread_mutex_t poll_mutex;
pthread_mutex_t spi_mutex;

int run = 0;

#ifdef BENS_STUFF

#define SPI_SPEED 500000

//////////////////////////////////////////////////////////////

#define BUT_REW  0
#define BUT_PPLAY  1
#define BUT_STOP  2
#define BUT_FFWD  3
#define BUT5  4
#define BUT6  5
#define BUT_CNT  6

struct gpio {
        int fd;
        int bcmNum;
        char path[128];
};

#define LCDLINELEN 16
#define LCDNUMBEROFLINES 4

struct lcd_line {
        char path[128];
        //   int fd;
        unsigned char len;
};

struct lcdisplay {
        int lcd_fd;
        struct lcd_line lines[4];
};

struct gpio gpios[BUT_CNT];
struct lcdisplay lcd;

void lcd_print_line(unsigned char line_idx, char *string);
#if 0
//
// Set up a memory regions to access GPIO
//
static void setup_io()
{
        /* open /dev/mem */
        if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
                printf("can't open /dev/mem \n");
                exit(-1);
        }

        /* mmap GPIO */
        gpio_map = mmap(
                        NULL,             //Any adddress in our space will do
                        BLOCK_SIZE,//Map length
                        PROT_READ | PROT_WRITE,// Enable reading & writting to mapped memory
                        MAP_SHARED,//Shared with other processes
                        mem_fd,//File to map
                        GPIO_BASE//Offset to GPIO peripheral
        );

        close(mem_fd);//No need to keep mem_fd open after mmap

        if (gpio_map == MAP_FAILED) {
                printf("mmap error %d\n", (int) gpio_map); //errno also set!
                exit(-1);
        }

        // Always use volatile pointer!
        gpio = (volatile unsigned *) gpio_map;

} // setup_io
#endif

void cleanup()
{
        int i = 0;
        // open
        for (i = 0; i < BUT_CNT; i++) {
                if (0 >= gpios[i].fd)
                        close(gpios[i].fd);
        }

//        for (i = 0; i < LCDNUMBEROFLINES; i++) {
//                if (0 >= lcd.lines[i].fd) {
//                        close(lcd.lines[i].fd);
//                }
//        }
        if (lcd.lcd_fd) {
                close(lcd.lcd_fd);
        }
}

// quick and dirty init
void init_gpios_from_sysfs()
{
        int i = 0;
        unsigned int tmp = 0;
        HERE();
#if 0
        gpios[BUT_REW].bcmNum = 9;
        gpios[BUT_PPLAY].bcmNum = 1;
        gpios[BUT_STOP].bcmNum = 4;
        gpios[BUT_FFWD].bcmNum = 14;
        gpios[BUT5].bcmNum = 11;
        gpios[BUT6].bcmNum = 10;
#endif

        gpios[BUT_REW].bcmNum = 4;
        gpios[BUT_PPLAY].bcmNum = 17;
        gpios[BUT_STOP].bcmNum = 14; //4;
        gpios[BUT_FFWD].bcmNum = 18; //17;
        gpios[BUT5].bcmNum = 1; //21;

        for (i = 0; i < BUT_CNT; i++) {
                char tmp[128];

                sprintf(gpios[i].path, "/sys/class/gpio/gpio%d", gpios[i].bcmNum);

                memset(tmp, 0x00, 128);
                snprintf(tmp, 127, "echo %d > /sys/class/gpio/export", gpios[i].bcmNum);
                tmp[127] = 0;
                system(tmp);

                memset(tmp, 0x00, 128);
                snprintf(tmp, 127, "echo rising > %s/edge", gpios[i].path);
                tmp[127] = 0;
                system(tmp);

                memset(tmp, 0x00, 128);
                snprintf(tmp, 127, "echo in > %s/direction", gpios[i].path);
                tmp[127] = 0;
                system(tmp);
                HERE();

                printf("GPIO configured: %s %d\n", gpios[i].path, gpios[i].bcmNum);
        }
        HERE();

        // open
        for (i = 0; i < BUT_CNT; i++) {
                char tmp[256];
                memset(tmp, 0x00, sizeof(tmp));
                sprintf(tmp, "%s/value", gpios[i].path);
                gpios[i].fd = open(tmp, O_RDONLY | O_NONBLOCK);
                if (0 >= gpios[i].fd) {
                        printf("fucked up\n");
                        cleanup();
                        exit(-1);
                }
        }

}

int getFirstSetGpio(fd_set *set)
{
        int i = 0;
        for (i = 0; i < BUT_CNT; i++) {
                if (FD_ISSET(gpios[i].fd, set)) {
                        return (i);
                }
        }
        return (-1);
}

//////////////////////////////////////////////////////////7
void *select_thread(void *nichts)
{
        //struct pollfd fdset[1];
        //int nfds = 1;
        fd_set rfdset;
        fd_set wfdset;
        fd_set efdset;

        static int wasHere = 0;

        int rc = -1;
        int n = 0;

        puts("thread comes");

        for (n = 0; n < BUT_CNT; n++) {
                int val;

                lseek(gpios[n].fd, 0, SEEK_SET);
                read(gpios[n].fd, &val, 1);
        }

        puts("fd is done!");

        while (run) {
                n = 0;
                struct timeval to;
                to.tv_sec = 2;
                to.tv_usec = 100000;

                FD_ZERO(&rfdset);
                FD_ZERO(&wfdset);
                FD_ZERO(&efdset);

                for (n = 0; n < BUT_CNT; n++) {
                        FD_SET(gpios[n].fd, &rfdset);
                        FD_SET(gpios[n].fd, &wfdset);
                        FD_SET(gpios[n].fd, &efdset);
                }

                rc = select(gpios[BUT_CNT - 1].fd + 1, &rfdset, &wfdset, &efdset, &to);
                int inGpio = getFirstSetGpio(&efdset);
                usleep(1);
                if (rc < 0) {
                        printf("error\n");
                } else if (rc == 0) {
                        puts("rc == 0");
                } else if (0 <= inGpio) {
                        char val = -1;

                        if (pthread_mutex_trylock(&poll_mutex)) {
                                // puts("mutex is mine");
                                // sagt kernel doc
                                lseek(gpios[inGpio].fd, 0, SEEK_SET);
                                read(gpios[inGpio].fd, &val, 1);
                                // printf("value <%c>\n", val);
                                if ('0' < val)
                                        switch (inGpio) {
                                        DOUT("gpio select: %d\n", inGpio);
                                case BUT_REW:
                                        DOUT("inGpio %d rev %d>\n", gpios[inGpio].bcmNum, wasHere);

                                        if (wasHere) {
                                                DOUT("xSong : %d -= 2\n", xSong);
                                                if (xSong > 1) {
                                                        xSong -= 2;
                                                } else {
                                                        DOUT("cd_tracks: %d - 2\n", cd_tracks);
                                                        xSong = cd_tracks - 2;
                                                }
                                                //skip_track(SIGUSR1);
                                                GLOBAL_TIME = ABORT_CONSTANT;
                                        }
                                        break;
                                case BUT_PPLAY:
                                        DOUT("inGpio %d play? %d>\n", gpios[inGpio].bcmNum, wasHere);
                                        TAKE_A_BREAK = !TAKE_A_BREAK;
                                        break;
                                case BUT_STOP:
                                        DOUT("inGpio %d stop? %d>\n", gpios[inGpio].bcmNum, wasHere);
                                        xSong = cd_tracks + 1;
                                        GLOBAL_TIME = ABORT_CONSTANT;
                                        break;
                                case BUT_FFWD:
                                        DOUT("inGpio %d forward %d>\n", gpios[inGpio].bcmNum, wasHere);
                                        if (wasHere) {
                                                DOUT("xSong goes on : %d \n", xSong);
                                                //skip_track(SIGUSR1);
                                                GLOBAL_TIME = ABORT_CONSTANT;
                                        }
                                        break;
                                case BUT5:
                                        DOUT("inGpio %d %d>\n", gpios[inGpio].bcmNum, wasHere);
                                        break;
                                case BUT6:
                                        DOUT("inGpio %d %d>\n", gpios[inGpio].bcmNum, wasHere);
                                        break;
                                default:
                                        printf("unknown button %d>\n", inGpio);
                                        }
                                pthread_mutex_unlock(&poll_mutex);
                        }
                        wasHere = 1;
                }
                usleep(50000);
        }
        // clean up
        cleanup();

        run = 0;
        return NULL;
}

//#define LCDNUMBEROFLINES 2

struct cdTime {
        unsigned char trackNr;
        unsigned char min;
        unsigned char sec;
};

struct cdTime totalCdTime;
struct cdTime currentTrack;
struct cdTime currentRunning;

void lcd_print_line(unsigned char line_idx, char *string)
{
        struct spi_ioc_transfer tr;
        unsigned char *tx = NULL;
        unsigned char *rx = NULL;

        int ret = 0;
        int len = strlen(string);

        pthread_mutex_lock(&spi_mutex);
        tx = malloc(len + 6);

        if (!tx) {
                DOUT("no mem for tx %s:%d", __FILE__, __LINE__);
                HERE();
                goto FREE_TX;
        }

        rx = malloc(len + 6);
        if (!rx) {
                DOUT("no mem for rx %s:%d", __FILE__, __LINE__);
                HERE();
                goto FREE_RX_TX;
        }

        memset(tx, 0x00, sizeof(len + 6));
        memset(rx, 0x00, sizeof(len + 6));

        if (len > 110) {
                len = 110;
        } else if (len <= 0) {
                DOUT("error strange string for me: %s\n", string);
                DOUT("error in %s:%d", __func__, __LINE__);
                HERE();
                goto FREE_RX_TX;
        }

        tx[0] = (unsigned char) 0x01;
        tx[1] = (unsigned char) line_idx;
        tx[2] = (unsigned char) len;
        tx[3] = (unsigned char) 0x02;
        memcpy(tx + 4, string, len);

        tx[len + 4] = (unsigned char) 0x03;
        tx[len + 5] = 0;
        printf("[%X] sending (%d): [%X] %d %d [%x] \"%s\" [%X] \n",lcd.lcd_fd, len, tx[0], tx[1], tx[2], tx[3], &tx[4], tx[len + 4]);

//        if (0 > lcd.lines[line_idx].fd) {
//                DOUT("ERROR: no file opened for lcd line %d", line_idx);
//                DOUT("error in %s:%d", __func__, __LINE__);
//                return;
//        }
#if 0
        char txx[] = { 0x01, 1, 5, 0x02,  'h', 'a', 'l','l', 'o', 0x03};
        len = sizeof(txx) - 5;
#endif
        memset(&tr, 0x00, sizeof(tx));
        tr.tx_buf = (unsigned long) tx;
        tr.rx_buf = (unsigned long) rx;
        tr.len = len + 5;
        tr.delay_usecs = 0;
        tr.speed_hz = SPI_SPEED; //500000;
        tr.bits_per_word = 8;
        //   HERE();
        ret = ioctl(lcd.lcd_fd, SPI_IOC_MESSAGE(1), &tr);
        //    ret = write(lcd.lines[line_idx].fd, string, tmp);

        if (1 > ret) {
                int n = 0;
                DOUT("error no: %d\n", errno);
                DOUT("error in %s:%d\n", __func__, __LINE__);
                for (n = 0; n < len; n++) {
                        if (!(n % 6))
                                puts("");
                        printf("%.2X ", rx[n]);
                }
        }

        FREE_RX_TX: ;
        free(rx);
        FREE_TX: ;
        free(tx);

        pthread_mutex_unlock(&spi_mutex);
}

#if 1
void *lcdThreadFunc(void *nuthin)
{
//#define VERBPRINT DOUT
#define VERBPRINT(...)

        char dsp_out_buff_3[LCDLINELEN + 1];
        char dsp_out_buff_4[LCDLINELEN + 1];

        memset(dsp_out_buff_3, 0x00, sizeof(dsp_out_buff_3));
        memset(dsp_out_buff_4, 0x00, sizeof(dsp_out_buff_4));

        struct timeval te;
        unsigned long oldSec = 0;

        // use run from poll thread, cause if that is gonna die we go with it
        while (run) {

                /******** LINE 3 *************/
                /* remain_time -> running_time */
                gettimeofday(&te, NULL); // get current time
                if (!playing) {
                        oldSec = te.tv_sec;
                }

                int all_sec = (currentTrack.sec) + (currentTrack.min * 60);
                int play_time = (currentRunning.sec) + (currentRunning.min * 60);
                int remaining = (all_sec - play_time);

                sprintf(dsp_out_buff_3, "%2d  %2d:%02d->%2d:%02d", currentTrack.trackNr, currentRunning.min,
                                currentRunning.sec, remaining / 60, remaining % 60);

                if (oldSec != te.tv_sec) {
                        if (!TAKE_A_BREAK) {
                                if (0 == currentRunning.sec) {
                                        currentRunning.min--;
                                        currentRunning.sec = 60;
                                }
                                currentRunning.sec -= (te.tv_sec - oldSec);
                        }
                        oldSec = te.tv_sec;
                }

                /******** LINE 4 **************/
                /* track / track  track_length */
                sprintf(dsp_out_buff_4, "%2d  %2d:%02d->%2d:%02d", cd_tracks, currentTrack.min, currentTrack.sec,
                                totalCdTime.min, totalCdTime.sec);
                /********************************/

#if LCDNUMBEROFLINES > 2
                //    strcpy(display3, "empty");
                VERBPRINT("<3>3: %s\n", dsp_out_buff_3);
                lcd_print_line(3, dsp_out_buff_3);
#endif
                usleep(300000);
#if LCDNUMBEROFLINES > 3
                //    strcpy(display4, "line");
               VERBPRINT("<3>4: %s\n", dsp_out_buff_4);
               lcd_print_line(4, dsp_out_buff_4);
#endif
                VERBPRINT("<3>**********************\n");

                // update lcd around twice a second or you'll wait forever to read the song title
                usleep(300000);
        }
        DOUT("lcd thread closing");
        return NULL;

}
#endif




void init_spi()
{
        int tmp = 0;
        int ret = 0 ;
        HERE();
        // lcd.lcd_fd = open("/dev/lcdude", O_WRONLY);
        lcd.lcd_fd = open("/dev/spidev0.0", O_RDWR);
        if (0 > lcd.lcd_fd) {
                DOUT("init of lcd device failed\n");
                return;
        }
#if 0
#if 0
        /*
         * spi mode
         */
        tmp =
        ret = ioctl(lcd.lcd_fd, SPI_IOC_WR_MODE, &tmp);
        if (ret == -1)
        pabort("can't set spi mode");

        ret = ioctl(lcd.lcd_fd, SPI_IOC_RD_MODE, &mode);
        if (ret == -1)
        pabort("can't get spi mode");

#endif
        /*
         * bits per word
         */
        tmp = 8;
        int ret = ioctl(lcd.lcd_fd, SPI_IOC_WR_BITS_PER_WORD, &tmp);
        if (ret == -1)
        printf("can't set bits per word\n");
#if 0
        ret = ioctl(lcd.lcd_fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
        if (ret == -1)
        p("can't get bits per word");

#endif
#endif
        /*
         * max speed hz
         */
        tmp = SPI_SPEED;
        ret = ioctl(lcd.lcd_fd, SPI_IOC_WR_MAX_SPEED_HZ, &tmp);
        if (ret == -1)
        printf("can't set max speed hz\n");
#if 0
#if 0
        ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
        if (ret == -1)
        pabort("can't get max speed hz");

        printf("spi mode: %d\n", mode);
        printf("bits per word: %d\n", bits);
        printf("max speed: %d Hz (%d KHz)\n", speed, speed / 1000);

        //transfer(fd);
#endif
        HERE();
#endif
        HERE();
        lcd_print_line(1, "HALLO GOD!");
        usleep(100000);
        HERE();
        lcd_print_line(2, "HALLO DEVIL!");
        usleep(100000);
        HERE();
        lcd_print_line(3, "HALLO TOM!");
        usleep(100000);
        HERE();
        lcd_print_line(4, "HALLO JERRY!");
        usleep(100000);
        HERE();

        //        for (i = 0; i < LCDNUMBEROFLINES; i++) {
        //                sprintf(lcd.lines[i].path, "/sys/class/lcdude/lcdude/lcd_line_%d", i + 1);
        //                lcd.lines[i].fd = open(lcd.lines[i].path, O_RDWR);
        //                lcd.lines[i].len = LCDLINELEN;
        //                if (0 >= lcd.lines[i].fd) {
        //                        DOUT("failed for %s\n",lcd.lines[i].path);
        //                        cleanup();
        //                        exit(-2);
        //                }
        //        }
        HERE();


        pthread_create(&lcdThread, NULL, lcdThreadFunc, NULL);

}


void gpio_init()
{

        pthread_mutex_init(&spi_mutex, NULL);
#if 0
        pthread_mutex_init(&lcdMutex, NULL);
        lcd = lcdInit(24, 25, 23, 17, 21, 22);
        puts("lcdInit done");
        pthread_create(&lcdThread, NULL, lcdThreadFunc, NULL);
        puts("lcdThread done");
#endif
        //   setup_io();

        init_gpios_from_sysfs();

        // init_io was called in lcdInit. so we use that here
        //INP_GPIO(18);

        //  GPIO_CLR = 1 << 18;

        puts("init");
        pthread_mutex_init(&poll_mutex, NULL);
        puts("mutex done");

        pthread_create(&pollThread, NULL, select_thread, NULL);

        puts("pollthread done");
        return;

}

#endif

/*
 *
 <!DOCTYPE html>
 <html>
 <body>
 <p>Hello, world!</p>
 </body>
 </html>
 */

void create_web_site()
{
        int n = 0;
        FILE *web = fopen("/var/www/index.html", "w");
        if (!web)
                return;
        fprintf(web, "<!DOCTYPE html>");
        fprintf(web, "<html>");
        fprintf(web, "<body><p>");
        for (n = 1; n < cd_tracks; n++) {
                fprintf(web, "%02d :: %s :: %s<br />", n, music[n].artist, music[n].title);
        }
        fprintf(web, "</p></body> </html>");
        fflush(web);
        fclose(web);
}

int prepare_cd_info(int *cdrom_fd)
{
        DIR *directory;
        HERE();
        /* get the file descriptor for the CD device */
        *cdrom_fd = open(CDDEV, O_RDONLY | O_NONBLOCK);
        if (*cdrom_fd < 0) {
                fprintf(stderr, "open %s: %s\n", CDDEV, strerror(errno));
                //   free_globals();
                //exit(EXIT_FAILURE);
                HERE();
                return 1;
        }
        HERE();

        if (calculate_cdrom_globals(*cdrom_fd)) {
                return 2;
        }

        HERE();
        /* check if our cddb path really exist */
        directory = opendir(REAL_PATH);
        if (!directory) {
                if (VERBOSE) {
                        printf("\nDirectory %s doesn't exist, trying to create it... ", REAL_PATH);
                        fflush(stdout);
                }
                HERE();
                /* try to create it */
                if ((mkdir(REAL_PATH, 0744)) < 0)
                        fprintf(stderr, "mkdir %s: %s\n", REAL_PATH, strerror(errno));
                else {
                        if (VERBOSE)
                                printf("Directory successfully created!\n\n");
                }

        } else
                closedir(directory);
        HERE();

        /* show intro */
        show_intro();

        return 0;
}

int get_track_info(int *START_TRACK, int track, int* cdrom_fd)
{
        int i, y, tmp;
        char *file_name;
        /* get the total tracks from the CD */
        int total_tracks = read_cdtoc(*cdrom_fd);
        if (total_tracks < 0) {
                printf("Error scanning the number of tracks in your CD.\n");
                return -1;
                //exit(EXIT_FAILURE);
        }
        /* save the total tracks from the CD into another variable */
        cd_tracks = total_tracks;
        //struct cdTime totalCdTime;
        totalCdTime.trackNr = total_tracks;
        totalCdTime.min = cdtoc[cd_tracks].min;
        totalCdTime.sec = cdtoc[cd_tracks].sec;
        /* check if START_TRACK <= total_tracks */
        if (*START_TRACK > total_tracks) {
                printf("Error: Track %02d doesn't exist in this album."
                                "The last track is %02d.\n\n", (*START_TRACK) + 1, total_tracks);
                ///free_globals();
                //exit(EXIT_FAILURE);
                return -2;
        }
        HERE();
        /* get the cd_id */
        unsigned int cd_id = cddb_disc_id(total_tracks);
        HERE();
        /* try to get the audio info from the hard disk.. */
        file_name = get_hd_name(REAL_PATH, cd_id);
        if (file_name) {
                /* open the 'file_name' and search for album information */
                get_info(file_name);
        } else {
                /* if could not, try to get it from the internet connection.. */
                file_name = get_inet_name(ADDRESS, PORT, cd_id, total_tracks);
                if (file_name) {

                        /* fine, now we have the hard disk access! so, let's use it's information */
                        file_name = get_hd_name(REAL_PATH, cd_id);
                        if (file_name)
                                get_info(file_name);
                        else {
                                for (track = 1; track <= total_tracks; track++) {
                                        music[track].name = strdup(UNRECOGNIZED);
                                        music[track].artist = strdup(UNKNOWN_ART);
                                        music[track].title = strdup("something");                // malloc(17);
                                        //sprintf(music[track].title, "Unknown Track %2d", track);
                                }
                        }

                } else {
                        for (track = 1; track <= total_tracks; track++) {
                                music[track].name = strdup(UNRECOGNIZED);
                                music[track].artist = strdup(UNKNOWN_ART);
                                music[track].title = strdup("something");                                 // malloc(17);
                                                                                                          //sprintf(music[track].title, "Unknown Track %2d", track);
                        }
                }
        }
        HERE();
#if 0
        /* if just want to see the tracks names in the CD */
        if (SHOWONLY) {
                for (xSong = 1; xSong <= total_tracks; xSong++) {
                        printf("[%02d] - %s\n", xSong, music[xSong].name);
                        free(music[xSong].name);
                        free(music[xSong].title);
                        free(music[xSong].artist);
                }
                printf("\n");
                free_globals();
                exit(EXIT_SUCCESS);
        }
#endif
        /* if have a playlist */
        if (PLAYLIST) {
                /* set the new total_tracks size */
                total_tracks = xSong;
        } else {
                /* just fill the playlist array with the normal track order */
                for (xSong = 0; xSong < total_tracks; xSong++)
                        song_list[xSong] = xSong + 1;
        }
        /* if we are going to randomize the play order */
        if (RANDOM) {
                /* to make the random list different than the last one! */
                randomize();
                for (xSong = 0; xSong < total_tracks; xSong++) {
                        /* get a track number from the Random() function */
                        y = Random(0, total_tracks);
                        tmp = song_list[xSong];
                        song_list[xSong] = song_list[y];
                        song_list[y] = tmp;
                }
        }
        /* check if we are not going to play any invalid track */
        for (xSong = 0; xSong < total_tracks; xSong++) {
                if ((song_list[xSong] <= 0) || (song_list[xSong] > cd_tracks)) {
                        printf("Error: invalid track %d\n", song_list[xSong]);
                        free_globals();
                        DOUT("->free: %s:%d\n", __FILE__, __LINE__);
                        for (i = 0; i < total_tracks; i++) {
                                if (music[i].name)
                                        free(music[i].name);
                                if (music[i].artist)
                                        free(music[i].artist);
                                if (music[i].title)
                                        free(music[i].title);
                        }
                        DOUT("<-free: %s:%d\n", __FILE__, __LINE__);
                        return -3;
                        //exit(EXIT_FAILURE);
                }
        }
        /* print the CD ID information */
        printf("CD ID: %08x || Total Tracks: %d\n\n", cd_id, total_tracks);
        /* print the playlist order */
        printf("Playlist: ");
        for (xSong = 0; xSong < total_tracks; xSong++)
                printf("%d ", song_list[xSong]);
        printf("\n");

        create_web_site();

        return total_tracks;
}

int play_song_list(int *cdrom_fd, int *total_tracks, int START_TRACK)
{

        /* play the track(s) */
        for (xSong = START_TRACK; xSong < *total_tracks; xSong++) {
                pthread_mutex_trylock(&poll_mutex);
                track = song_list[xSong];
                pthread_mutex_unlock(&poll_mutex);

                /* print the pretty information */
                printf("Playing CDDA stream from [%02d] - %s ...\n", track, music[track].name);

                HERE();
                DOUT("<2>track: %s\n", music[track].name);
                lcd_print_line(1, music[track].artist);
                lcd_print_line(2, music[track].title);

                currentTrack.trackNr = currentRunning.trackNr = track;

                HERE();
                /* play the song */
#if 0
                if (DIGITAL_EXTRACTION) {
                        HERE();
                        read_cdtrack(track - 1, total_tracks, *cdrom_fd);
                        HERE();
                } else {
                        HERE();
                        play_cd(track - 1, total_tracks, *cdrom_fd);

                        HERE();
                }
#else
                read_cdtrack(track - 1, *total_tracks, *cdrom_fd);
#endif
#if 0
                DOUT("->free: %s:%d\n", __FILE__, __LINE__);
                if (music[track].name)
                free(music[track].name);
                if (music[track].title)
                free(music[track].title);
                if (music[track].artist)
                free(music[track].artist);
                DOUT("<-free: %s:%d\n", __FILE__, __LINE__);
#endif
        }
#if 0 // do we want this here?
        int i = 0;
        DOUT("->free: %s:%d\n", __FILE__, __LINE__);
        for (i = 0; i < *total_tracks; i++) {
                if (music[i].name)
                free(music[i].name);
                if (music[i].artist)
                free(music[i].artist);
                if (music[i].title)
                free(music[i].title);
        }
        DOUT("<-free: %s:%d\n", __FILE__, __LINE__);
#endif
        return 2;
}

void param_eval(int argc, char** argv)
{
        int option;
        extern char* optarg;
        /* check if we're using extra args in the command line */
        while ((option = getopt(argc, argv, "vkcdt::f:rslo:h")) != -1) {
                switch (option) {
                case 'v':
                        /* verbose mode */
                        VERBOSE = 1;
                        break;
                case 'k':
                        /* keep the music playing when the program exit */
                        STOP_MUSIC = 0;
                        break;
                case 'c':
                        /* close the cd tray */
                        CLOSE_TRAY = 1;
                        break;
                case 'd':
                        /* will use digital extraction */
                        //          DIGITAL_EXTRACTION = 1;
                        //        open_dsp();
                        /** we will always do this */
                        break;
                case 'f':
                        if (optarg) {
                                DOUT("->free: %s:%d\n", __FILE__, __LINE__);
                                if (CDDEV)
                                        free(CDDEV);
                                CDDEV = strdup(optarg);
                                DOUT("<-free: %s:%d\n", __FILE__, __LINE__);
                        }
                        break;
                case 't':
                        /* show the seconds played */
                        SHOW_TIME = 1;
                        if (optarg) {
                                if (*optarg == 'r')
                                        SHOW_REMAINING = 1;
                                else if (*optarg == 'e')
                                        SHOW_REMAINING = 0;
                                else {
                                        printf("Error: invalid display mode for time.\n\n");
                                        free_globals();
                                        exit(EXIT_FAILURE);
                                }
                        }
                        break;
                case 'o':
                        /* wanna start playing from another track */
                        //    START_TRACK = atoi(optarg) - 1;
                        break;
                case 'r':
                        /* wanna listen in a random order */
                        RANDOM = 1;
                        break;
                case 's':
                        /* wanna show the tracks names only */
                        SHOWONLY = 1;
                        break;
                case 'l':
#if 0
                        /* wanna mount a playlist */
                        PLAYLIST = 1;
                        xSong = 0;
                        y = optind;

                        /* while has valid arguments, build the playlist */
                        while ((argv[y]) && (!strstr(argv[y], "-"))) {
                                song_list[xSong] = atoi(argv[y]);
                                xSong++;
                                y++;
                        }

                        if (!xSong) {
                                printf("Error: you must supply the tracks.\n\n");
                                free_globals();
                                exit(EXIT_FAILURE);
                        }

                        playlist_size = xSong;
#endif
                        break;
                case 'h':
                        /* show valid syntaxes */
                        show_intro();
                        printf("syntax: %s [options]\n", argv[0]);
                        printf("Available options:\n"
                                        "\t-h         : this help\n"
                                        "\t-v         : verbose mode\n"
                                        "\t-k         : keep the music playing when the program exit\n"
                                        "\t-c         : close the tray before play the disc\n"
                                        "\t-d         : use digital extraction\n"
                                        "\t-f <device>: overrides the CDROM <device> specified in ~/.lazyrc\n"
                                        "\t-t[r|e]    : show the seconds played from the track\n"
                                        "\t             e - display elapsed time\n"
                                        "\t             r - display remaining time\n"
                                        "\t             if none is specified, behave as set in ~/.lazyrc\n"
                                        "\t             (will not work with '-d' flag)\n"
                                        "\t-r         : play in random mode\n"
                                        "\t-s         : just list the track names, "
                                        "without playing any of them\n"
                                        "\t-l <list>  : play only <list> tracks\n"
                                        "\t-o <track> : start playing from track <track>\n"
                                        "example: $ %s -v -tr -l 4 5 7\n\n", argv[0]);

                        free_globals();
                        exit(EXIT_SUCCESS);
                        break;
                } /* switch */
        }
}

void almost_c99_signal_handler(int sig)
{
        void *array[10];
        unsigned long size;
        switch (sig) {
        case SIGABRT:
                fputs("Caught SIGABRT: usually caused by an abort() or assert()\n", stderr);
                break;
        case SIGFPE:
                fputs("Caught SIGFPE: arithmetic exception, such as divide by zero\n",
                stderr);
                break;
        case SIGILL:
                fputs("Caught SIGILL: illegal instruction\n", stderr);
                break;
        case SIGINT:
                fputs("Caught SIGINT: interactive attention signal, probably a ctrl+c\n",
                stderr);
                break;
        case SIGSEGV:
                fputs("Caught SIGSEGV: segfault\n", stderr);
                break;
        case SIGTERM:
        default:
                fputs("Caught SIGTERM: a termination request was sent to the program\n",
                stderr);
                break;
        }

        // get void*'s for all entries on the stack
        size = backtrace(array, 10);

        // print out all the frames to stderr
        fprintf(stderr, "Error: signal %d:\n", sig);
        backtrace_symbols_fd(array, size, STDERR_FILENO);
        _Exit(1);
}

void set_signal_handler()
{
        signal(SIGABRT, almost_c99_signal_handler);
        signal(SIGFPE, almost_c99_signal_handler);
        signal(SIGILL, almost_c99_signal_handler);
        signal(SIGINT, almost_c99_signal_handler);
        signal(SIGSEGV, almost_c99_signal_handler);
        signal(SIGTERM, almost_c99_signal_handler);
}

/*
 * here begins the scrambled code
 */

int main(argc, argv)
        int argc;char **argv;
{
        //unsigned cd_id;
        extern int optind; /* from getopt() */
        extern int opterr; /* from getopt() */
        extern char *optarg; /* from getopt() */

        char *home;
        char *temp;
        char *subdir;
        int option;

        int i, j;
        TAKE_A_BREAK = 0;

//#ifdef BENS_STUFF
        run = 1;
     //   gpio_init();
        init_spi();


        sleep(1);
        lcd_print_line(1, "Snurre CD - Cool oldschool cd player");
        lcd_print_line(2, "Please insert CD");
        HERE();
        set_signal_handler();
        sleep(1);
        sleep(1);
        sleep(1);

#if 0
        TAKE_A_BREAK = !TAKE_A_BREAK;
        while (TAKE_A_BREAK)
        usleep(100000);
#else
        // i am trying to debug here
        //just run
#endif
        VERBOSE = 1;
//#endif
////////////////////////////////////////////////////////////////7 BGH ben BEN BGBGBG

        /* set some global variables */
        home = getenv("HOME");
        if ((set_global_vars()) < 0) {
                printf("Could not find/create %s/%s\n"
                                "Please check if it exist, else copy it from the lazy/src/lazyrc.sample file\n", home,
                CONFIG_FILE);
                exit(EXIT_FAILURE);
        }

        /* check if we're using extra args in the command line */
        param_eval(argc, argv);

        // ALWAYS!!!!
        DIGITAL_EXTRACTION = 1;
        CLOSE_TRAY = 1;
        open_dsp();

        printf("strdup: %s\n", CFIG_PATH);
        /* put some stuff into memory */
        temp = strdup(CFIG_PATH);

        subdir = Calloc(strlen(temp), strlen(temp) * sizeof(char));
        REAL_PATH = Calloc(strlen(temp) + strlen(home), (strlen(temp) + strlen(home)) * sizeof(char));
        HERE();
        /* convert any tilde found in the CFIG_PATH to the $HOME string */
        if (temp[0] == '~') {
                for (i = 1, j = 0; i < (strlen(temp)); i++, j++)
                        subdir[j] = temp[i];
                subdir[j + 1] = '\0';
                sprintf(REAL_PATH, "%s%s", home, subdir);
        } else
                REAL_PATH = strdup(temp);
        //     DOUT("->free: %s:%d\n", __FILE__, __LINE__);
        free(temp);
        //    DOUT("<-free: %s:%d\n", __FILE__, __LINE__);

        /*******************************************
         ********************************************
         *****  I M P O R T A N T   P O I N T  ******
         ********************************************/
        /* listen for some signals */
        signal(SIGQUIT, suicide); /* quit program */

        int cdrom_fd = -1;
        int ret_prepare = 0;
        ret_prepare = prepare_cd_info(&cdrom_fd);
        while (0 != ret_prepare) {
                ret_prepare = prepare_cd_info(&cdrom_fd);
                DOUT("[%s:%d] preparing %d\n", __FILE__, __LINE__, ret_prepare);
                // lcd_print_16x2(1, "please insert CD");
                sleep(1);
        }
        DOUT("[%s:%d] preparing %d\n", __FILE__, __LINE__, ret_prepare);
        HERE();
        int START_TRACK = 0;

        /* get the total tracks from the CD */
        int total_tracks = get_track_info(&START_TRACK, track, &cdrom_fd);

        playing = 0;
        HERE();
        play_song_list(&cdrom_fd, &total_tracks, START_TRACK);

        /* close the CD tray */
        if (CLOSE_TRAY)
                close_tray(cdrom_fd);

        ////////////////////////////////////////////////77
        run = 0;
        pthread_join(pollThread, NULL);
        pthread_join(lcdThread, NULL);
        printf("retuned - freeing");

        puts("");
        Free(subdir);
        free_globals();
        exit(EXIT_SUCCESS);
}

/*
 * get_info - open the filename and put music title's into the global variable
 */
void get_info(file)
        char *file;
{
        char line[BUFFER_SIZE], name[BUFFER_SIZE];
        char *token = NULL, *tmp;
        int i, index = 1;
        FILE *f;

        /* try to open the file */
        f = fopen(file, "r");
        if (!f) {
                fprintf(stderr, "fopen %s: %s\n", file, strerror(errno));
                exit(EXIT_FAILURE);
        }

        /* read it line by line */
        while (!feof(f)) {
                if ((fgets(line, sizeof(line), f))) {
                        if (!(strstr(line, "DTITLE="))) {
                                /* check if is the music name.. */
                                if ((strstr(line, "TTITLE"))) {
                                        token = strtok(line, "=");
                                        if (!token) {
                                                printf("error: TTITLE has no arguments\n");
                                                continue;
                                        }

                                        token = strtok(NULL, "=");
                                        if (!token) {
                                                printf("error: TTITLE has no arguments\n");
                                                continue;
                                        }

                                        /* seek for the \r character */
                                        for (i = 0; i < strlen(token); i++) {
                                                if ((token[i] == '\n') || (token[i] == '\r'))
                                                        break;
                                        }
                                        token[i] = '\0';

                                        /* check if the last character is a space */
                                        if (artist[strlen(artist)] == ' ')
                                                snprintf(name, sizeof(name), "%s- %s", artist, token);
                                        else
                                                snprintf(name, sizeof(name), "%s - %s", artist, token);
                                        music[index].artist = strdup(artist);
                                        music[index].title = strdup(token);
                                        music[index].name = strdup(name);
                                        index++;
                                }
                                continue;
                        } else {
                                /* print the album name */
                                tmp = strtok(line, "=");
                                if (!tmp) {
                                        printf("error: no arguments given on %s\n", line);
                                        continue;
                                }
                                tmp = strtok(NULL, "=");
                                if (!tmp) {
                                        printf("error: no arguments given on %s\n", line);
                                        continue;
                                }
                                tmp = strtok(tmp, "/");
                                if (!tmp) {
                                        printf("error: no arguments given on %s\n", line);
                                        continue;
                                }

                                artist = strdup(tmp);
                                album = strdup(strtok(NULL, "/"));

                                /* verify if we have not an empty space in the end of the string */
                                if (artist[strlen(artist) - 1] == ' ')
                                        artist[strlen(artist) - 1] = '\0';

                                printf("Artist: %s   ", artist);
                                printf("Album name: %s\n", album);
                        }
                } /* if */
        } /* while */
}

/*
 * play_cd - analog play the 'track_no' track into 'device'
 */
void play_cd(int track_no, int total_tracks, int cdrom_fd)
{
        register int music_time = 0, seconds, retval;
        int minutes, length_min, length_sec;
        struct CDROM_MSF_STRUCT *track;
        char *error = NULL;

        /*
         * listen for the SIGINT signal for skipping tracks and for the
         * SIGQUIT signal to leave the program
         */
        // if (track_no == (total_tracks - 1)) {
        HERE();
        signal(SIGINT, suicide);
        //} else {
        //        HERE();
        //        signal(SIGINT, skip_track);
        //}
        signal(SIGQUIT, suicide);

        /* calculate the offsets */
        track = calculate_offsets(track_no, &length_min, &length_sec);

        /* set the total music time (in seconds) */
        music_time = (length_min * 60) + length_sec;

        /* try to play the 'track_no' track */
#if defined(__linux__) || defined(sun)
        retval = ioctl(cdrom_fd, CDROMPLAYMSF, track);
        error = "CDROMPLAYMSF";
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        retval = ioctl (cdrom_fd, CDIOCPLAYMSF, track);
        error = "CDIOCPLAYMSF";
#endif
        playing = 1;
        if (retval < 0) {
                perror(error);
                exit(EXIT_FAILURE);
        }

        //   char line1[17];
        HERE();
        //    sprintf(line1, "%02d:%02d\0", length_min, length_sec);
        //   line1[16] = 0;
        //   lcdPrint(1, line1);

        /* if wanna show the seconds played */
        if (SHOW_TIME) {
                if (SHOW_REMAINING) {

                        minutes = length_min;
                        seconds = length_sec;
                        for (GLOBAL_TIME = 0; GLOBAL_TIME < music_time; GLOBAL_TIME++) {
                                printf("\tRemaining Time: %02d:%02d\r", minutes, seconds--);
                                fflush(stdout);
                                if (seconds < 0) {
                                        --minutes;
                                        seconds = 59;
                                }

                                /* I'm not sleeping 1sec because of the printf + var calculus delay */
                                usleep(991000);
                        }
                } else {
                        minutes = 0;
                        seconds = 0;
                        for (GLOBAL_TIME = 0; GLOBAL_TIME < music_time; GLOBAL_TIME++) {
                                printf("\tElapsed Time: %02d:%02d\r", minutes, seconds);
                                seconds++;
                                fflush(stdout);
                                if (seconds == 60) {
                                        ++minutes;
                                        seconds = 0;
                                }

                                /* I'm not sleeping 1sec because of the printf + var calculus delay */
                                usleep(991000);
                        }
                }
        } else {
                /* just sleep the music time */
                sleep(music_time);
        }

        Free(track);
}

struct CDROM_MSF_STRUCT * calculate_offsets(track_no, length_min, length_sec)
        int track_no, *length_min, *length_sec;
{
        struct CDROM_MSF_STRUCT *track;
        int length_frame = 0;
        int offset_min = 0, offset_sec = 0, offset_frame = 0;
        int end_min = 0, end_sec = 0, end_frame = 0;
        int tmp1 = 0, tmp2 = 0;

        track = (struct CDROM_MSF_STRUCT *) malloc(sizeof(struct CDROM_MSF_STRUCT));
        *length_min = *length_sec = 0;
        HERE();
        /* set start min/sec/frame within the informations on the toc_entry struct (from cddb_get.c) */
        offset_min = cdtoc[track_no].min;
        offset_sec = cdtoc[track_no].sec;
        offset_frame = cdtoc[track_no].frame;

        /*  --- calculate the end frame for 'track_no' track --- */
        /* get the total 'track_no' time */
        tmp1 = offset_frame; /* tmp1 == start frame offset */
        tmp1 += offset_sec * CD_FRAMES; /* there are CD_FRAMES in 1sec - defined in cdrom.h */
        tmp1 += offset_min * CD_SECS * CD_FRAMES; /* CD_SECS is also a constant in cdrom.h */

        /* calculate the total time from the next track */
        tmp2 = cdtoc[track_no + 1].frame;
        tmp2 += cdtoc[track_no + 1].sec * CD_FRAMES;
        tmp2 += cdtoc[track_no + 1].min * CD_SECS * CD_FRAMES;

        tmp2 -= tmp1; /* get the total track length in frames 		*/
        *length_min = tmp2 / (CD_SECS * CD_FRAMES); /* calculate the number of minutes of the track	*/
        tmp2 %= CD_SECS * CD_FRAMES; /* get the left-over frames						*/
        *length_sec = tmp2 / CD_FRAMES; /* get the number of seconds of the track		*/
        length_frame = tmp2 % CD_SECS; /* get the left-over frames again				*/
        HERE();
        /* get the end_frame offset */
        end_frame += offset_frame + length_frame;
        if (end_frame >= CD_FRAMES) /* check if end_frame is in it's valid limits */
        {
                end_sec += end_frame / CD_FRAMES;
                end_frame %= CD_SECS;
        }

        /* get the end_sec offset */
        end_sec += offset_sec + *length_sec;
        if (end_sec >= CD_SECS) /* check if end_sec is in it's valid limits */
        {
                end_min += end_sec / CD_SECS;
                end_sec %= CD_SECS;
        }
        HERE();
        /* get the end_min offset */
        end_min += offset_min + *length_min;
        /* --- end of the track frame calculus --- */

        /*
         * set toc_msf info with 'track_no+1' offsets, obtained from the
         * toc_entry struct + previous calculus
         */
#if defined(__linux__) || defined(sun)
        track->cdmsf_min0 = offset_min; /* start minute				*/
        track->cdmsf_sec0 = offset_sec; /* start second				*/
        track->cdmsf_frame0 = offset_frame; /* start frame offset		*/
        track->cdmsf_min1 = end_min; /* minute offset to end at	*/
        track->cdmsf_sec1 = end_sec; /* second offset to end at	*/
        track->cdmsf_frame1 = end_frame; /* frame  offset to end at	*/
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        track->start_m = offset_min;
        track->start_s = offset_sec;
        track->start_f = offset_frame;
        track->end_m = end_min;
        track->end_s = end_sec;
        track->end_f = end_frame;
#endif

        /* print some information for the 'track_no' track */
        printf("\tTrack time: %d:%02d\n\n", *length_min, *length_sec);

        currentRunning.min = currentTrack.min = *length_min;
        currentRunning.sec = currentTrack.sec = *length_sec;

        /* if DEBUG mode is set, print the offsets and extra information, as you can see below */
#ifdef DEBUG
        printf ("\tOffsets are at %d minutes, %d seconds and %d frames\n"
                        "\tLength is %d minutes, %d seconds and %d frames\n"
                        "\tEnd is at %d minutes, %d seconds and %d frames\n\n",
                        offset_min, offset_sec, offset_frame,
                        *length_min, *length_sec, length_frame,
                        end_min, end_sec, end_frame);
#endif
        return track;
}

/*
 * close_tray - close the CD tray
 */
void close_tray(cdrom_fd)
        int cdrom_fd;
{
        int retval = -1;
#if defined(__linux__) || defined(sun)
        retval = ioctl(cdrom_fd, CDROMCLOSETRAY);
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        retval = ioctl (cdrom_fd, CDIOCCLOSE);
#endif
        if (retval < 0)
                printf("Could not close the CD tray: %s\n", strerror(errno));
}

/*
 * show_intro - print some information about this software
 */
void show_intro(void)
{
        printf("Lazy - a CD player with track/artist/song informations\n"
                        "Version %s (%s) with CDDB-enabled.\n"
                        "Written by Lucas Correia Villa Real <lucasvr@gobolinux.org>.\n"
                        "This software is licensed under GPL. See 'README' for more information\n\n",
        LAZY_VERSION, LAZY_DATE);
}

/*
 * set_global_vars - set some global variables with informations taken from CONFIG_FILE, 
 * 					 returning 0 on success or -1 on error.
 */
int set_global_vars(void)
{
        FILE *config;
        char temp[strlen(CONFIG_FILE) + strlen(getenv("HOME")) + 2];
        char *token, line[BUFFER_SIZE], bkp_line[BUFFER_SIZE];

        /* set the pointers to the heart of the sun */
        artist = album = ADDRESS = PORT = CFIG_PATH = REAL_PATH = NULL;
#if defined(__linux__) || defined(sun)
        CDDEV = strdup("/dev/sr0");
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
        CDDEV = strdup ("/dev/acd0");
#endif
        AUDIO_DEVICE = strdup("/dev/dsp");
        VERBOSE = 0;
        STOP_MUSIC = 1;
        CLOSE_TRAY = 0;
        SHOW_TIME = 0;
        SHOW_REMAINING = 0;
        RANDOM = 0;
        PLAYLIST = 0;
        SHOWONLY = 0;
        DIGITAL_EXTRACTION = 0;

        /* try to open the config file */
        snprintf(temp, sizeof(temp), "%s/%s", getenv("HOME"), CONFIG_FILE);
        config = fopen(temp, "r");
        if (!config) {
                /* try to fetch information from /etc/lazyrc */
                snprintf(temp, sizeof(temp), "/etc/%s", CONFIG_FILE);
                config = fopen(temp, "r");
                if (!config) {
                        printf("%s: %s", temp, strerror(errno));
                        return -1;
                }
        }

        /* get the information from it */
        while (!(feof(config))) {
                fgets(line, sizeof(line), config);
                strcpy(bkp_line, line);
                if ((!(strncmp(line, "#", 1))) || (!(strncmp(line, "\n", 1))))
                        continue;
                else if ((strstr(line, "CDDEV"))) {
                        token = strtok(line, "=");
                        if (!token) {
                                printf("syntax error on config file: %s\n", bkp_line);
                                fclose(config);
                                exit(EXIT_FAILURE);
                        }
                        token = strtok(NULL, " \t");
                        if (token)
                                CDDEV = strdup(token);
                        else
                                printf("CDDEV has no value in the config. Please check it!\n");

                } else if ((strstr(line, "AUDIODEV"))) {
                        token = strtok(line, "=");
                        if (!token) {
                                printf("syntax error on config file: %s\n", bkp_line);
                                fclose(config);
                                exit(EXIT_FAILURE);
                        }
                        token = strtok(NULL, " \t");
                        if (token)
                                AUDIO_DEVICE = strdup(token);
                        else
                                printf("AUDIODEV has no value in the config. Please check it!\n");

                } else if ((strstr(line, "ADDRESS"))) {
                        token = strtok(line, "=");
                        if (!token) {
                                printf("syntax error on config file: %s\n", bkp_line);
                                fclose(config);
                                exit(EXIT_FAILURE);
                        }
                        token = strtok(NULL, " \t");
                        if (token)
                                ADDRESS = strdup(token);
                        else
                                printf("ADDRESS has no value in the config. Please check it!\n");

                } else if ((strstr(line, "REMAINING"))) {
                        SHOW_REMAINING = 1;
                } else if ((strstr(line, "ELAPSED"))) {
                        SHOW_REMAINING = 0;
                } else if ((strstr(line, "PORT"))) {
                        token = strtok(line, "=");
                        if (!token) {
                                printf("syntax error on config file: %s\n", bkp_line);
                                fclose(config);
                                exit(EXIT_FAILURE);
                        }
                        token = strtok(NULL, " \t");
                        if (token)
                                PORT = strdup(token);
                        else
                                printf("PORT has no value in the config. Please check it!\n");

                } else if ((strstr(line, "CDDB_PATH"))) {
                        token = strtok(line, "=");
                        if (!token) {
                                printf("syntax error on config file: %s\n", bkp_line);
                                fclose(config);
                                exit(EXIT_FAILURE);
                        }
                        token = strtok(NULL, " \t");
                        if (token)
                                CFIG_PATH = strdup(token);
                        else
                                printf("CDDB_PATH has no value in the config. Please check it!\n");
                }
        } /* while */
        return (0);
}

/*
 * free_globals - free the globals alloc'ed in set_global_vars()
 */
void free_globals(void)
{
        DOUT("->free: %s:%d\n", __FILE__, __LINE__);
        if (CDDEV)
                free(CDDEV);
        if (AUDIO_DEVICE)
                free(AUDIO_DEVICE);
        if (ADDRESS)
                free(ADDRESS);
        if (PORT)
                free(PORT);
        if (CFIG_PATH)
                free(CFIG_PATH);
        if (REAL_PATH)
                free(REAL_PATH);
        if (artist)
                free(artist);
        if (album)
                free(album);
        DOUT("<-free: %s:%d\n", __FILE__, __LINE__);
}

/*
 * skip_track - when received the SIGINT signal, skip a track. This function doesn't need 
 * 				to have anything on it's body, but a simple information is not a bad idea.
 */
void skip_track(int signal)
{
        /* call the do_nop function. this function will let us to listen for another signals again */
        printf("Skipping track...             \n\n");
        do_nop();

        /*
         * change the GLOBAL_TIME to a value bigger than one possible to be created by play_cd()
         * this will cause the function to abort, skipping the track instead of still showing it's
         * remaining time
         */
        GLOBAL_TIME = ABORT_CONSTANT;
}

/*
 * do_nop - this function returns and receive nothing
 */
void do_nop(void)
{
        signal(SIGINT, suicide);
        usleep(300000);
}

/*
 * suicide - if signal == SIGINT, quit the program
 */
void suicide(int signal)
{
        int cdrom, retval = -1;
        char *error = NULL;
        char *device = strdup(CDDEV);

        /* open the device */
        if ((cdrom = open(device, O_RDONLY | O_NONBLOCK)) < 0) {
                fprintf(stderr, "open %s: %s\n", device, strerror(errno));
                exit(EXIT_FAILURE);
        }

        if (STOP_MUSIC) {
                // stop the music
#if defined(__linux__) || defined(sun)
                retval = ioctl(cdrom, CDROMSTOP);
                error = "CDROMSTOP";
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__qnx__)
                retval = ioctl (cdrom, CDIOCSTOP);
                error = "CDIOCSTOP";
#endif
                if (retval < 0) {
                        perror(error);
                        close(cdrom);
                        exit(EXIT_FAILURE);
                }
        }

        /* close the file descriptor */
        if (close(cdrom) < 0) {
                fprintf(stderr, "close: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
        }

        printf("Oh my God! They killed Kenny!\n");
        free_globals();
        exit(EXIT_SUCCESS);
}

