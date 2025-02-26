/* 
 * This file is part of the ftdi-nand-flash-reader distribution (https://github.com/maehw/ftdi-nand-flash-reader).
 * Copyright (c) 2018 maehw.
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * \file bitbang_ft2232.c
 * \brief NAND flash reader based on FTDI FT2232 IC in bit-bang IO mode
 * Interfacing NAND flash devices with an x8 I/O interface for address and data.
 * Additionally the signals Chip Enable (nCE), Write Enable (nWE), Read Enable (nRE), 
 * Address Latch Enable (ALE), Command Latch Enable (CLE), Write Protect (nWP) 
 * and Ready/Busy (RDY) on the control bus are used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ftdi.h>

// #define DEBUG 

#ifdef DEBUG
  #define DBG(...) do { printf(__VA_ARGS__); } while (0)
  #define DBGFLUSH(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
  #define DBG(...)
  #define DBGFLUSH(...)
#endif


/* FTDI FT2232H VID and PID */
#define FT2232H_VID 0x0403
#define FT2232H_PID 0x6010

/* Pins on ADBUS0..7 (I/O bus) */
#define PIN_DIO0 0x01
#define PIN_DIO1 0x02
#define PIN_DIO2 0x04
#define PIN_DIO3 0x08
#define PIN_DIO4 0x10
#define PIN_DIO5 0x20
#define PIN_DIO6 0x40
#define PIN_DIO7 0x80
#define IOBUS_BITMASK_WRITE 0xFF
#define IOBUS_BITMASK_READ  0x00

/* Pins on BDBUS0..7 (control bus) */
#define PIN_CLE  0x01
#define PIN_ALE  0x02
#define PIN_nCE  0x04
#define PIN_nWE  0x08
#define PIN_nRE  0x10
#define PIN_nWP  0x20
#define PIN_RDY  0x40 /* READY / nBUSY output signal */
#define PIN_LED  0x80
#define CONTROLBUS_BITMASK 0xBF /* 0b1011 1111 = 0xBF */

#define STATUSREG_IO0  0x01

#define REALWORLD_DELAY 10 /* 10 usec */

#define PAGE_SIZE 2112
#define PAGE_SIZE_NOSPARE 2048
#define PAGE_PER_BLOCK 64
#define BLOCK_COUNT 2048

#define DEFAULT_FILENAME "flashdump.bin"
#define DEFAULT_START_PAGE 0
#define DEFAULT_PAGE_COUNT 131072
#define DEFAULT_DELAY 0


const unsigned char CMD_READID = 0x90; /* read ID register */
const unsigned char CMD_READ1[2] = { 0x00, 0x30 }; /* page read */
const unsigned char CMD_BLOCKERASE[2] = { 0x60, 0xD0 }; /* block erase */
const unsigned char CMD_READSTATUS = 0x70; /* read status */
const unsigned char CMD_PAGEPROGRAM[2] = { 0x80, 0x10 }; /* program page */

typedef enum { OFF=0, ON=1 } onoff_t;
typedef enum { IOBUS_IN=0, IOBUS_OUT=1 } iobus_inout_t;

unsigned char iobus_value;
unsigned char controlbus_value;

struct ftdi_context *nandflash_iobus, *nandflash_controlbus;

typedef struct _prog_params {
    int start_page;
    char *filename;
    int overwrite;
    int count;
    int delay; /* delay in usec */
    int test; /* run simple tests instead of dump */
    int do_program;
    char *input_file;
    int input_skip; /* Number of pages to first skip when programming */
    int do_erase;
    int start_block;
} prog_params_t;


void reset_prog_params(prog_params_t *params)
{
    memset(params, 0, sizeof(*params));
    params->start_page = DEFAULT_START_PAGE;
    params->filename = DEFAULT_FILENAME;
    params->delay = DEFAULT_DELAY;
}

void print_prog_params(prog_params_t *params)
{
    printf("Params: start_page=%d (%x), count=%d, filename=%s, "
           "overwrite=%d, delay=%d, test=%d, program=%d (input file=%s, skip=%d) "
           "erase=%d (start_block=%d)\n",
        params->start_page,
        params->start_page,
        params->count,
        params->filename,
        params->overwrite,
        params->delay,
        params->test,
        params->do_program,
        params->input_file,
        params->input_skip,
        params->do_erase,
        params->start_block);
}

void usage(char **argv)
{
    printf("usage: %s  [-s start-page] [-c count] [-k skip-pages] [-d delay]" \
           " [-b start-block] [-o] [-t] [-h] [-f output] [-p input]\n", argv[0]);
    printf("  -h      : this help\n");

    printf("  -b n    : start erasing at block n (erase)\n");
    printf("  -c n    : only process n pages (dump, program) or blocks (erase)\n");
    printf("  -d n    : add n usecs of delay for some operations (default 0)\n");
    printf("  -E      : erase flash content (dangerous!)\n");
    printf("  -f name : name of output file when dumping (default: flashdump.bin)\n");
    printf("  -k n    : skip of n pages in input file when programming (program)\n");
    printf("  -o      : overwrite output file (dump)\n");
    printf("  -p name : program file 'name' into flash (dangerous!) (program)\n");
    printf("  -s n    : start page in flash (dump, program)\n");
    printf("  -t      : run tests to check correct wiring; DISCONNECT THE FLASH\n");
    printf("\n");
    printf("Examples:\n");
    printf("   %s -f /tmp/dump1.bin -s 10000 -c 500\n", argv[0]);
    printf("      dump 500 pages, starting at page 10000, into file /tmp/dump1.bin\n");
    printf("\n");
    printf("   %s -p /tmp/dump1.bin -s 10100 -c 400 -k 100\n", argv[0]);
    printf("      program 400 pages starting at page 10100, using file /tmp/dump1.bin\n");
    printf("      and after skipping 100 pages from file\n");
    printf("\n");
    printf("   %s -E -b 10 -c 5\n", argv[0]);
    printf("      erase 5 blocks, starting with block 10\n");
    printf("\n");
}

int parse_prog_params(prog_params_t *params, int argc, char **argv)
{
  int index;
  int c;

  reset_prog_params(params);

  opterr = 0;

  while ((c = getopt(argc, argv, "b:c:d:Es:tf:hk:op:")) != -1)
    switch (c)
      {
      case 'b':
        params->start_block = atoi(optarg);
        break;
      case 'c':
        params->count = atoi(optarg);
        break;
      case 'd':
        params->delay = atoi(optarg);
        break;
      case 'E':
        params->do_erase = 1;
        break;
      case 'f':
        params->filename = optarg;
        break;
      case 'h':
        usage(argv);
        return -1;
      case 'k':
        params->input_skip = atoi(optarg);
        break;
      case 'o':
        params->overwrite = 1;
        break;
      case 'p':
        params->do_program = 1;
        params->input_file = optarg;
        break;
      case 's':
        params->start_page = atoi(optarg);
        break;
      case 't':
        params->test = 1;
        break;
      case '?':
        if (strchr("bcdsfkp", optopt))
          fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        else 
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        fprintf(stderr, "Use -h for help\n");
        return 1;
      default:
        abort ();
      }

  if (params->do_erase && params->start_page)
  {
      fprintf(stderr, "-s (start page) does not work with erase. Use -b (start block)\n");
      return -1;
  }

  if (params->start_block && params->start_page)
  {
      fprintf(stderr, "You can't use -s (start page) and -b (start block) "
                      "together. Choose one.\n");
      return -1;
  }

  if (params->start_block)
  {
      params->start_page = params->start_block * PAGE_PER_BLOCK;
  }

  for (index = optind; index < argc; index++)
    printf ("Non-option argument %s\n", argv[index]);
  return 0;
}

void inline _usleep(int delay_us)
{
    if (delay_us)
    {
        usleep(delay_us);
    }
}

void controlbus_reset_value()
{
    controlbus_value = 0x00;
}

void controlbus_pin_set(unsigned char pin, onoff_t val)
{
    if (val == ON)
        controlbus_value |= pin;
    else
        controlbus_value &= (unsigned char)0xFF ^ pin;
}

void controlbus_update_output()
{
    unsigned char buf[1]; /* buffer for FTDI function needs to be an array */
    buf[0] = controlbus_value;
    ftdi_write_data(nandflash_controlbus, buf, 1);
}

void test_controlbus()
{
    #define CONTROLBUS_TEST_DELAY 1000000 /* 1 sec */

    printf("  CLE on\n");
    controlbus_pin_set(PIN_CLE, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  ALE on\n");
    controlbus_pin_set(PIN_ALE, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nCE on\n");
    controlbus_pin_set(PIN_nCE, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nWE on\n");
    controlbus_pin_set(PIN_nWE, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);


    printf("  nRE on\n");
    controlbus_pin_set(PIN_nRE, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nWP on\n");
    controlbus_pin_set(PIN_nWP, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  LED on\n");
    controlbus_pin_set(PIN_LED, ON);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);


    printf("  CLE off\n");
    controlbus_pin_set(PIN_CLE, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  ALE off\n");
    controlbus_pin_set(PIN_ALE, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nCE off\n");
    controlbus_pin_set(PIN_nCE, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nWE off\n");
    controlbus_pin_set(PIN_nWE, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nRE off\n");
    controlbus_pin_set(PIN_nRE, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  nWP off\n");
    controlbus_pin_set(PIN_nWP, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);

    printf("  LED off\n");
    controlbus_pin_set(PIN_LED, OFF);
    controlbus_update_output(nandflash_controlbus);
    _usleep(CONTROLBUS_TEST_DELAY);
}

void iobus_set_direction(iobus_inout_t inout)
{
    if (inout == IOBUS_OUT)
        ftdi_set_bitmode(nandflash_iobus, IOBUS_BITMASK_WRITE, BITMODE_BITBANG);
    else if (inout == IOBUS_IN)
        ftdi_set_bitmode(nandflash_iobus, IOBUS_BITMASK_READ, BITMODE_BITBANG);        
}

void iobus_reset_value()
{
    iobus_value = 0x00;
}

void iobus_pin_set(unsigned char pin, onoff_t val)
{
    if (val == ON)
        iobus_value |= pin;
    else
        iobus_value &= (unsigned char)0xFF ^ pin;
}

void iobus_set_value(unsigned char value)
{
    iobus_value = value;
}

void iobus_update_output()
{
    unsigned char buf[1]; /* buffer for FTDI function needs to be an array */
    buf[0] = iobus_value;
    ftdi_write_data(nandflash_iobus, buf, 1);
}

unsigned char iobus_read_input()
{
    unsigned char buf; 
    //ftdi_read_data(nandflash_iobus, buf, 1); /* buffer for FTDI function needed to be an array */
    ftdi_read_pins(nandflash_iobus, &buf);
    return buf;
}

unsigned char controlbus_read_input()
{
    unsigned char buf;
    //ftdi_read_data(nandflash_controlbus, buf, 1); /* buffer for FTDI function needed to be an array */
    ftdi_read_pins(nandflash_controlbus, &buf);
    return buf;
}


void test_iobus()
{
    #define IOBUS_TEST_DELAY 1000000 /* 1 sec */

    printf("  DIO0 on\n");
    iobus_pin_set(PIN_DIO0, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO1 on\n");
    iobus_pin_set(PIN_DIO1, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO2 on\n");
    iobus_pin_set(PIN_DIO2, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO3 on\n");
    iobus_pin_set(PIN_DIO3, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO4 on\n");
    iobus_pin_set(PIN_DIO4, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO5 on\n");
    iobus_pin_set(PIN_DIO5, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO6 on\n");
    iobus_pin_set(PIN_DIO6, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    printf("  DIO7 on\n");
    iobus_pin_set(PIN_DIO7, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);


    iobus_pin_set(PIN_DIO0, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO1, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO2, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO3, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO4, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO5, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO6, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    iobus_pin_set(PIN_DIO7, OFF);
    iobus_update_output(nandflash_iobus);
    _usleep(IOBUS_TEST_DELAY);

    _usleep(5 * IOBUS_TEST_DELAY);
    iobus_set_value(0xFF);
    iobus_update_output();
    _usleep(5 * IOBUS_TEST_DELAY);
    iobus_set_value(0xAA);
    iobus_update_output();
    _usleep(5 * IOBUS_TEST_DELAY);
    iobus_set_value(0x55);
    iobus_update_output();
    _usleep(5 * IOBUS_TEST_DELAY);
    iobus_set_value(0x00);
    iobus_update_output();

    
    iobus_pin_set(PIN_DIO0, ON);
    iobus_pin_set(PIN_DIO2, ON);
    iobus_pin_set(PIN_DIO4, ON);
    iobus_pin_set(PIN_DIO6, ON);
    iobus_update_output(nandflash_iobus);
    _usleep(2* 100000);

}

/* 
 * "Command Input bus operation is used to give a command to the memory device.
 *  Command are accepted with Chip Enable low, Command Latch Enable High, 
 *  Address Latch Enable low and Read Enable High and latched on the rising
 *  edge of Write Enable. Moreover for commands that starts a modify operation
 *  (write/erase) the Write Protect pin must be high."
*/
int latch_command(prog_params_t *params, unsigned char command)
{
    /* check if ALE is low and nRE is high */
    if (controlbus_value & PIN_nCE)
    {
        fprintf(stderr, "latch_command requires nCE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if (~controlbus_value & PIN_nRE)
    {
        fprintf(stderr, "latch_command requires nRE pin to be high\n");
        return EXIT_FAILURE;
    }

    DBG("latch_command(0x%02X)\n", command);

    /* toggle CLE high (activates the latching of the IO inputs inside the 
     * Command Register on the Rising edge of nWE) */
    DBGFLUSH("  setting CLE high,");
    controlbus_pin_set(PIN_CLE, ON);
    controlbus_update_output();

    // toggle nWE low
    DBGFLUSH(" nWE low,");
    controlbus_pin_set(PIN_nWE, OFF);
    controlbus_update_output();

    // change I/O pins
    DBGFLUSH(" I/O bus to command,");
    iobus_set_value(command);
    iobus_update_output();

    // toggle nWE back high (acts as clock to latch the command!)
    DBGFLUSH(" nWE high,");
    controlbus_pin_set(PIN_nWE, ON);
    controlbus_update_output();

    // toggle CLE low
    DBG(" CLE low\n");
    controlbus_pin_set(PIN_CLE, OFF);
    controlbus_update_output();

    return 0;
}

/** 
 * "Address Input bus operation allows the insertion of the memory address. 
 * Five cycles are required to input the addresses for the 4Gbit devices. 
 * Addresses are accepted with Chip Enable low, Address Latch Enable High, 
 * Command Latch Enable low and Read Enable High and latched on the rising 
 * edge of Write Enable. 
 * 
 * Moreover for commands that starts a modifying operation (write/erase) 
 * the Write Protect pin must be high. See Figure 5 and Table 13 for details 
 * of the timings requirements. 
 * 
 * Addresses are always applied on IO7:0 regardless of the bus configuration 
 * (x8 or x16)."
 */
int latch_address(prog_params_t *params, unsigned char address[], unsigned int addr_length)
{
    unsigned int addr_idx = 0;

    /* check if ALE is low and nRE is high */
    if (controlbus_value & PIN_nCE)
    {
        fprintf(stderr, "latch_address requires nCE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if (controlbus_value & PIN_CLE)
    {
        fprintf(stderr, "latch_address requires CLE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if (~controlbus_value & PIN_nRE)
    {
        fprintf(stderr, "latch_address requires nRE pin to be high\n");
        return EXIT_FAILURE;
    }

    /* toggle ALE high (activates the latching of the IO inputs inside
     * the Address Register on the Rising edge of nWE. */
    controlbus_pin_set(PIN_ALE, ON);
    controlbus_update_output();

    for (addr_idx = 0; addr_idx < addr_length; addr_idx++)
    {
        // toggle nWE low
        controlbus_pin_set(PIN_nWE, OFF);
        controlbus_update_output();
        _usleep(params->delay);

        // change I/O pins
        iobus_set_value(address[addr_idx]);
        iobus_update_output();
        _usleep(params->delay); /* TODO: assure setup delay */

        // toggle nWE back high (acts as clock to latch the current address byte!)
        controlbus_pin_set(PIN_nWE, ON);
        controlbus_update_output();
        _usleep(params->delay); /* TODO: assure hold delay */
    }

    // toggle ALE low
    controlbus_pin_set(PIN_ALE, OFF);
    controlbus_update_output();

    // wait for ALE to nRE Delay tAR before nRE is taken low (nanoseconds!)

    return 0;
}

/* Data Output bus operation allows to read data from the memory array and to 
 * check the status register content, the EDC register content and the ID data.
 * Data can be serially shifted out by toggling the Read Enable pin with Chip 
 * Enable low, Write Enable High, Address Latch Enable low, and Command Latch 
 * Enable low. */
int latch_register(prog_params_t *params, unsigned char reg[], unsigned int reg_length)
{
    unsigned int addr_idx = 0;

    /* check if ALE is low and nRE is high */
    if (controlbus_value & PIN_nCE)
    {
        fprintf(stderr, "latch_address requires nCE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if (~controlbus_value & PIN_nWE)
    {
        fprintf(stderr, "latch_address requires nWE pin to be high\n");
        return EXIT_FAILURE;
    }
    else if (controlbus_value & PIN_ALE)
    {
        fprintf(stderr, "latch_address requires ALE pin to be low\n");
        return EXIT_FAILURE;
    }

    iobus_set_direction(IOBUS_IN);

    for (addr_idx = 0; addr_idx < reg_length; addr_idx++)
    {
        /* toggle nRE low; acts like a clock to latch out the data;
         * data is valid tREA after the falling edge of nRE 
         * (also increments the internal column address counter by one) */
        controlbus_pin_set(PIN_nRE, OFF);
        controlbus_update_output();
        _usleep(params->delay);

        // read I/O pins
        reg[addr_idx] = iobus_read_input();

        // toggle nRE back high
        controlbus_pin_set(PIN_nRE, ON);
        controlbus_update_output();
        _usleep(params->delay);
    }

    iobus_set_direction(IOBUS_OUT);

    return 0;
}

void check_ID_register(unsigned char* ID_register)
{
    unsigned char ID_register_exp[5] = { 0xAD, 0xDC, 0x10, 0x95, 0x54 };

    /* output the retrieved ID register content */
    printf("actual ID register:   0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
        ID_register[0], ID_register[1], ID_register[2],
        ID_register[3], ID_register[4] ); 

    /* output the expected ID register content */
    printf("expected ID register: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
        ID_register_exp[0], ID_register_exp[1], ID_register_exp[2],
        ID_register_exp[3], ID_register_exp[4] ); 

    if (memcmp(ID_register_exp, ID_register, 5) == 0)
    {
        printf("PASS: ID register did match\n");
    }
    else
    {
        printf("FAIL: ID register did not match\n");
    }
}

/* 
 * Address Cycle Map calculations, for Toshiba TC58NVG1S3HTA00, page based.
 *
 * CA: Column Address (12 bits)
 * PA: Page Address 17 bits (6 bits page in block, 11 bits block address)
 *
 * NOTE: this will actually populate the 2nd byte (CA high) with all 8
 * bits (instead of 4), and the 5th byte (PA16..) with all 8 bits instead
 * of just 1 to let the function be more generalizable then the 2048*131072
 * configuration of the toshiba chip. If not acceptable, this function
 * should somehow fail instead of silently producing the wrong address
 * bytes.
 */
void get_address_cycle_map_x8_toshiba_page(unsigned int page, 
                                           unsigned int column, 
                                           unsigned char* addr_cycles)
{
    addr_cycles[0] = (unsigned char) ((column      ) & 0xFF); // CA0..CA7
    addr_cycles[1] = (unsigned char) ((column >>  8) & 0xFF); // CA8..CA11 //NOTE
    addr_cycles[2] = (unsigned char) ((page        ) & 0xFF); // PA0..PA7
    addr_cycles[3] = (unsigned char) ((page   >>  8) & 0xFF); // PA8..PA15
    addr_cycles[4] = (unsigned char) ((page   >> 16) & 0xFF); // PA16 // SEE NOTE
}

/* Address Cycle Map calculations 
 *
 * NOTE(vm): this was the original address calculation. I swapped it
 * with the "_toshiba" one that's appropriate for the Toshiba TC58NVG1S3HTA00
 * chip I'm working with.
 */
void get_address_cycle_map_x8(uint32_t mem_address, unsigned char* addr_cylces)
{
    addr_cylces[0] = (unsigned char)(  mem_address & 0x000000FF);
    addr_cylces[1] = (unsigned char)( (mem_address & 0x00000F00) >> 8 );
    addr_cylces[2] = (unsigned char)( (mem_address & 0x000FF000) >> 12 );
    addr_cylces[3] = (unsigned char)( (mem_address & 0x0FF00000) >> 20 );
    addr_cylces[4] = (unsigned char)( (mem_address & 0x30000000) >> 28 );
}

void wait_while_busy()
{
    DBG("Checking for busy line...");
    int loops = 0;
    unsigned char controlbus_val;
    do
    {
      if (loops)
      {
          DBGFLUSH(".");
      }
      loops++;
      controlbus_val = controlbus_read_input();
    }
    while (!(controlbus_val & PIN_RDY));

    DBG("  done\n");
}

int dump_memory(prog_params_t *params)
{
    FILE *fp;
    unsigned int page_idx;
    unsigned int page_idx_max;
    unsigned char addr_cycles[5];
    uint32_t mem_address;
    unsigned char mem_large_block[PAGE_SIZE]; /* page content */
    //    unsigned int byte_offset;
    //    unsigned int line_no;

    fp = fopen(params->filename, "wb");
    if (fp == NULL)
    {
        fprintf(stderr, "Could not open file: %s\n", params->filename);
        return -1;
    }
    printf("Opened output file: %s\n", params->filename);

    int count = params->count;
    if (count == 0)
    {
        count = DEFAULT_PAGE_COUNT - params->start_page;
    }

    // Start reading the data
    page_idx_max = params->start_page + count;
    for (page_idx = params->start_page; page_idx < page_idx_max; /* blocks per page * overall blocks */ page_idx++)
    {
      mem_address = page_idx * PAGE_SIZE_NOSPARE; // start address
      printf("Reading data from page %d / %d (%.2f %%), address: %08X\n", 
             page_idx, page_idx_max, (float)page_idx/(float)page_idx_max * 100,
             mem_address);
      {

          DBG("Latching first command byte to read a page: ");
          latch_command(params, CMD_READ1[0]);

          get_address_cycle_map_x8_toshiba_page(page_idx, 0, addr_cycles);
          DBG("Latching address cycles: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
              addr_cycles[0], addr_cycles[1], /* column address */
              addr_cycles[2], addr_cycles[3], addr_cycles[4]); /* row address */
          latch_address(params, addr_cycles, 5);

          DBG("Latching second command byte to read a page: ");
          latch_command(params, CMD_READ1[1]);

          // busy-wait for high level at the busy line
          wait_while_busy();

          DBG("Clocking out data block...\n");
          latch_register(params, mem_large_block, PAGE_SIZE);
      }

//      // Dumping memory to console and file
//      // 2112 bytes = 132 lines * 16 bytes/line
//      for( line_no = 0; line_no < 132; line_no++ )
//      {
//          byte_offset = line_no * 16;
//
//          // Dumping memory to console
//          // printf("%02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X | ",
//          //     mem_large_block[byte_offset+0], mem_large_block[byte_offset+1],
//          //     mem_large_block[byte_offset+2], mem_large_block[byte_offset+3],
//          //     mem_large_block[byte_offset+4], mem_large_block[byte_offset+5],
//          //     mem_large_block[byte_offset+6], mem_large_block[byte_offset+7],
//          //     mem_large_block[byte_offset+8], mem_large_block[byte_offset+9],
//          //     mem_large_block[byte_offset+10], mem_large_block[byte_offset+11],
//          //     mem_large_block[byte_offset+12], mem_large_block[byte_offset+13],
//          //     mem_large_block[byte_offset+14], mem_large_block[byte_offset+15] );
//
//          // for( k = 0; k < 16; k++ )
//          // {
//          //   c = mem_large_block[byte_offset+k];
//          //   if( c >= 0x20u && c <= 0x7Fu)
//          //     printf("%c", c);
//          //   else
//          //     printf("_");
//          // }
//          // printf("\n");
//      }

      // Dumping memory to file
      if (!fwrite(mem_large_block, PAGE_SIZE, 1, fp))
      {
          fprintf(stderr, "Error writing page %d to file, aborting\n", page_idx);
          fclose(fp);
          return -1;
      }
      // Flush every page so we can Ctrl-C happily; the dump is slow enough anyways.
      fflush(fp);
      DBG("\n");
    }

    // Finished reading the data
    printf("Closing binary dump file...\n");
    fclose(fp);

    return 0;
}

/**
 * BlockErase
 *
 * "The Erase operation is done on a block basis.
 * Block address loading is accomplished in three cycles initiated by an Erase Setup command (60h).
 * Only address A18 to A29 is valid while A12 to A17 is ignored (x8).
 *
 * The Erase Confirm command (D0h) following the block address loading initiates the internal erasing process.
 * This two step sequence of setup followed by execution command ensures that memory contents are not
 * accidentally erased due to external noise conditions.
 *
 * At the rising edge of WE after the erase confirm command input,
 * the internal write controller handles erase and erase verify.
 *
 * Once the erase process starts, the Read Status Register command may be entered to read the status register.
 * The system controller can detect the completion of an erase by monitoring the R/B output,
 * or the Status bit (I/O 6) of the Status Register.
 * Only the Read Status command and Reset command are valid while erasing is in progress.
 * When the erase operation is completed, the Write Status Bit (I/O 0) may be checked."
 */
int erase_block(prog_params_t *params, unsigned int block)
{
    uint32_t mem_address;
    unsigned int page;
    unsigned char addr_cycles[5];

    /* calculate memory address */
    page = block * PAGE_PER_BLOCK;
    mem_address = PAGE_SIZE_NOSPARE * page; 

    /* remove write protection */
    controlbus_pin_set(PIN_nWP, ON);

    DBG("Latching first command byte to erase a block...\n");
    latch_command(params, CMD_BLOCKERASE[0]); /* block erase setup command */

    DBG("Erasing block %u at memory address 0x%08X (page %u)\n", block, mem_address, page);
    get_address_cycle_map_x8_toshiba_page(page, 0, addr_cycles);
    DBG("  Address cycles are (but: will take only cycles 3..5) : 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
        addr_cycles[0], addr_cycles[1], /* column address */
        addr_cycles[2], addr_cycles[3], addr_cycles[4] ); /* row address */

    DBG("Latching page(row) address (3 bytes)...\n");
    unsigned char address[] = { addr_cycles[2], addr_cycles[3], addr_cycles[4] };
    latch_address(params, address, 3);

    DBG("Latching second command byte to erase a block...\n");
    latch_command(params, CMD_BLOCKERASE[1]);

    /* tWB: WE High to Busy is 100 ns -> ignore it here as it takes some time for the next command to execute */

    // busy-wait for high level at the busy line
    wait_while_busy();

    /* Read status */
    DBG("Latching command byte to read status...\n");
    latch_command(params, CMD_READSTATUS);

    unsigned char status_register;
    latch_register(params, &status_register, 1); /* data output operation */
    DBG("Status register content:   0x%02X\n", status_register);

    /* activate write protection again */
    controlbus_pin_set(PIN_nWP, OFF);

    if (status_register & STATUSREG_IO0)
    {
        fprintf(stderr, "Failed to erase block %u, status register=%02X.\n", block, status_register);
        return 1;
    }

    printf("  Successfully erased block %u.\n", block);
    return 0;
}

int latch_data_out(prog_params_t *params, unsigned char data[], unsigned int length)
{
    for (unsigned int k = 0; k < length; k++)
    {
        // toggle nWE low
        controlbus_pin_set(PIN_nWE, OFF);
        controlbus_update_output();
        _usleep(params->delay);

        // change I/O pins
        iobus_set_value(data[k]);
        iobus_update_output();
        _usleep(params->delay); /* TODO: assure setup delay */

        // toggle nWE back high (acts as clock to latch the current address byte!)
        controlbus_pin_set(PIN_nWE, ON);
        controlbus_update_output();
        _usleep(params->delay); /* TODO: assure hold delay */
    }

    return 0;
}

/**
 * Page Program
 *
 * "The device is programmed by page.
 * The number of consecutive partial page programming operation within the same page
 * without an intervening erase operation must not exceed 8 times.
 *
 * The addressing should be done on each pages in a block.
 * A page program cycle consists of a serial data loading period in which up to 2112 bytes of data
 * may be loaded into the data register, followed by a non-volatile programming period where the loaded data
 * is programmed into the appropriate cell.
 *
 * The serial data loading period begins by inputting the Serial Data Input command (80h),
 * followed by the five cycle address inputs and then serial data.
 *
 * The bytes other than those to be programmed do not need to be loaded.
 *
 * The device supports random data input in a page.
 * The column address of next data, which will be entered, may be changed to the address which follows
 * random data input command (85h).
 * Random data input may be operated multiple times regardless of how many times it is done in a page.
 *
 * The Page Program confirm command (10h) initiates the programming process.
 * Writing 10h alone without pre-viously entering the serial data will not initiate the programming process.
 * The internal write state controller automatically executes the algorithms and timings necessary for
 * program and verify, thereby freeing the system controller for other tasks.
 * Once the program process starts, the Read Status Register command may be entered to read the status register.
 * The system controller can detect the completion of a program cycle by monitoring the R/B output,
 * or the Status bit (I/O 6) of the Status Register.
 * Only the Read Status command and Reset command are valid while programming is in progress.
 *
 * When the Page Program is complete, the Write Status Bit (I/O 0) may be checked.
 * The internal write verify detects only errors for "1"s that are not successfully programmed to "0"s.
 *
 * The command register remains in Read Status command mode until another valid command is written to the
 * command register.
 */
int program_page(prog_params_t *params, unsigned int page, unsigned char* data)
{
    uint32_t mem_address;
    unsigned char addr_cycles[5];

    mem_address = page* PAGE_SIZE_NOSPARE;
    printf("Writing data to page %u, memory address 0x%02X\n", page, mem_address);

    /* remove write protection */
    controlbus_pin_set(PIN_nWP, ON);

    get_address_cycle_map_x8_toshiba_page(page, 0, addr_cycles);
    DBG("  Address cycles are: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
        addr_cycles[0], addr_cycles[1], /* column address */
        addr_cycles[2], addr_cycles[3], addr_cycles[4]); /* row address */

    DBG("Latching first command byte to write a page (page size is %d)...\n",
            PAGE_SIZE);
    latch_command(params, CMD_PAGEPROGRAM[0]); /* Serial Data Input command */

    DBG("Latching address cycles...\n");
    latch_address(params, addr_cycles, 5);

    DBG("Latching out the data of the page...\n");
    latch_data_out(params, data, PAGE_SIZE);

    DBG("Latching second command byte to write a page...\n");
    latch_command(params, CMD_PAGEPROGRAM[1]); /* Page Program confirm command command */

    // busy-wait for high level at the busy line
    wait_while_busy();

    /* Read status */
    DBG("Latching command byte to read status...\n");
    latch_command(params, CMD_READSTATUS);

    unsigned char status_register;
    latch_register(params, &status_register, 1); /* data output operation */

    /* output the retrieved status register content */
    printf("  Status register content:   0x%02X\n", status_register);

    /* activate write protection again */
    controlbus_pin_set(PIN_nWP, OFF);

    if (status_register & STATUSREG_IO0)
    {
        fprintf(stderr, "Failed to program page %u.\n", page);
        return 1;
    }

    printf("  => Successfully programmed page %u.\n", page);
    return 0;
}

/*
 * Return 1 if the given buffer is all the same value, 0 if at least one byte
 * is different.
 */
int is_all_val(unsigned char *b, int len, unsigned char val)
{
    unsigned char *p = b;
    for (int i = 0; i < len; i++, p++) 
    {
        if (*p != val) 
        {
            return 0;
        }
    }

    return 1;
}

/*
 * Program params->count pages of the given file (params->input_file) 
 * into the flash starting at page params->start_page.
 */
int program_file(prog_params_t *params)
{
    if (params->input_file == NULL)
    {
        fprintf(stderr, "error: no input_file specified\n");
        return -1;
    }

    FILE *f = fopen(params->input_file, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "Error: can't open input data file: %s\n", params->input_file);
        return -1;
    }

    unsigned char *buf = malloc(PAGE_SIZE);
    if (buf == NULL) 
    {
        fprintf(stderr, "malloc error, size=%d\n", PAGE_SIZE);
        fclose(f);
        return -1;
    }

    if (params->input_skip)
    {
        long skip_bytes = params->input_skip * PAGE_SIZE;
        printf("Skipping %d pages from input file (%ld bytes)\n", 
               params->input_skip, skip_bytes);
        fseek(f, skip_bytes, SEEK_SET);
        if (ftell(f) != skip_bytes)
        {
            fprintf(stderr, "Seek failed, aborting\n");
            free(buf);
            fclose(f);
            return -1;
        }
    }

    int count = params->count;
    if (count == 0)
    {
        count = DEFAULT_PAGE_COUNT - params->start_page;
    }

    int n = 0;
    int programmed = 0, skipped = 0;
    unsigned int page_idx = params->start_page;
    while (n < count && fread(buf, PAGE_SIZE, 1, f))
    {
        // Skip pages that are purely 0xFFs (NAND only programs bits to 0)
        // HACK: also skip pages that are purely 0x00s as these might have come 
        //   from bad blocks, and flashing them would turn possibly good blocks
        //   into marked-as-bad blocks
        // TODO: This code will blindly attempt to write over factory bad blocks,
        //   possibly loosing factory bad block information. 
        if (!is_all_val(buf, PAGE_SIZE, 0xFF) && !is_all_val(buf, PAGE_SIZE, 0x00))
        {
            programmed++;
            int ret = program_page(params, page_idx, buf);
            if (ret != 0)
            {
                fprintf(stderr, "Program error on page=%d (0x%x), file buf %d; "
                                "aborting programming\n", 
                        page_idx, page_idx, n);
                free(buf);
                fclose(f);
                return -1;
            }
        }
        else 
        {
            skipped++;
        }
        page_idx++;
        n++;
    }

    printf("Went over %d pages, programmed %d pages, empty skipped %d\n", n, programmed, skipped);

    free(buf);
    fclose(f);

    return 0;
}

/*
 * Erase params->count blocks, starting at block params->start_block.
 */
int erase_flash(prog_params_t *params)
{
    int count = params->count; /* BLOCK count in this case */
    if (count == 0)
    {
        count = BLOCK_COUNT - params->start_block;
    }

    unsigned int block = params->start_block;
    for (int i = 0; i < count; i++) 
    {
        printf("Erasing block %u (%d/%d, %.1f%%)\n", block, i+1, count, ((i+1) * 100.0) / count);
        if (erase_block(params, block)) 
        {
            return -1;
        }

        block++;
    }

    return 0;
}

void run_tests(prog_params_t *params)
{
    printf("Running visual tests; it is recommended you DON'T have a chip "
           "connected to the rig when this is going on... sleeping 5 seconds, "
           "press CTRL-C NOW if you want to abort...\n");

    _usleep(5* 1000000);

    printf("testing control bus, check visually...\n");
    _usleep(2* 1000000);
    test_controlbus();

    printf("testing I/O bus for output, check visually...\n");
    _usleep(2* 1000000);
    test_iobus();
}

void close_bus(struct ftdi_context *bus, char *msg)
{
    printf("%s", msg);
    ftdi_disable_bitbang(bus);
    ftdi_usb_close(bus);
    ftdi_free(bus);
}

void close_busses()
{
    close_bus(nandflash_iobus, "disabling bitbang mode (channel 1)\n");
    close_bus(nandflash_controlbus, "disabling bitbang mode (channel 2)\n");
}

int main(int argc, char **argv)
{
    struct ftdi_version_info version;
    unsigned char ID_register[5];
    int f;
    prog_params_t params;

    if (parse_prog_params(&params, argc, argv))
    {
       return 1;
    }

    print_prog_params(&params);
    printf("Current NAND params: page size: %d, page size (w/ OOB): %d, "
           "pages per block: %d, block count: %d, page count: %d\n",
           PAGE_SIZE_NOSPARE, PAGE_SIZE, PAGE_PER_BLOCK, BLOCK_COUNT,
           DEFAULT_PAGE_COUNT);

    if (!params.do_program && !params.do_erase 
        && !access(params.filename, F_OK) && !params.overwrite)
    {
        printf("File already exists, use -o to overwrite: %s\n", params.filename);
        return 2;
    }

    // show library version
    version = ftdi_get_library_version();
    printf("Initialized libftdi %s (major: %d, minor: %d, micro: %d,"
        " snapshot ver: %s)\n", version.version_str, version.major,
        version.minor, version.micro, version.snapshot_str);

    // Init 1. channel for databus
    if ((nandflash_iobus = ftdi_new()) == NULL)
    {
        fprintf(stderr, "ftdi_new failed for iobus\n");
        return EXIT_FAILURE;
    }

    ftdi_set_interface(nandflash_iobus, INTERFACE_A);
    f = ftdi_usb_open(nandflash_iobus, FT2232H_VID, FT2232H_PID);
    if (f < 0 && f != -5)
    {
        fprintf(stderr, "unable to open ftdi device: %d (%s)  --  " \
                        "Should you run as root?\n", f,
          ftdi_get_error_string(nandflash_iobus));
        ftdi_free(nandflash_iobus);
        exit(-1);
    }
    printf("ftdi open succeeded(channel 1): %d\n", f);

    printf("enabling bitbang mode(channel 1)\n");
    ftdi_set_bitmode(nandflash_iobus, IOBUS_BITMASK_WRITE, BITMODE_BITBANG);

    // Init 2. channel
    if ((nandflash_controlbus = ftdi_new()) == NULL)
    {
        fprintf(stderr, "ftdi_new failed\n");
        return EXIT_FAILURE;
    }
    ftdi_set_interface(nandflash_controlbus, INTERFACE_B);
    f = ftdi_usb_open(nandflash_controlbus, FT2232H_VID, FT2232H_PID);
    if (f < 0 && f != -5)
    {
        fprintf(stderr, "unable to open ftdi device: %d (%s)\n", f,
          ftdi_get_error_string(nandflash_controlbus));
        ftdi_free(nandflash_controlbus);
        exit(-1);
    }
    printf("ftdi open succeeded(channel 2): %d\n",f);

    printf("enabling bitbang mode (channel 2)\n");
    ftdi_set_bitmode(nandflash_controlbus, CONTROLBUS_BITMASK, BITMODE_BITBANG);

    _usleep(500 * 1000);  // 500ms

    controlbus_reset_value();
    controlbus_update_output();

    iobus_set_direction(IOBUS_OUT);
    iobus_reset_value();
    iobus_update_output();

    if (params.test)
    {
        printf("Test mode; running tests, then aborting\n");
        run_tests(&params);

        close_busses();

        return 0;
    }

    printf("testing I/O and control bus for input read...\n");
    iobus_set_direction(IOBUS_IN);
    //while(1)
    {
        unsigned char iobus_val = iobus_read_input();
        unsigned char controlbus_val = controlbus_read_input();
        printf("data read back: iobus=0x%02x, controlbus=0x%02x\n",
                iobus_val, controlbus_val);
        _usleep(1* 1000000);
    }
    iobus_set_direction(IOBUS_OUT);

    // set nRE high and nCE and nWP low
    controlbus_pin_set(PIN_nRE, ON);
    controlbus_pin_set(PIN_nCE, OFF);
    controlbus_pin_set(PIN_nWP, OFF); /* nWP low provides HW protection against undesired modify (program / erase) operations */
    controlbus_update_output();

    // Read the ID register
    {
        printf("Trying to read the ID register...\n");

        latch_command(&params, CMD_READID); /* command input operation; command: READ ID */

        //unsigned char address[] = { 0x11, 0x22, 0x44, 0x88, 0xA5 };
        //latch_address(address, 5);
        unsigned char address[] = { 0x00 };
        latch_address(&params, address, 1); /* address input operation */

        latch_register(&params, ID_register, 5); /* data output operation */

        check_ID_register(ID_register);
    }

    int ret = 0;
    if (params.do_program)
    {
        ret = program_file(&params);
    }
    else if (params.do_erase)
    {
        ret = erase_flash(&params);
    }
    else
    {
        ret = dump_memory(&params);
    }

    // set nCE high
    controlbus_pin_set(PIN_nCE, ON);

    printf("done, 1 sec to go...\n");
    _usleep(1 * 1000000);

    close_busses();

    return ret;
}
