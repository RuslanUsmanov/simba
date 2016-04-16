/**
 * @file console.c
 * @version 0.3.0
 *
 * @section License
 * Copyright (C) 2015-2016, Erik Moqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

#include "bootloader.h"

#define START_OF_HEADING 0x01
#define END_OF_TEXT      0x03

#define SPACE            ' '

/* Packet types. */
#define PACKET_TYPE_SEND         'S'
#define PACKET_TYPE_DATA         'D'
#define PACKET_TYPE_ACK          'Y'
#define PACKET_TYPE_BREAK        'B'

/* Configuration. */
#define PACKET_LENGTH_MAX            94
#define TIMEOUT                       1
#define PAD_COUNT                     0
#define PAD_VALUE                   '\0'
#define END_OF_LINE                 '\r'
#define QUOTE_CONTROL                '#'
#define REFUSE                       'N'
#define CLOCK_CHECK_TYPE             '1'
#define LONG_PACKETS                  2
#define NO_WINDOWS                    0
#define LONG_PACKETS_LENGTH_MAX_MSB   1
#define LONG_PACKETS_LENGTH_MAX_LSB  94

struct kermit_t {
    chan_t *chin_p;
    chan_t *chout_p;
    uint32_t application_address;
    uint32_t application_size;
    struct flash_driver_t *flash_p;
    struct {
        uint32_t address;
    } swdl;
    struct {
        int end_of_line;
        int pad_value;
        int pad_count;
    } peer;
    struct {
        uint8_t buf[256];
        size_t size;
    } input;
};

FS_COMMAND_DEFINE("/application/erase", cmd_application_erase);
FS_COMMAND_DEFINE("/application/is_valid", cmd_application_is_valid);
FS_COMMAND_DEFINE("/application/load_kermit", cmd_application_load_kermit);
FS_COMMAND_DEFINE("/application/start", cmd_application_start);

static struct kermit_t state;
static struct shell_args_t shell_args;

/**
 * Encode given binary value to a printable ascii value.
 */
static inline uint8_t encode(int value)
{
    return ((uint8_t)value + SPACE);
}

/**
 * Decode given printable ascii value to a binary value.
 */
static inline int decode(uint8_t value)
{
    return ((int)(value - SPACE));
}

/**
 * Unescape a data byte.
 */
static uint8_t unescape(uint8_t value)
{
    if ((value & 0x60) == 0x40) {
        return (value & 0xbf);
    } else if ((value & 0x7f) == 0x3f) {
        return (value | 0x40);
    } else {
        return (value);
    }
}

/**
 * Calculate the checksum of given buffer.
 */
static int checksum(uint8_t *buf_p, size_t length)
{
    int checksum = 0;

    while (length > 0) {
        checksum += *buf_p++;
        length--;
    }

    return ((checksum + ((checksum >> 6) & 0x03)) & 0x3f);
}

static int write_ack(void *out_p,
                     void *in_p,
                     int sequence_number)
{
    uint8_t response[6];

    /* Build the response packet. */
    response[0] = START_OF_HEADING;
    response[1] = encode(3);
    response[2] = encode(sequence_number);
    response[3] = PACKET_TYPE_ACK;
    response[4] = encode(checksum(&response[1], 3));
    response[5] = END_OF_LINE;

    chan_write(out_p, response, sizeof(response));

    return (0);
}

/**
 * Handle a send packet.
 */
static int handle_send(void *out_p,
                       void *in_p,
                       int sequence_number)
{
    uint8_t response[20];

    /* Build the response packet. */
    response[0] = START_OF_HEADING;
    response[1] = encode(16);
    response[2] = encode(sequence_number);
    response[3] = PACKET_TYPE_ACK;
    response[4] = encode(PACKET_LENGTH_MAX);
    response[5] = encode(TIMEOUT);
    response[6] = encode(PAD_COUNT);
    response[7] = encode(PAD_VALUE);
    response[8] = encode(END_OF_LINE);
    response[9] = QUOTE_CONTROL;
    response[10] = REFUSE;
    response[11] = CLOCK_CHECK_TYPE;
    response[12] = REFUSE;
    response[13] = encode(LONG_PACKETS);
    response[14] = encode(NO_WINDOWS);
    response[15] = encode(LONG_PACKETS_LENGTH_MAX_MSB);
    response[16] = encode(LONG_PACKETS_LENGTH_MAX_LSB);
    response[17] = encode(checksum(&response[1], 16));
    response[18] = END_OF_LINE;

    chan_write(out_p, response, 19);

    return (0);
}

/**
 * Handle a data packet.
 */
static int handle_data(void *out_p,
                       void *in_p,
                       int sequence_number)
{
    size_t i, j;

    /* Decode the buffer. */
    for (i = 0, j = 0; i < state.input.size; i++, j++) {
        if (state.input.buf[i] == QUOTE_CONTROL) {
            i++;
            state.input.buf[j] = unescape(state.input.buf[i]);
        } else {
            state.input.buf[j] = state.input.buf[i];
        }
    }

    flash_write(state.flash_p,
                state.swdl.address,
                state.input.buf,
                j);

    state.swdl.address += j;

    return (write_ack(out_p, in_p, sequence_number));
}

/**
 * Handle other packets.
 */
static int handle_default(void *out_p,
                          void *in_p,
                          int sequence_number)
{
    return (write_ack(out_p, in_p, sequence_number));
}

/**
 * Read a packet from the input channel and process it.
 */
static int handle_packet(void *out_p,
                         void *in_p)
{
    uint8_t value;
    int length, length_msb, length_lsb;
    int sequence_number;
    int type;
    int actual_checksum, expected_checksum;

    /* Wait for a packet. */
    while (1) {
        chan_read(in_p, &value, sizeof(value));

        if (value == START_OF_HEADING) {
            break;
        } else if (value == END_OF_TEXT) {
            return (-1);
        }
    }

    actual_checksum = 0;

    /* Length. */
    chan_read(in_p, &value, sizeof(value));
    length = decode(value);
    actual_checksum += value;

    /* Sequence number. */
    chan_read(in_p, &value, sizeof(value));
    sequence_number = decode(value);
    actual_checksum += value;
    length--;

    /* Type. */
    chan_read(in_p, &value, sizeof(value));
    type = value;
    actual_checksum += value;
    length--;

    /* Extended length. */
    if (length == -2) {
        chan_read(in_p, &value, sizeof(value));
        actual_checksum += value;
        length_msb = decode(value);

        chan_read(in_p, &value, sizeof(value));
        actual_checksum += value;
        length_lsb = decode(value);

        length = (95 * length_msb + length_lsb);

        chan_read(in_p, &value, sizeof(value));
        /* Should compare the checksums here. */
        actual_checksum += value;
    }

    /* Read the packet. */
    state.input.size = 0;

    while (length > 1) {
        chan_read(in_p, &value, sizeof(value));
        actual_checksum += value;
        length--;
        state.input.buf[state.input.size++] = value;
    }

    actual_checksum += ((actual_checksum >> 6) & 0x03);
    actual_checksum &= 0x3f;
    actual_checksum = encode(actual_checksum);

    /* Read the checksum. */
    chan_read(in_p, &value, sizeof(value));
    expected_checksum = value;

    /* Comapare the actual checksum to the expected checksum. */
    if (actual_checksum != expected_checksum) {
        std_printf(FSTR("error: actual_checksum = 0x%02x, "
                        "expected_checksum = 0x%02x\r\n"),
                   actual_checksum,
                   expected_checksum);

        return (-1);
    }

    /* Read end character. */
    chan_read(in_p, &value, sizeof(value));

    if (value != END_OF_LINE) {
        std_printf(FSTR("error: bad end character %d\r\n"), value);

        return (-1);
    }

    switch (type) {

    case PACKET_TYPE_SEND:
        handle_send(out_p, in_p, sequence_number);
        break;

    case PACKET_TYPE_DATA:
        handle_data(out_p, in_p, sequence_number);
        break;

    case PACKET_TYPE_BREAK:
        write_ack(out_p, in_p, sequence_number);
        std_fprintf(out_p,
                    FSTR("File transfer completed successfully.\r\n"));
        return (1);

    default:
        handle_default(out_p, in_p, sequence_number);
        break;
    }

    return (0);
}

/**
 * Shell command that erases the application from the flash memory.
 */
int cmd_application_erase(int argc,
                          const char *argv[],
                          void *out_p,
                          void *in_p)
{
    bootloader_erase_application(state.application_address,
                                 state.application_size,
                                 state.flash_p);

    return (0);
}

/**
 * Shell command that writes a file to flash using the kermit
 * protocol.
 */
int cmd_application_is_valid(int argc,
                             const char *argv[],
                             void *out_p,
                             void *in_p)
{
    int is_valid;

    is_valid = bootloader_is_application_valid(state.application_address,
                                               state.application_size);
    std_fprintf(out_p, FSTR("%s\r\n"), is_valid ? "yes" : "no");

    return (0);
}

/**
 * Shell command that receives a file over the Kermit file transfer
 * protocol and writes it to flash.
 */
int cmd_application_load_kermit(int argc,
                                const char *argv[],
                                void *out_p,
                                void *in_p)
{
    int res;

    /* Initialize the software download state. */
    state.swdl.address = state.application_address;

    std_printf(FSTR("Ready to receive a file over the Kermit file "
                    "transfer protocol.\r\n"
                    "\r\n"
                    "In kermit; press '\\+c' and use the kermit command "
                    "'send' to start the file transfer. Type 'c' and press "
                    "Enter after the file transfer to return to the "
                    "bootloader shell.\r\n"));

    while (1) {
        res = handle_packet(out_p, in_p);

        if (res == 1) {
            bootloader_write_application_valid_flag(state.application_address,
                                                    state.application_size,
                                                    state.flash_p);

            std_printf(FSTR("software download successful\r\n"));
            break;
        } else if (res < 0) {
            std_printf(FSTR("error: software download failed\r\n"));
            break;
        }
    }

    return (0);
}

/**
 * Start the application.
 */
int cmd_application_start(int argc,
                          const char *argv[],
                          void *out_p,
                          void *in_p)
{
# if !defined(BOOTLOADER_TEST)
    if (bootloader_is_application_valid(state.application_address,
                                        state.application_size) == 0) {
        std_printf(FSTR("error: no valid application found\r\n"));

        return (-1);
    }
#endif

    std_printf(FSTR("starting the application\r\n"));

    /* Call the application. */
# if !defined(BOOTLOADER_TEST)
    bootloader_jump(state.application_address);
#endif

    return (0);
}

int console_init(chan_t *chin_p,
                 chan_t *chout_p,
                 uint32_t application_address,
                 uint32_t application_size,
                 struct flash_driver_t *flash_p)
{
    state.chin_p = chin_p;
    state.chout_p = chout_p;
    state.application_address = application_address;
    /* Subtract one for the application valid flag. */
    state.application_size = (application_size - 1);
    state.flash_p = flash_p;

    return (0);
}

void console_main()
{
    struct pin_driver_t stay_in_bootloader_pin;

    pin_init(&stay_in_bootloader_pin, &pin_d2_dev, PIN_INPUT);

    /* Check the "stay in bootloader" pin. */
    if (pin_read(&stay_in_bootloader_pin) == 0) {
        std_printf(FSTR("stay in bootloader pin (d2) low\r\n"));

        /* Call the application if it is valid. */
        if (bootloader_is_application_valid(state.application_address,
                                            state.application_size) == 1) {
            std_printf(FSTR("calling application\r\n"));
            bootloader_jump(state.application_address);
        } else {
            std_printf(FSTR("application invalid\r\n"));
        }
    }

    std_printf(FSTR("staying in the bootloader\r\n"));

    /* Enter the bootloader main loop. */
    shell_args.username_p = NULL;
    shell_args.password_p = NULL;
    shell_args.chin_p = state.chin_p;
    shell_args.chout_p = state.chout_p;
    shell_main(&shell_args);
}