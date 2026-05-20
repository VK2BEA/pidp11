/* gpio.c: the real-time process that handles multiplexing

 Copyright (c) 2015-2016, Oscar Vermeulen & Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 01-Sep-2025  TK    Change list is now at https://github.com/Terri-Kennedy/pidp11/commits/main/
 06-Jul-2025  TK    Merge mutex additions from Eric N, add comments about knob rotation direction
 18-Dec-2023  OV    new GPIO interface using the code of pinctrl, needed for Pi 5
 14-Aug-2019  OV    fix for Raspberry Pi 4's different pullup configuration
 01-Jul-2017  MH    remove INP_GPIO before OUT_GPIO and change knobValue
 01-Apr-2016  OV    almost perfect before VCF SE
 15-Mar-2016  JH    display patterns for brightness levels
 16-Nov-2015  JH    acquired from Oscar


 gpio.c from Oscar Vermeules PiDP8-sources.
 Slightest possible modification by Joerg.
 See www.obsolescenceguaranteed.blogspot.com

 The only communication with the main program (simh):
 external variable ledstatus is read to determine which leds to light.
 external variable switchstatus is updated with current switch settings.

 */

#define _GPIO_C_

#include <time.h>
#include <pthread.h>
#include <stdint.h>
//#include "gpio.h"
#include "gpiopattern.h"


//20231218
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "pinctrl/gpiolib.h"
//-20231218


typedef unsigned int uint32;
typedef signed int int32;
typedef unsigned short uint16;

void short_wait(void); // used as pause between clocked GPIO changes
extern int knobValue[2];	// value for knobs. 0=ADDR, 1=DATA. see main.c.
void check_rotary_encoders(int switchscan);

// long intervl = 300000; // light each row of leds this long
long intervl = 50000; // light each row of leds 50 us
// almost flickerfree at 32 phases

// PART 1 - GPIO and RT process stuff ----------------------------------


// PART 2 - the multiplexing logic driving the front panel -------------

uint8_t ledrows[] = {20, 21, 22, 23, 24, 25};
uint8_t rows[] = {16, 17, 18};
uint8_t cols[] = {26,27,4, 5,6,7, 8,9,10, 11,12,13};


void *blink(int *terminate)
{
	int i, j, k, switchscan, tmp;

	// init GPIO stuff ----------
        int num_gpios;
	int ret;

	ret = gpiolib_init();

	if (ret < 0)
	{
		printf("Failed to initialise gpiolib - %d\n", ret);
		return (void *)-1;
	}

	num_gpios = ret;
	if (!num_gpios)
	{
		printf("No GPIO chips found\n");
		return (void *)-1;
	}

	// -----
	uint32_t gpiomask[(MAX_GPIO_PINS + 31)/32] = { 0 };
#define ARRAY_SIZE(_a) (sizeof(_a)/sizeof(_a[0]))
	for (i = ARRAY_SIZE(gpiomask) - 1; i >= 0; i--)
	{
		if (gpiomask[i])
			break;
	}
	if (i < 0)
		memset(gpiomask, 0xff, sizeof(gpiomask));

	ret = gpiolib_mmap();
	if (ret)
	{
		if (ret == EACCES && geteuid())
			printf("Must be root\n");
		else
			printf("Failed to mmap gpiolib - %s\n", strerror(ret));
		return (void *)-1;
	}
	
	// set thread to real time priority -----------------
	struct sched_param sp;
	sp.sched_priority = 98; // maybe 99, 32, 31?
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) 
	{
		fprintf(stderr, "warning: failed to set RT priority\n");
	}

	// initialise GPIO (all pins used as inputs, with pull-ups enabled on cols)
/*	for (i = 0; i < 6; i++)  // Define ledrows as input
	for (i = 0; i < 12; i++) // Define cols as input
	for (i = 0; i < 3; i++) // Define rows as input
*/
	// GPIO column pins - input pullup
	// GPIO row pins - input pullup
	// GPIO ledrow pins - input pullup
	// --------------------------------------------------
	for (i=0;i<6;i++)
	{

		gpio_set_fsel(ledrows[i], GPIO_FSEL_OUTPUT);
		gpio_set_dir(ledrows[i], DIR_OUTPUT);
		gpio_set_drive(ledrows[i], DRIVE_LOW);
	}
	for (i=0;i<12;i++)
	{
		gpio_set_fsel(cols[i], GPIO_FSEL_OUTPUT);
		gpio_set_pull(cols[i], PULL_UP);
		gpio_set_dir(cols[i], DIR_OUTPUT);
		gpio_set_drive(cols[i], DRIVE_LOW);
	}
	for (i=0;i<3;i++)
	{
		gpio_set_fsel(rows[i], GPIO_FSEL_INPUT);
		//gpio_set_dir(rows[i], DIR_INPUT);
		gpio_set_pull(rows[i], PULL_UP);
	}

	printf("\nPiDP-11 FP 20251020\n");

// ========== TEMPORARY TEST LOOP
	while (*terminate == 0) {
		unsigned phase;
		//unsigned loopcount=0;
		//if ((loopcount++ % 500) == 0)	printf("1\n"); // visual heart beat


		// display all phases circular
		for (phase = 0; phase < GPIOPATTERN_LED_BRIGHTNESS_PHASES; phase++) {
			// each phase must be eact same duration, so include switch scanning here

			// the original gpio_ledstatus[8] runs trough all phases
			// safely grab the current page index			
			int idx;
			pthread_mutex_lock(&gpiopattern_swap_lock);
			idx = gpiopattern_ledstatus_phases_readidx;
			pthread_mutex_unlock(&gpiopattern_swap_lock);
			
			volatile uint32_t *gpio_ledstatus =
					gpiopattern_ledstatus_phases[idx][phase];

			// prepare for lighting LEDs by setting col pins to output
			for (i = 0; i < 12; i++) {
				gpio_set_dir(cols[i], DIR_OUTPUT);
			}

			// light up 6 rows of 12 LEDs each
			for (i = 0; i < 6; i++) {

				// Toggle columns for this ledrow (which LEDs should be on (CLR = on))
				for (k = 0; k < 12; k++) {
					if ((gpio_ledstatus[i] & (1 << k)) == 0)
						gpio_set_drive(cols[k], DRIVE_HIGH);
					else
						gpio_set_drive(cols[k], DRIVE_LOW);
				}

				// Toggle this ledrow on
				gpio_set_drive(ledrows[i], DRIVE_HIGH);

				nanosleep((struct timespec[]
				)	{	{	0, intervl}}, NULL);

				// Toggle ledrow off
				gpio_set_drive(ledrows[i], DRIVE_LOW);

				usleep(10); // waste of cpu cycles but may help against udn2981 ghosting, not flashes though
			}

			//nanosleep ((struct timespec[]){{0, intervl}}, NULL); // test

			// prepare for reading switches
			for (i = 0; i < 12; i++)			// flip columns to input 
				gpio_set_dir(cols[i], DIR_INPUT);	// Need intl pull-ups enabled

			// read three rows of switches
			for (i = 0; i < 3; i++)
			{
				// on one row pin, 
				// output 0V to overrule the built-in pull-up from column input pin
				// to read this row of switches
				gpio_set_dir(rows[i], DIR_OUTPUT);
				gpio_set_drive(rows[i], DRIVE_LOW);

				nanosleep((struct timespec[]) { { 0, intervl / 100}}, NULL); 
				// probably unnecessary long wait

				switchscan = 0;
				for (j = 0; j < 12; j++) // 12 switches in each row
				{
					tmp = gpio_get_level(cols[j]);
					if (tmp != 0)
						switchscan += 1 << j;
	
					//printf("-%d",tmp);
				}
				//printf(" | ");
				
				// stop sinking current from this row of switches
				gpio_set_dir(rows[i], DIR_INPUT);

				if (i==2)
					check_rotary_encoders(switchscan);	// translate raw encoder data to switch position

				gpio_switchstatus[i] = switchscan;

			}
			//printf("\n");
		}
	}
	//printf("\nFP off\n");
	// at this stage, all cols, rows, ledrows are set to input, so elegant way of closing down.

	return 0;
}

void short_wait(void) // creates pause required in between clocked GPIO settings changes
{
	fflush(stdout); //
	usleep(1); // suggested as alternative for asm which c99 does not accept
}



#define N_ROTARY    2
#define ADDR        0
#define DATA        1
#define UNDEFINED   (-1)
#define NORMAL      0
#define REVERSED    1        // early production PiDP11s had reversed rotarty encoders
#define SYNC        0b11
#define CW          0
#define CCW         1

#ifndef true
  #define true    (1)
  #define false   (0)
#endif

void check_rotary_encoders(int switchscan)
{
    // 2 rotary encoders. Each has two switch pins. Normally, both are 0 - no rotation.
    // encoder 1: row1, bits 8,9. Encoder 2: row1, bits 10,11
    // Gray encoding: rotate up sequence   = 11 -> 01 -> 00 -> 10 -> 11
    // Gray encoding: rotate down sequence = 11 -> 10 -> 00 -> 01 -> 11

	// When 0b11, 0b01 or 0b11, 0b10 is detected - mark the assumed rotary direction.
	// This must correspond to the appropriate trailing sequence 0b10, 0b11 or 0b01, 0b11
	// to be valid. 
    static int startDirection[ N_ROTARY ] = {0,0};
	// On in nvalid sequence, mark to resync (ignore until 0b11 is received)
    static int bResync[ N_ROTARY ] = {true, true};

	// There are two variants of switch hardware. If the action of the control is backward,
	// define the environment variable to reverse
	//     export PIDP_11_ROTATION="FLIP"
    static int direction = UNDEFINED;
    static char* envdirection;

	// extract the two bits for each rotary switch from a row containing other switches.
    int currentCode[ N_ROTARY ];
	// save the previous state (when it differs from the current one) so we can act
	// on specific transitions
    static int previousCode[2] = { SYNC, SYNC };
	// which switch (address (8 positions) or data (4 positions))
    int i;

	// We only need to do this once upon start
    if (direction == UNDEFINED)
    {
        // First pass through, check environment
        // Assume PIDP_11_ROTATION is not set
        direction = NORMAL;
        envdirection = getenv("PIDP_11_ROTATION");
        if (envdirection != NULL)
        {
            if (strcmp(envdirection, "FLIP") == 0)
            {
                direction = REVERSED;
            }
        }
    }

	// extract the two bits for the quadrature channels of each switch
    currentCode[ ADDR ] = (switchscan & 0x300) >> 8;
    currentCode[ DATA ] = (switchscan & 0xC00) >> 10;

    switchscan = switchscan & 0xff;	// set the 4 bits to zero

	// act for each switch
    for ( i=0; i < N_ROTARY; i++)
    {
        if( bResync[i] ) {
            // looking for sync sequence (0b11)
            if( currentCode[i] == SYNC )
                bResync[i] = false;

            previousCode[i] = currentCode[i];   // this will abandon any further processing
        }

		// only act on transitions
        if( currentCode[i] == previousCode[i] ) // nothing to see here
            continue;

        // detect initial rotation direction
        // look for 0b11, 0b01 or 0b11, 0b11 which are the first two bytes of a sequence
        if( previousCode[ i ] == SYNC ) {
            if ( currentCode[ i ] == 0b01 ) {
                startDirection[ i ] = CW;			// 0b11, 0b01 clockwise
            } else if ( currentCode[i] == 0b10 ) {
                startDirection[ i ] = CCW;			// 0b11, 0b11 counter clockwise
            } else { 
                bResync[i] = true;     				// should never see 0b11 followed by 0b00
            }
        }

        // detect final rotation direction
        // look for 0b10, 0b11 or 0b01, 0b11 which are the last two bytes of a sequence
        if( currentCode[i] == SYNC && !bResync[i]) {
            if ( previousCode[ i ] == 0b01 ) {
                if( startDirection[ i ] == CCW ) {
                    // 0b11, 0b10, 0b00, 0b01, 0x11 recognised (CCW)
                    knobValue[i] += ( direction == REVERSED ? +1 : -1 );
                } else {
                    // this is an error .. 
                    // 0b11, 0b10, 0b00, 0b10, 0x11 recognised (bogus/partial switch)
                }
            } else if ( previousCode[i] == 0b10 ) {
                if( startDirection[ i ] == CW ) {
                    // 0b11, 0b01, 0b00, 0b10, 0x11 recognised (CCW)
                    knobValue[i] += ( direction == REVERSED ? -1 : +1 );
                } else {
                    // this is an error .. 
                    // 0b11, 0b01, 0b00, 0b01, 0x11 recognised (bogus/partial switch)
                }
            } 
        }

        if( currentCode[i] != previousCode[i] ) {
            // printf( "%s %02b --> %02b\r\n", i==0 ? "address" : "data", previousCode[i], currentCode[i]);
            previousCode[i] = currentCode[i];
        }

    }

    knobValue[ ADDR ] = knobValue[ ADDR ] & 0b00000111;   // 8 selections for address rotary switch
    knobValue[ DATA ] = knobValue[ DATA ] & 0b00000011;   // 4 selections for data rotary switch

    // Status is communicated through global variable knobValue[i]

}
