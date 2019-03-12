/*
 *    rpi.h
 *
 *    Various defines for playing with the GPIO pins
 *    15-January-2012
 *    Dom and Gert van Loo
 */

// Access from ARM Running Linux

#ifndef RPI_MEMORY_STUFF_H_
#define RPI_MEMORY_STUFF_H_

#define BCM2708_PERI_BASE        0x20000000
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>



#if 1
#define HERES(A) printf("-- %s :( %d ) -- %s -- %s\n", __FILE__, __LINE__, __FUNCTION__,  A )
#define HERE()  printf("-HERE- %s :( %d ) -- %s \n", __FILE__, __LINE__, __FUNCTION__)
//#define DOUT(...) fprintf(stdout, __VA_ARGS__)
#define DOUT(...) printf(__VA_ARGS__)
#else
#define HERE()
#define HERES(A)
#define DOUT(...)
#endif

unsigned char playing;


#define PI_PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

int  mem_fd;
char *gpio_mem, *gpio_map;
char *spi0_mem, *spi0_map;


// I/O access
volatile unsigned *gpio;

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0

#endif

