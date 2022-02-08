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
    int page_count;
    int delay; /* delay in usec */
    int test; /* run simple tests instead of dump */
} prog_params_t;


void reset_prog_params(prog_params_t *params)
{
    memset(params, 0, sizeof(*params));
    params->start_page = DEFAULT_START_PAGE;
    params->filename = DEFAULT_FILENAME;
    params->page_count = DEFAULT_PAGE_COUNT;
    params->delay = DEFAULT_DELAY;
}

void print_prog_params(prog_params_t *params)
{
    printf("Params: start_page=%d (%x), page_count=%d, filename=%s, "
           "overwrite=%d, delay=%d, test=%d\n",
        params->start_page,
        params->start_page,
        params->page_count,
        params->filename,
        params->overwrite,
        params->delay,
        params->test);
}

int parse_prog_params(prog_params_t *params, int argc, char **argv)
{
  int index;
  int c;

  reset_prog_params(params);

  opterr = 0;

  while ((c = getopt (argc, argv, "c:d:s:tf:o")) != -1)
    switch (c)
      {
      case 'c':
        params->page_count = atoi(optarg);
        break;
      case 'd':
        params->delay = atoi(optarg);
        break;
      case 'f':
        params->filename = optarg;
        break;
      case 'o':
        params->overwrite = 1;
        break;
      case 's':
        params->start_page = atoi(optarg);
        break;
      case 't':
        params->test = 1;
        break;
      case '?':
        if (strchr("csf", optopt))
          fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        else 
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        return 1;
      default:
        abort ();
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
    if(val == ON)
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
    if( inout == IOBUS_OUT )
        ftdi_set_bitmode(nandflash_iobus, IOBUS_BITMASK_WRITE, BITMODE_BITBANG);
    else if( inout == IOBUS_IN )
        ftdi_set_bitmode(nandflash_iobus, IOBUS_BITMASK_READ, BITMODE_BITBANG);        
}

void iobus_reset_value()
{
    iobus_value = 0x00;
}

void iobus_pin_set(unsigned char pin, onoff_t val)
{
    if(val == ON)
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

/* "Command Input bus operation is used to give a command to the memory device. Command are accepted with Chip
Enable low, Command Latch Enable High, Address Latch Enable low and Read Enable High and latched on the rising
edge of Write Enable. Moreover for commands that starts a modify operation (write/erase) the Write Protect pin must be
high."" */
int latch_command(prog_params_t *params, unsigned char command)
{
    /* check if ALE is low and nRE is high */
    if( controlbus_value & PIN_nCE )
    {
        fprintf(stderr, "latch_command requires nCE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if( ~controlbus_value & PIN_nRE )
    {
        fprintf(stderr, "latch_command requires nRE pin to be high\n");
        return EXIT_FAILURE;
    }

    /* debug info */
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
 * Addresses are accepted with Chip Enable low, Address Latch Enable High, Command Latch Enable low and 
 * Read Enable High and latched on the rising edge of Write Enable.
 * Moreover for commands that starts a modifying operation (write/erase) the Write Protect pin must be high. 
 * See Figure 5 and Table 13 for details of the timings requirements.
 * Addresses are always applied on IO7:0 regardless of the bus configuration (x8 or x16).""
 */
int latch_address(prog_params_t *params, unsigned char address[], unsigned int addr_length)
{
    unsigned int addr_idx = 0;

    /* check if ALE is low and nRE is high */
    if( controlbus_value & PIN_nCE )
    {
        fprintf(stderr, "latch_address requires nCE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if( controlbus_value & PIN_CLE )
    {
        fprintf(stderr, "latch_address requires CLE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if( ~controlbus_value & PIN_nRE )
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
    if( controlbus_value & PIN_nCE )
    {
        fprintf(stderr, "latch_address requires nCE pin to be low\n");
        return EXIT_FAILURE;
    }
    else if( ~controlbus_value & PIN_nWE )
    {
        fprintf(stderr, "latch_address requires nWE pin to be high\n");
        return EXIT_FAILURE;
    }
    else if( controlbus_value & PIN_ALE )
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

    if( strncmp( (char *)ID_register_exp, (char *)ID_register, 5 ) == 0 )
    {
        printf("PASS: ID register did match\n");
    }
    else
    {
        printf("FAIL: ID register did not match\n");
    }
}

/* Address Cycle Map calculations */
void get_address_cycle_map_x8(uint32_t mem_address, unsigned char* addr_cylces)
{
    addr_cylces[0] = (unsigned char)(  mem_address & 0x000000FF);
    addr_cylces[1] = (unsigned char)( (mem_address & 0x00000F00) >> 8 );
    addr_cylces[2] = (unsigned char)( (mem_address & 0x000FF000) >> 12 );
    addr_cylces[3] = (unsigned char)( (mem_address & 0x0FF00000) >> 20 );
    addr_cylces[4] = (unsigned char)( (mem_address & 0x30000000) >> 28 );
}

int dump_memory(prog_params_t *params)
{
    FILE *fp;
    unsigned int page_idx;
    unsigned int page_idx_max;
    unsigned char addr_cylces[5];
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

    // Start reading the data
    page_idx_max = params->start_page + params->page_count;
    for( page_idx = params->start_page; page_idx < page_idx_max; /* blocks per page * overall blocks */ page_idx++)
    {
      mem_address = page_idx * PAGE_SIZE_NOSPARE; // start address
      printf("Reading data from page %d / %d (%.2f %%), address: %08X\n", 
             page_idx, page_idx_max, (float)page_idx/(float)page_idx_max * 100,
             mem_address);
      {

          DBG("Latching first command byte to read a page: ");
          latch_command(params, CMD_READ1[0]);

          get_address_cycle_map_x8(mem_address, addr_cylces);
          DBG("Latching address cycles: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
              addr_cylces[0], addr_cylces[1], /* column address */
              addr_cylces[2], addr_cylces[3], addr_cylces[4] ); /* row address */
          latch_address(params, addr_cylces, 5);

          DBG("Latching second command byte to read a page: ");
          latch_command(params, CMD_READ1[1]);

          // busy-wait for high level at the busy line
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
 * Block address loading is accomplished in there cycles initiated by an Erase Setup command (60h).
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
int erase_block(prog_params_t *params, unsigned int nBlockId)
{
	uint32_t mem_address;
	unsigned char addr_cylces[5];

	/* calculate memory address */
	mem_address = 2048 * 64 * nBlockId; // (2K + 64) bytes x 64 pages per block

	/* remove write protection */
	controlbus_pin_set(PIN_nWP, ON);

	printf("Latching first command byte to erase a block...\n");
	latch_command(params, CMD_BLOCKERASE[0]); /* block erase setup command */

	printf("Erasing block of data from memory address 0x%02X\n", mem_address);
	get_address_cycle_map_x8(mem_address, addr_cylces);
	printf("  Address cycles are (but: will take only cycles 3..5) : 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
		addr_cylces[0], addr_cylces[1], /* column address */
		addr_cylces[2], addr_cylces[3], addr_cylces[4] ); /* row address */

	printf("Latching page(row) address (3 bytes)...\n");
	unsigned char address[] = { addr_cylces[2], addr_cylces[3], addr_cylces[4] };
	latch_address(params, address, 3);

	printf("Latching second command byte to erase a block...\n");
	latch_command(params, CMD_BLOCKERASE[1]);

	/* tWB: WE High to Busy is 100 ns -> ignore it here as it takes some time for the next command to execute */

	// busy-wait for high level at the busy line
	printf("Checking for busy line...\n");
	unsigned char controlbus_val;
	do
	{
		controlbus_val = controlbus_read_input();
	}
	while( !(controlbus_val & PIN_RDY) );

	printf("  done\n");


	/* Read status */
	printf("Latching command byte to read status...\n");
	latch_command(params, CMD_READSTATUS);

	unsigned char status_register;
	latch_register(params, &status_register, 1); /* data output operation */

	/* output the retrieved status register content */
	printf("Status register content:   0x%02X\n", status_register);


	/* activate write protection again */
	controlbus_pin_set(PIN_nWP, OFF);


	if(status_register & STATUSREG_IO0)
	{
		fprintf(stderr, "Failed to erase block.\n");
		return 1;
	}
	else
	{
		printf("Successfully erased block.\n");
		return 0;
	}
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
int program_page(prog_params_t *params, unsigned int nPageId, unsigned char* data)
{
	uint32_t mem_address;
    unsigned char addr_cylces[5];

    mem_address = nPageId * PAGE_SIZE;

	/* remove write protection */
	controlbus_pin_set(PIN_nWP, ON);

	printf("Writing data to memory address 0x%02X\n", mem_address);
    get_address_cycle_map_x8(mem_address, addr_cylces);
    printf("  Address cycles are: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
        addr_cylces[0], addr_cylces[1], /* column address */
        addr_cylces[2], addr_cylces[3], addr_cylces[4] ); /* row address */

	printf("Latching first command byte to write a page (page size is %d)...\n",
			PAGE_SIZE);
	latch_command(params, CMD_PAGEPROGRAM[0]); /* Serial Data Input command */

	printf("Latching address cycles...\n");
    latch_address(params, addr_cylces, 5);

	printf("Latching out the data of the page...\n");
	latch_data_out(params, data, PAGE_SIZE);

	printf("Latching second command byte to write a page...\n");
	latch_command(params, CMD_PAGEPROGRAM[1]); /* Page Program confirm command command */

	// busy-wait for high level at the busy line
	printf("Checking for busy line...\n");
	unsigned char controlbus_val;
	do
	{
		controlbus_val = controlbus_read_input();
		printf(".");
	}
	while( !(controlbus_val & PIN_RDY) );

	printf("  done\n");


	/* Read status */
	printf("Latching command byte to read status...\n");
	latch_command(params, CMD_READSTATUS);

	unsigned char status_register;
	latch_register(params, &status_register, 1); /* data output operation */

	/* output the retrieved status register content */
	printf("Status register content:   0x%02X\n", status_register);


	/* activate write protection again */
	controlbus_pin_set(PIN_nWP, OFF);


	if(status_register & STATUSREG_IO0)
	{
		fprintf(stderr, "Failed to program page.\n");
		return 1;
	}
	else
	{
		printf("Successfully programmed page.\n");
		return 0;
	}
}

void get_page_dummy_data(unsigned char* page_data)
{
	for(unsigned int k=0; k<PAGE_SIZE_NOSPARE; k++)
	{
		unsigned int m = k % 8;
		switch( m )
		{
			case 0:
			case 4:
				page_data[k] = 0xDE;
				break;
			case 1:
			case 5:
				page_data[k] = 0xAD;
				break;
			case 2:
			case 6:
				page_data[k] = 0xBE;
				break;
			case 3:
			case 7:
				page_data[k] = 0xEF;
				break;
		}
	}
	for(unsigned int k=PAGE_SIZE_NOSPARE; k<PAGE_SIZE; k++)
	{
		page_data[k] = 0x11;
	}
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

    if (!access(params.filename, F_OK) && !params.overwrite)
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
    if ((nandflash_iobus = ftdi_new()) == 0)
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
    if ((nandflash_controlbus = ftdi_new()) == 0)
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


    /*printf("testing control bus, check visually...\n");
    _usleep(2* 1000000);
    test_controlbus();

    printf("testing I/O bus for output, check visually...\n");
    _usleep(2* 1000000);
    test_iobus();*/

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

	/* Erase all blocks */
//    for( unsigned int nBlockId = 0; nBlockId < 4096; nBlockId++ )
//    {
//    	erase_block(nBlockId);
//    }
    /* Erase block #0 */
//	erase_block(0);
//
//	_usleep(1* 1000000);
//
//	/* Write pages 0..9 with dummy data */
//    unsigned char page_data[PAGE_SIZE];
//    get_page_dummy_data(page_data);
//
//    for(unsigned int m=0; m<10; m++)
//    {
//		program_page(0, page_data);
//		_usleep(1* 1000000);
//    }

    /* Dump memory of the chip */
    dump_memory(&params);


    // set nCE high
    controlbus_pin_set(PIN_nCE, ON);


    printf("done, 1 sec to go...\n");
    _usleep(1 * 1000000);

    close_busses();

    return 0;
}
