/**
 * avr uart stdio setup
 * mostly copied from https://appelsiini.net/2011/simple-usart-with-avr-libc/
 * with a small sprinkling here and there.
 */

#include <avr/io.h>
#include <stdio.h>
#include <util/setbaud.h>

#ifndef F_CPU
#   error No CPU frequency supplied; #define F_CPU or use -DF_CPU=x
#endif

#ifndef BAUD
#   error Baud rate not specified; #define BAUD or use -DBAUD=x
#endif

int uart_putchar(char c, FILE *stream)
{
    if (c == '\n') {
        uart_putchar('\r', stream);
    }
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = c;
    return (int) c;
}

int uart_getchar(FILE *stream)
{
    loop_until_bit_is_set(UCSR0A, RXC0); /* Wait until data exists. */
    return UDR0;
}

FILE uart_output = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);
FILE uart_input = FDEV_SETUP_STREAM(NULL, uart_getchar, _FDEV_SETUP_READ);
FILE uart_io = FDEV_SETUP_STREAM(uart_putchar, uart_getchar, _FDEV_SETUP_RW);

void uart_init(void)
{
    UBRR0H = UBRRH_VALUE;
    UBRR0L = UBRRL_VALUE;

#if USE_2X
    UCSR0A |= _BV(U2X0);
#else
    UCSR0A &= ~(_BV(U2X0));
#endif

    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00); /* 8-bit data */
    UCSR0B = _BV(RXEN0) | _BV(TXEN0);   /* Enable RX and TX */

    stdout = &uart_output;
    stdin = &uart_input;
}
