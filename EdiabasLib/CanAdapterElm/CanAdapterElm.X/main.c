/*
 * File:   main.c
 * Author: Ulrich
 *
 * Created on 16. Oktober 2015, 09:58
 */


#include <xc.h>
#include <p18cxxx.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "can.h"

// #pragma config statements should precede project file includes.
// Use project enums instead of #define for ON and OFF.

// CONFIG1L
#pragma config RETEN = ON       // VREG Sleep Enable bit (Ultra low-power regulator is Enabled (Controlled by SRETEN bit))
#pragma config INTOSCSEL = HIGH // LF-INTOSC Low-power Enable bit (LF-INTOSC in High-power mode during Sleep)
#pragma config SOSCSEL = DIG    // SOSC Power Selection and mode Configuration bits (Digital (SCLKI) mode)
#pragma config XINST = OFF      // Extended Instruction Set (Disabled)

// CONFIG1H
#pragma config FOSC = HS1       // Oscillator (HS oscillator (Medium power, 4 MHz - 16 MHz))
#pragma config PLLCFG = ON      // PLL x4 Enable bit (Enabled)
#pragma config FCMEN = OFF      // Fail-Safe Clock Monitor (Disabled)
#pragma config IESO = OFF       // Internal External Oscillator Switch Over Mode (Disabled)

// CONFIG2L
#pragma config PWRTEN = ON      // Power Up Timer (Enabled)
#pragma config BOREN = SBORDIS  // Brown Out Detect (Enabled in hardware, SBOREN disabled)
#pragma config BORV = 0         // Brown-out Reset Voltage bits (3.0V)
#pragma config BORPWR = HIGH    // BORMV Power level (BORMV set to high power level)

// CONFIG2H
#pragma config WDTEN = ON       // Watchdog Timer (WDT controlled by SWDTEN bit setting)
#pragma config WDTPS = 128      // Watchdog Postscaler (1:128)

// CONFIG3H
#pragma config CANMX = PORTB    // ECAN Mux bit (ECAN TX and RX pins are located on RB2 and RB3, respectively)
#pragma config MSSPMSK = MSK7   // MSSP address masking (7 Bit address masking mode)
#pragma config MCLRE = ON       // Master Clear Enable (MCLR Enabled, RE3 Disabled)

// CONFIG4L
#pragma config STVREN = ON      // Stack Overflow Reset (Enabled)
#pragma config BBSIZ = BB1K     // Boot Block Size (1K word Boot Block size)

// CONFIG5L
#pragma config CP0 = OFF        // Code Protect 00800-01FFF (Disabled)
#pragma config CP1 = OFF        // Code Protect 02000-03FFF (Disabled)
#pragma config CP2 = OFF        // Code Protect 04000-05FFF (Disabled)
#pragma config CP3 = OFF        // Code Protect 06000-07FFF (Disabled)

// CONFIG5H
#pragma config CPB = OFF        // Code Protect Boot (Disabled)
#pragma config CPD = OFF        // Data EE Read Protect (Disabled)

// CONFIG6L
#pragma config WRT0 = OFF       // Table Write Protect 00800-01FFF (Disabled)
#pragma config WRT1 = OFF       // Table Write Protect 02000-03FFF (Disabled)
#pragma config WRT2 = OFF       // Table Write Protect 04000-05FFF (Disabled)
#pragma config WRT3 = OFF       // Table Write Protect 06000-07FFF (Disabled)

// CONFIG6H
#pragma config WRTC = OFF       // Config. Write Protect (Disabled)
#pragma config WRTB = OFF       // Table Write Protect Boot (Disabled)
#pragma config WRTD = OFF       // Data EE Write Protect (Disabled)

// CONFIG7L
#pragma config EBTR0 = OFF      // Table Read Protect 00800-01FFF (Disabled)
#pragma config EBTR1 = OFF      // Table Read Protect 02000-03FFF (Disabled)
#pragma config EBTR2 = OFF      // Table Read Protect 04000-05FFF (Disabled)
#pragma config EBTR3 = OFF      // Table Read Protect 06000-07FFF (Disabled)

// CONFIG7H
#pragma config EBTRB = OFF      // Table Read Protect Boot (Disabled)

#define LED_RS_RX LATBbits.LATB4
#define LED_RS_TX LATBbits.LATB5
#define LED_OBD_RX LATBbits.LATB6
#define LED_OBD_TX LATBbits.LATB7

#define TIMER0_RELOAD (0x100-156)       // 10 ms
#define TIMER1_RELOAD (0x10000-500)     // 1 ms

#define CAN_MODE            1       // default can mode (1=500kb)
#define CAN_BLOCK_SIZE      0       // 0 is disabled
#define CAN_MIN_SEP_TIME    0       // min separation time (ms)

#define EEP_ADDR_BAUD       0x00    // eeprom address for baud setting (2 bytes)
#define EEP_ADDR_BLOCKSIZE  0x02    // eeprom address for FC block size (2 bytes)
#define EEP_ADDR_SEP_TIME   0x04    // eeprom address for FC separation time (2 bytes)
#define EEP_ADDR_PLD_MODE   0x06    // eeprom address for PLD mode

// receiver state machine
typedef enum
{
    rec_state_idle,     // wait
    rec_state_rec,      // receive
    rec_state_done,     // receive complete, ok
    rec_state_error,    // receive error
} rec_states;

// wait types
typedef enum
{
    wait_off,           // no wait
    wait_1ms,           // wait in 1ms units
    wait_10ms,          // wait in 10ms units
} wait_types;

static volatile uint8_t time_tick_10;  // time tick 10 ms

static volatile rec_states rec_state;
static volatile uint16_t rec_len;
static uint8_t rec_chksum;
static volatile uint8_t rec_buffer[260];

static uint16_t send_set_idx;
static uint16_t send_get_idx;
static volatile uint16_t send_len;
static volatile uint8_t send_buffer[280];   // larger send buffer for multi responses

static uint8_t temp_buffer[260];

static bool can_enabled;
static uint8_t can_mode;
static uint8_t can_blocksize;
static uint8_t can_sep_time;

bool uart_send(uint8_t *buffer, uint16_t count)
{
    uint16_t volatile temp_len;

    if (count == 0)
    {
        return true;
    }
    di();
    temp_len = send_len;
    ei();

    if (temp_len + count > sizeof(send_buffer))
    {
        return false;
    }

    for (uint16_t i = 0; i < count; i++)
    {
        send_buffer[send_set_idx++] = buffer[i];
        if (send_set_idx >= sizeof(send_buffer))
        {
            send_set_idx = 0;
        }
    }

    di();
    send_len += count;
    PIE1bits.TXIE = 1;    // enable TX interrupt
    ei();
    return true;
}

uint16_t uart_receive(uint8_t *buffer)
{
    if (rec_state != rec_state_done)
    {
        return 0;
    }

    uint16_t data_len = rec_len;
    memcpy(buffer, (void *) rec_buffer, data_len);
    rec_state = rec_state_idle;
    return data_len;
}

uint8_t calc_checkum(uint8_t *buffer, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        sum += buffer[i];
    }
    return sum;
}

void can_config()
{
    uint8_t bitrate = 5;
    switch (can_mode)
    {
        case 0:     // can off
            can_enabled = false;
            break;

        default:
        case 1:     // can 500kb
            bitrate = 5;
            can_enabled = true;
            break;

        case 9:     // can 100kb
            bitrate = 1;
            can_enabled = true;
            break;
    }
    if (can_enabled)
    {
        // 100kb: SJW_1_TQ, BRP_FOSC_4, PSEG1T_8_TQ, PRGT_3_TQ, PSEG2T_8_TQ
        // 500kb: SJW_1_TQ, BRP_FOSC_2, PSEG1T_8_TQ, PRGT_1_TQ, PSEG2T_6_TQ
        if (bitrate == 1)
        {   // 100 kb
            open_can(SJW_1_TQ, BRP_FOSC_4, PSEG1T_8_TQ, PRGT_3_TQ, PSEG2T_8_TQ,
                0x600, 0x700);
        }
        else
        {   // 500kb
            open_can(SJW_1_TQ, BRP_FOSC_2, PSEG1T_8_TQ, PRGT_1_TQ, PSEG2T_6_TQ,
                0x600, 0x700);    // B
        }
    }
    else
    {
        close_can();
    }
}

void read_eeprom()
{
    uint8_t temp_value1;
    uint8_t temp_value2;

      // wait for write to finish
    while(WR) continue;

    temp_value1 = eeprom_read(EEP_ADDR_BAUD);
    temp_value2 = eeprom_read(EEP_ADDR_BAUD + 1);
    can_mode = CAN_MODE;
    if ((~temp_value1 & 0xFF) == temp_value2)
    {
        can_mode = temp_value1;
    }

    temp_value1 = eeprom_read(EEP_ADDR_BLOCKSIZE);
    temp_value2 = eeprom_read(EEP_ADDR_BLOCKSIZE + 1);
    can_blocksize = CAN_BLOCK_SIZE;
    if ((~temp_value1 & 0xFF) == temp_value2)
    {
        can_blocksize = temp_value1;
    }

    temp_value1 = eeprom_read(EEP_ADDR_SEP_TIME);
    temp_value2 = eeprom_read(EEP_ADDR_SEP_TIME + 1);
    can_sep_time = CAN_MIN_SEP_TIME;
    if ((~temp_value1 & 0xFF) == temp_value2)
    {
        can_sep_time = temp_value1;
    }
}

bool internal_telegram(uint16_t len)
{
    if ((len == 5) &&
    (temp_buffer[0] == 0x81) &&
    (temp_buffer[1] == 0x00) &&
    (temp_buffer[2] == 0x00))
    {
        uint8_t cfg_value = temp_buffer[3];
        eeprom_write(EEP_ADDR_BAUD, cfg_value);
        eeprom_write(EEP_ADDR_BAUD + 1, ~cfg_value);
        read_eeprom();
        can_config();
        temp_buffer[3] = ~can_mode;
        temp_buffer[len - 1] = calc_checkum(temp_buffer, len - 1);
        uart_send(temp_buffer, len);
        return true;
    }

    if ((len == 6) &&
    (temp_buffer[0] == 0x82) &&
    (temp_buffer[1] == 0xF1) &&
    (temp_buffer[2] == 0xF1))
    {
        if ((temp_buffer[3] & 0x7F) == 0x00)
        {      // block size
            if ((temp_buffer[3] & 0x80) == 0x00)
            {   // write
                uint8_t cfg_value = temp_buffer[4];
                eeprom_write(EEP_ADDR_BLOCKSIZE, cfg_value);
                eeprom_write(EEP_ADDR_BLOCKSIZE + 1, ~cfg_value);
                read_eeprom();
            }
            temp_buffer[4] = can_blocksize;
            temp_buffer[len - 1] = calc_checkum(temp_buffer, len - 1);
            uart_send(temp_buffer, len);
            return true;
        }
        if ((temp_buffer[3] & 0x7F) == 0x01)
        {      // separation time
            if ((temp_buffer[3] & 0x80) == 0x00)
            {   // write
                uint8_t cfg_value = temp_buffer[4];
                eeprom_write(EEP_ADDR_SEP_TIME, cfg_value);
                eeprom_write(EEP_ADDR_SEP_TIME + 1, ~cfg_value);
                read_eeprom();
            }
            temp_buffer[4] = can_sep_time;
            temp_buffer[len - 1] = calc_checkum(temp_buffer, len - 1);
            uart_send(temp_buffer, len);
            return true;
        }
        if ((temp_buffer[3] & 0x7F) == 0x02)
        {      // can mode
            if ((temp_buffer[3] & 0x80) == 0x00)
            {   // write
                uint8_t cfg_value = temp_buffer[4];
                eeprom_write(EEP_ADDR_BAUD, cfg_value);
                eeprom_write(EEP_ADDR_BAUD + 1, ~cfg_value);
                read_eeprom();
                can_config();
            }
            temp_buffer[4] = can_mode;
            temp_buffer[len - 1] = calc_checkum(temp_buffer, len - 1);
            uart_send(temp_buffer, len);
            return true;
        }
        if ((temp_buffer[3] == 0xFD) && (temp_buffer[4] == 0xFD))
        {      // read adapter type
            temp_buffer[4] = 0x02;
            temp_buffer[len - 1] = calc_checkum(temp_buffer, len - 1);
            uart_send(temp_buffer, len);
            return true;
        }
#if 0
        if ((temp_buffer[3] == 0xFE) && (temp_buffer[4] == 0xFE))
        {      // read ignition state
            temp_buffer[4] = IGNITION_STATE() ? 0x01 : 0x00;
            temp_buffer[4] |= (can_mode != 0) ? 0x80 : 0x00;
            temp_buffer[len - 1] = calc_checkum(temp_buffer, len - 1);
            uart_send(temp_buffer, len);
            return true;
        }
#endif
        if ((temp_buffer[3] == 0xFF) && (temp_buffer[4] == 0xFF))
        {      // reset command
            RESET();
            return true;
        }
        return true;
    }
    return false;
}

void main(void)
{
    time_tick_10 = 0;

    // LED on
    LED_RS_RX = 0;
    LED_RS_TX = 0;
    LED_OBD_RX = 0;
    LED_OBD_TX = 0;
    // LED as output
    TRISBbits.TRISB4 = 0;
    TRISBbits.TRISB5 = 0;
    TRISBbits.TRISB6 = 0;
    TRISBbits.TRISB7 = 0;

    TRISBbits.TRISB3 = 1;   // CAN RX input
    //TRISBbits.TRISB2 = 0;   // CAN TX output (set automatically)

    RCONbits.IPEN = 1;      // interrupt priority enable

    TRISCbits.TRISC6 = 0;   // TX output
    TRISCbits.TRISC7 = 1;   // RX input
    SPBRG1 = 103;           // 38400 @ 16MHz
    TXSTA1bits.TXEN = 1;    // Enable transmit
    TXSTA1bits.BRGH = 1;    // Select high baud rate
    TXSTA1bits.SYNC = 0;    // async mode
    BAUDCON1bits.BRG16 = 1; // 16 bit counter
    RCSTA1bits.CREN = 1;    // Enable continuous reception

    IPR1bits.RCIP = 1;      // UART interrupt high priority
    PIR1bits.RCIF = 0;      // Clear RCIF Interrupt Flag
    PIE1bits.RCIE = 1;      // Set RCIE Interrupt Enable
    PIR1bits.TXIF = 0;      // Clear TXIF Interrupt Flag
    PIE1bits.TXIE = 0;      // Set TXIE Interrupt disable
    RCSTA1bits.SPEN = 1;    // Enable Serial Port

    // timer 0
    T0CONbits.T08BIT = 1;   // 8 bit mode
    T0CONbits.T0CS = 0;     // clock internal
    T0CONbits.T0PS = 7;     // prescaler 256 = 15625Hz
    T0CONbits.PSA = 0;      // prescaler enabled
    TMR0L = TIMER0_RELOAD;

    INTCON2bits.T0IP = 0;   // low priority
    INTCONbits.TMR0IF = 0;  // clear timer 0 interrupt flag
    INTCONbits.TMR0IE = 1;  // enable timer 0 interrupt
    T0CONbits.TMR0ON = 1;   // enable timer 0

    // timer 1
    T1CONbits.TMR1ON = 0;   // stop timer
    T1CONbits.RD16 = 1;     // 16 bit access
    T1CONbits.TMR1CS = 0;   // internal clock 4MHz
    T1CONbits.SOSCEN = 0;   // oscillator disabled
    T1CONbits.T1CKPS = 3;   // prescaler 8 = 500kHz

    IPR1bits.TMR1IP = 1;    // timer 1 high prioriy
    PIR1bits.TMR1IF = 0;    // clear timer 1 interrupt flag
    PIE1bits.TMR1IE = 1;    // enable timer 1 interrupt

    // timer 2
    T2CONbits.T2CKPS = 0;   // prescaler 1
    T2CONbits.T2OUTPS = 0xF; // postscaler 16
    // fout = fclk / (4 * prescaler * PR2 * postscaler)
    // PR2 = fclk / (4 * prescaler * fout * postscaler)
    // PR2 = 16000000 / (4 * 1 * 1000 * 16) = 250
    TMR2 = 0x00;            // timer 2 start value
    PR2 = 250;              // timer 2 stop value

    IPR1bits.TMR2IP = 0;    // timer 2 low prioriy
    PIR1bits.TMR2IF = 0;    // clear timer 2 interrupt flag
    PIE1bits.TMR2IE = 1;    // enable timer 2 interrupt
    //T2CONbits.TMR2ON = 1;   // enable timer 2

    INTCONbits.GIEL = 1;    // enable low priority interrupts
    INTCONbits.GIEH = 1;    // enable high priority interrupts

    read_eeprom();
    can_config();

    WDTCONbits.SWDTEN = 1;  // enable watchdog
    for (;;)
    {
        CLRWDT();
        uint16_t len = uart_receive(temp_buffer);
        if (len > 0)
        {
            internal_telegram(len);
        }

        if (time_tick_10 & 0x10) LED_OBD_RX = 1;
        else LED_OBD_RX = 0;

        if (time_tick_10 & 0x20) LED_OBD_TX = 1;
        else LED_OBD_TX = 0;

        if (can_enabled)
        {
            if (readCAN())
            {
                static uint8_t test = 0;
                memcpy(&can_out_msg, &can_in_msg, sizeof(can_out_msg));
                can_out_msg.data[0] = test++;
                writeCAN();
            }
        }
    }
}

// vector 0x0018
void interrupt low_priority low_isr (void)
{
    if (INTCONbits.TMR0IF)
    {
        TMR0L = TIMER0_RELOAD - TMR0L;
        INTCONbits.TMR0IF = 0;
        time_tick_10++;
        return;
    }
    if (PIR1bits.TMR2IF)
    {
        PIR1bits.TMR2IF = 0;
        return;
    }
}

// vector 0x0008
void interrupt high_priority high_isr (void)
{
    if (PIE1bits.RCIE && PIR1bits.RCIF)
    {   // receive interrupt
        if (RCSTA1 & 0x06)
        {   // receive error -> reset flags
            RCSTA1bits.CREN = 0;
            RCSTA1bits.CREN = 1;
        }
        else
        {
            uint8_t rec_data = RCREG1;
            // restart timeout timer
            TMR1H = TIMER1_RELOAD >> 8;
            TMR1L = TIMER1_RELOAD;
            PIR1bits.TMR1IF = 0;    // clear interrupt flag
            T1CONbits.TMR1ON = 1;   // start timeout timer

            switch (rec_state)
            {
                case rec_state_idle:
                    rec_len = 0;
                    rec_buffer[rec_len++] = rec_data;
                    rec_chksum = rec_data;
                    rec_state = rec_state_rec;
                    break;

                case rec_state_rec:
                    if (rec_len < sizeof(rec_buffer))
                    {
                        rec_buffer[rec_len++] = rec_data;
                    }
                    if (rec_len >= 4)
                    {   // header received
                        uint16_t tel_len = rec_buffer[0] & 0x3F;
                        if (tel_len == 0)
                        {
                            tel_len = rec_buffer[3] + 5;
                        }
                        else
                        {
                            tel_len += 4;
                        }
                        if (rec_len >= tel_len)
                        {   // complete tel received
                            if (rec_chksum != rec_data)
                            {   // checksum error
                                rec_state = rec_state_error;
                                break;
                            }
                            T1CONbits.TMR1ON = 0;   // stop timer
                            PIR1bits.TMR1IF = 0;
                            rec_state = rec_state_done;
                            break;
                        }
                    }
                    rec_chksum += rec_data;
                    break;

                default:
                    break;
            }
        }
        return;
    }
    if (PIE1bits.TXIE && PIR1bits.TXIF)
    {
        if (send_len == 0)
        {
            PIE1bits.TXIE = 0;    // disable TX interrupt
        }
        else
        {
            TXREG1 = send_buffer[send_get_idx++];
            send_len--;
            if (send_get_idx >= sizeof(send_buffer))
            {
                send_get_idx = 0;
            }
        }
        return;
    }
    if (PIE1bits.TMR1IE && PIR1bits.TMR1IF)
    {   // timeout timer
        T1CONbits.TMR1ON = 0;   // stop timer
        PIR1bits.TMR1IF = 0;
        switch (rec_state)
        {
            case rec_state_rec:
            case rec_state_error:
                // receive timeout
                rec_state = rec_state_idle;
                break;

            default:
                break;
        }
        return;
    }
}
