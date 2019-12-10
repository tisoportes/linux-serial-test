// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <linux/serial.h>
#include <errno.h>

/* 
 * glibc for MIPS has its own bits/termios.h which does not define
 * CMSPAR, so we vampirise the value from the generic bits/termios.h
 */
#ifndef CMSPAR
#define CMSPAR 010000000000
#endif

// command line args
int _cl_baud = 0;
char *_cl_port = NULL;
int _cl_divisor = 0;
int _cl_rx_dump = 0;
int _cl_rx_dump_ascii = 0;
int _cl_tx_detailed = 0;
int _cl_stats = 0;
int _cl_stop_on_error = 0;
int _cl_single_byte = -1;
int _cl_another_byte = -1;
int _cl_rts_cts = 0;
int _cl_2_stop_bit = 0;
int _cl_parity = 0;
int _cl_odd_parity = 0;
int _cl_stick_parity = 0;
int _cl_dump_err = 0;
int _cl_no_rx = 0;
int _cl_no_tx = 0;
int _cl_rx_delay = 0;
int _cl_tx_delay = 0;
int _cl_tx_bytes = 0;
int _cl_rs485_delay = -1;
int _cl_rs485_rts_after_send = 0;
int _cl_tx_time = 0;
int _cl_rx_time = 0;
int _cl_ascii_range = 0;

// Module variables
unsigned char _write_count_value = 0;
unsigned char _read_count_value = 0;
int _fd = -1;
unsigned char * _write_data;
ssize_t _write_size;

// keep our own counts for cases where the driver stats don't work
long long int _write_count = 0;
long long int _read_count = 0;
long long int _error_count = 0;

static void dump_data(unsigned char * b, int count)
{
	printf("%i bytes: ", count);
	int i;
	for (i=0; i < count; i++) {
		printf("%02x ", b[i]);
	}

	printf("\n");
}

static void dump_data_ascii(unsigned char * b, int count)
{
	int i;
	for (i=0; i < count; i++) {
		printf("%c", b[i]);
	}
}

static void set_baud_divisor(int speed)
{
	// default baud was not found, so try to set a custom divisor
	struct serial_struct ss;
	if (ioctl(_fd, TIOCGSERIAL, &ss) < 0) {
		perror("TIOCGSERIAL failed");
		exit(1);
	}

	ss.flags = (ss.flags & ~ASYNC_SPD_MASK) | ASYNC_SPD_CUST;
	ss.custom_divisor = (ss.baud_base + (speed/2)) / speed;
	int closest_speed = ss.baud_base / ss.custom_divisor;

	if (closest_speed < speed * 98 / 100 || closest_speed > speed * 102 / 100) {
		fprintf(stderr, "Cannot set speed to %d, closest is %d\n", speed, closest_speed);
		exit(1);
	}

	printf("closest baud = %i, base = %i, divisor = %i\n", closest_speed, ss.baud_base,
			ss.custom_divisor);

	if (ioctl(_fd, TIOCSSERIAL, &ss) < 0) {
		perror("TIOCSSERIAL failed");
		exit(1);
	}
}

// converts integer baud to Linux define
static int get_baud(int baud)
{
	switch (baud) {
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	case 460800:
		return B460800;
	case 500000:
		return B500000;
	case 576000:
		return B576000;
	case 921600:
		return B921600;
#ifdef B1000000
	case 1000000:
		return B1000000;
#endif
#ifdef B1152000
	case 1152000:
		return B1152000;
#endif
#ifdef B1500000
	case 1500000:
		return B1500000;
#endif
#ifdef B2000000
	case 2000000:
		return B2000000;
#endif
#ifdef B2500000
	case 2500000:
		return B2500000;
#endif
#ifdef B3000000
	case 3000000:
		return B3000000;
#endif
#ifdef B3500000
	case 3500000:
		return B3500000;
#endif
#ifdef B4000000
	case 4000000:
		return B4000000;
#endif
	default: 
		return -1;
	}
}

static void display_help(void)
{
	printf("Usage: linux-serial-test [OPTION]\n"
			"\n"
			"  -h, --help\n"
			"  -b, --baud        Baud rate, 115200, etc (115200 is default)\n"
			"  -p, --port        Port (/dev/ttyS0, etc) (must be specified)\n"
			"  -d, --divisor     UART Baud rate divisor (can be used to set custom baud rates)\n"
			"  -R, --rx_dump     Dump Rx data (ascii, raw)\n"
			"  -T, --detailed_tx Detailed Tx data\n"
			"  -s, --stats       Dump serial port stats every 5s\n"
			"  -S, --stop-on-err Stop program if we encounter an error\n"
			"  -y, --single-byte Send specified byte to the serial port\n"
			"  -z, --second-byte Send another specified byte to the serial port\n"
			"  -c, --rts-cts     Enable RTS/CTS flow control\n"
			"  -B, --2-stop-bit  Use two stop bits per character\n"
			"  -P, --parity      Use parity bit (odd, even, mark, space)\n"
			"  -e, --dump-err    Display errors\n"
			"  -r, --no-rx       Don't receive data (can be used to test flow control)\n"
			"                    when serial driver buffer is full\n"
			"  -t, --no-tx       Don't transmit data\n"
			"  -l, --rx-delay    Delay between reading data (ms) (can be used to test flow control)\n"
			"  -a, --tx-delay    Delay between writing data (ms)\n"
			"  -w, --tx-bytes    Number of bytes for each write (default is to repeatedly write 1024 bytes\n"
			"                    until no more are accepted)\n"
			"  -q, --rs485       Enable RS485 direction control on port, and set delay\n"
			"                    from when TX is finished and RS485 driver enable is\n"
			"                    de-asserted. Delay is specified in bit times.\n"
			"  -Q, --rs485_rts   Deassert RTS on send, assert after send. Omitting -Q inverts this logic.\n"
			"  -o, --tx-time     Number of seconds to transmit for (defaults to 0, meaning no limit)\n"
			"  -i, --rx-time     Number of seconds to receive for (defaults to 0, meaning no limit)\n"
			"  -A, --ascii       Output bytes range from 32 to 126 (default is 0 to 255)\n"
			"\n"
	      );
}

static void process_options(int argc, char * argv[])
{
	for (;;) {
		int option_index = 0;
		static const char *short_options = "hb:p:d:R:TsSy:z:cBertq:Ql:a:w:o:i:P:A";
		static const struct option long_options[] = {
			{"help", no_argument, 0, 0},
			{"baud", required_argument, 0, 'b'},
			{"port", required_argument, 0, 'p'},
			{"divisor", required_argument, 0, 'd'},
			{"rx_dump", required_argument, 0, 'R'},
			{"detailed_tx", no_argument, 0, 'T'},
			{"stats", no_argument, 0, 's'},
			{"stop-on-err", no_argument, 0, 'S'},
			{"single-byte", no_argument, 0, 'y'},
			{"second-byte", no_argument, 0, 'z'},
			{"rts-cts", no_argument, 0, 'c'},
			{"2-stop-bit", no_argument, 0, 'B'},
			{"parity", required_argument, 0, 'P'},
			{"dump-err", no_argument, 0, 'e'},
			{"no-rx", no_argument, 0, 'r'},
			{"no-tx", no_argument, 0, 't'},
			{"rx-delay", required_argument, 0, 'l'},
			{"tx-delay", required_argument, 0, 'a'},
			{"tx-bytes", required_argument, 0, 'w'},
			{"rs485", required_argument, 0, 'q'},
			{"rs485_rts", no_argument, 0, 'Q'},
			{"tx-time", required_argument, 0, 'o'},
			{"rx-time", required_argument, 0, 'i'},
			{"ascii", no_argument, 0, 'A'},
			{0,0,0,0},
		};

		int c = getopt_long(argc, argv, short_options,
				long_options, &option_index);

		if (c == EOF) {
			break;
		}

		switch (c) {
		case 0:
		case 'h':
			display_help();
			exit(0);
			break;
		case 'b':
			_cl_baud = atoi(optarg);
			break;
		case 'p':
			_cl_port = strdup(optarg);
			break;
		case 'd':
			_cl_divisor = atoi(optarg);
			break;
		case 'R':
			_cl_rx_dump = 1;
			_cl_rx_dump_ascii = !strcmp(optarg, "ascii");
			break;
		case 'T':
			_cl_tx_detailed = 1;
			break;
		case 's':
			_cl_stats = 1;
			break;
		case 'S':
			_cl_stop_on_error = 1;
			break;
		case 'y': {
			char * endptr;
			_cl_single_byte = strtol(optarg, &endptr, 0);
			break;
		}
		case 'z': {
			char * endptr;
			_cl_another_byte = strtol(optarg, &endptr, 0);
			break;
		}
		case 'c':
			_cl_rts_cts = 1;
			break;
		case 'B':
			_cl_2_stop_bit = 1;
			break;
		case 'P':
			_cl_parity = 1;
			_cl_odd_parity = (!strcmp(optarg, "mark")||!strcmp(optarg, "odd"));
			_cl_stick_parity = (!strcmp(optarg, "mark")||!strcmp(optarg, "space"));
			break;
		case 'e':
			_cl_dump_err = 1;
			break;
		case 'r':
			_cl_no_rx = 1;
			break;
		case 't':
			_cl_no_tx = 1;
			break;
		case 'l': {
			char *endptr;
			_cl_rx_delay = strtol(optarg, &endptr, 0);
			break;
		}
		case 'a': {
			char *endptr;
			_cl_tx_delay = strtol(optarg, &endptr, 0);
			break;
		}
		case 'w': {
			char *endptr;
			_cl_tx_bytes = strtol(optarg, &endptr, 0);
			break;
		}
		case 'q': {
			char *endptr;
			_cl_rs485_delay = strtol(optarg, &endptr, 0);
			break;
		}
		case 'Q':
			_cl_rs485_rts_after_send = 1;
			break;
		case 'o': {
			char *endptr;
			_cl_tx_time = strtol(optarg, &endptr, 0);
			break;
		}
		case 'i': {
			char *endptr;
			_cl_rx_time = strtol(optarg, &endptr, 0);
			break;
		}
		case 'A':
			_cl_ascii_range = 1;
			break;
		}
	}
}

static void dump_serial_port_stats(void)
{
	struct serial_icounter_struct icount = { 0 };

	printf("%s: count for this session: rx=%lld, tx=%lld, rx err=%lld\n", _cl_port, _read_count, _write_count, _error_count);

	int ret = ioctl(_fd, TIOCGICOUNT, &icount);
	if (ret != -1) {
		printf("%s: TIOCGICOUNT: ret=%i, rx=%i, tx=%i, frame = %i, overrun = %i, parity = %i, brk = %i, buf_overrun = %i\n",
				_cl_port, ret, icount.rx, icount.tx, icount.frame, icount.overrun, icount.parity, icount.brk,
				icount.buf_overrun);
	}
}

static unsigned char next_count_value(unsigned char c)
{
	c++;
	if (_cl_ascii_range && c == 127)
		c = 32;
	return c;
}

static void process_read_data(void)
{
	unsigned char rb[1024];
	int c = read(_fd, &rb, sizeof(rb));
	if (c > 0) {
		if (_cl_rx_dump) {
			if (_cl_rx_dump_ascii)
				dump_data_ascii(rb, c);
			else
				dump_data(rb, c);
		}

		// verify read count is incrementing
		int i;
		for (i = 0; i < c; i++) {
			if (rb[i] != _read_count_value) {
				if (_cl_dump_err) {
					printf("Error, count: %lld, expected %02x, got %02x\n",
							_read_count + i, _read_count_value, rb[i]);
				}
				_error_count++;
				if (_cl_stop_on_error) {
					dump_serial_port_stats();
					exit(1);
				}
				_read_count_value = rb[i];
			}
			_read_count_value = next_count_value(_read_count_value);
		}
		_read_count += c;
	}
}

static void process_write_data(void)
{
	ssize_t count = 0;
	int repeat = (_cl_tx_bytes == 0);

	do
	{
		ssize_t i;
		for (i = 0; i < _write_size; i++) {
			_write_data[i] = _write_count_value;
			_write_count_value = next_count_value(_write_count_value);
		}

		ssize_t c = write(_fd, _write_data, _write_size);

		if (c < 0) {
			if (errno != EAGAIN) {
				printf("write failed - errno=%d (%s)\n", errno, strerror(errno));
			}
			c = 0;
		}

		count += c;

		if (c < _write_size) {
			_write_count_value = _write_data[c];
			repeat = 0;
		}
	} while (repeat);

	_write_count += count;

	if (_cl_tx_detailed)
		printf("wrote %zd bytes\n", count);
}


static void setup_serial_port(int baud)
{
	struct termios newtio;
	struct serial_rs485 rs485;

	_fd = open(_cl_port, O_RDWR | O_NONBLOCK);

	if (_fd < 0) {
		perror("Error opening serial port");
		free(_cl_port);
		exit(1);
	}

	bzero(&newtio, sizeof(newtio)); /* clear struct for new port settings */

	/* man termios get more info on below settings */
	newtio.c_cflag = baud | CS8 | CLOCAL | CREAD;

	if (_cl_rts_cts) {
		newtio.c_cflag |= CRTSCTS;
	}

	if (_cl_2_stop_bit) {
		newtio.c_cflag |= CSTOPB;
	}

	if (_cl_parity) {
		newtio.c_cflag |= PARENB;
		if (_cl_odd_parity) {
			newtio.c_cflag |= PARODD;
		}
		if (_cl_stick_parity) {
			newtio.c_cflag |= CMSPAR;
		}
	}

	newtio.c_iflag = 0;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;

	// block for up till 128 characters
	newtio.c_cc[VMIN] = 128;

	// 0.5 seconds read timeout
	newtio.c_cc[VTIME] = 5;

	/* now clean the modem line and activate the settings for the port */
	tcflush(_fd, TCIOFLUSH);
	tcsetattr(_fd,TCSANOW,&newtio);

	/* enable/disable rs485 direction control */
	if(ioctl(_fd, TIOCGRS485, &rs485) < 0) {
		if (_cl_rs485_delay >= 0) {
			/* error could be because hardware is missing rs485 support so only print when actually trying to activate it */
			perror("Error getting RS-485 mode");
		}
	} else if (_cl_rs485_delay >= 0) {
		rs485.flags |= SER_RS485_ENABLED | SER_RS485_RX_DURING_TX |
			(_cl_rs485_rts_after_send ? SER_RS485_RTS_AFTER_SEND : SER_RS485_RTS_ON_SEND);
		rs485.flags &= ~(_cl_rs485_rts_after_send ? SER_RS485_RTS_ON_SEND : SER_RS485_RTS_AFTER_SEND);
		rs485.delay_rts_after_send = _cl_rs485_delay;
		rs485.delay_rts_before_send = 0;
		if(ioctl(_fd, TIOCSRS485, &rs485) < 0) {
			perror("Error setting RS-485 mode");
		}
	} else {
		rs485.flags &= ~(SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND);
		rs485.delay_rts_after_send = 0;
		rs485.delay_rts_before_send = 0;
		if(ioctl(_fd, TIOCSRS485, &rs485) < 0) {
			perror("Error setting RS-232 mode");
		}
	}
}

static int diff_ms(const struct timespec *t1, const struct timespec *t2)
{
	struct timespec diff;

	diff.tv_sec = t1->tv_sec - t2->tv_sec;
	diff.tv_nsec = t1->tv_nsec - t2->tv_nsec;
	if (diff.tv_nsec < 0) {
		diff.tv_sec--;
		diff.tv_nsec += 1000000000;
	}
	return (diff.tv_sec * 1000 + diff.tv_nsec/1000000);
}

int main(int argc, char * argv[])
{
	printf("Linux serial test app\n");

	process_options(argc, argv);

	if (!_cl_port) {
		fprintf(stderr, "ERROR: Port argument required\n");
		display_help();
		return 1;
	}

	int baud = B115200;

	if (_cl_baud)
		baud = get_baud(_cl_baud);

	if (baud <= 0) {
		printf("NOTE: non standard baud rate, trying custom divisor\n");
		baud = B38400;
		setup_serial_port(B38400);
		set_baud_divisor(_cl_baud);
	} else {
		setup_serial_port(baud);
	}

	if (_cl_single_byte >= 0) {
		unsigned char data[2];
		int bytes = 1;
		int written;
		data[0] = (unsigned char)_cl_single_byte;
		if (_cl_another_byte >= 0) {
			data[1] = (unsigned char)_cl_another_byte;
			bytes++;
		}
		written = write(_fd, &data, bytes);
		if (written < 0) {
			perror("write()");
			return 1;
		} else if (written != bytes) {
			fprintf(stderr, "ERROR: write() returned %d, not %d\n", written, bytes);
			return 1;
		}
		return 0;
	}

	_write_size = (_cl_tx_bytes == 0) ? 1024 : _cl_tx_bytes;

	_write_data = malloc(_write_size);
	if (_write_data == NULL) {
		fprintf(stderr, "ERROR: Memory allocation failed\n");
		return 1;
	}

	if (_cl_ascii_range) {
		_read_count_value = _write_count_value = 32;
	}

	struct pollfd serial_poll;
	serial_poll.fd = _fd;
	if (!_cl_no_rx) {
		serial_poll.events |= POLLIN;
	} else {
		serial_poll.events &= ~POLLIN;
	}

	if (!_cl_no_tx) {
		serial_poll.events |= POLLOUT;
	} else {
		serial_poll.events &= ~POLLOUT;
	}

	struct timespec start_time, last_stat, last_timeout, last_read, last_write;

	clock_gettime(CLOCK_MONOTONIC, &start_time);
	last_stat = start_time;
	last_timeout = start_time;
	last_read = start_time;
	last_write = start_time;

	while (!(_cl_no_rx && _cl_no_tx)) {
		struct timespec current;
		int retval = poll(&serial_poll, 1, 1000);

		clock_gettime(CLOCK_MONOTONIC, &current);

		if (retval == -1) {
			perror("poll()");
		} else if (retval) {
			if (serial_poll.revents & POLLIN) {
				if (_cl_rx_delay) {
					// only read if it has been rx-delay ms
					// since the last read
					if (diff_ms(&current, &last_read) > _cl_rx_delay) {
						process_read_data();
						last_read = current;
					}
				} else {
					process_read_data();
					last_read = current;
				}
			}

			if (serial_poll.revents & POLLOUT) {
				if (_cl_tx_delay) {
					// only write if it has been tx-delay ms
					// since the last write
					if (diff_ms(&current, &last_write) > _cl_tx_delay) {
						process_write_data();
						last_write = current;
					}
				} else {
					process_write_data();
					last_write = current;
				}
			}
		}

		// Has it been at least a second since we reported a timeout?
		if (diff_ms(&current, &last_timeout) > 1000) {
			int rx_timeout, tx_timeout;

			// Has it been over two seconds since we transmitted or received data?
			rx_timeout = (!_cl_no_rx && diff_ms(&current, &last_read) > 2000);
			tx_timeout = (!_cl_no_tx && diff_ms(&current, &last_write) > 2000);
			// Special case - we don't want to warn about receive
			// timeouts at the end of a loopback test (where we are
			// no longer transmitting and the receive count equals
			// the transmit count).
			if (_cl_no_tx && _write_count != 0 && _write_count == _read_count) {
				rx_timeout = 0;
			}

			if (rx_timeout || tx_timeout) {
				const char *s;
				if (rx_timeout) {
					printf("No data received for %.1fs.",
					       (double)diff_ms(&current, &last_read) / 1000);
					s = " ";
				} else {
					s = "";
				}
				if (tx_timeout) {
					printf("%sNo data transmitted for %.1fs.",
					       s, (double)diff_ms(&current, &last_write) / 1000);
				}
				printf("\n");
				last_timeout = current;
			}
		}

		if (_cl_stats) {
			if (current.tv_sec - last_stat.tv_sec > 5) {
				dump_serial_port_stats();
				last_stat = current;
			}
		}

		if (_cl_tx_time) {
			if (current.tv_sec - start_time.tv_sec >= _cl_tx_time) {
				_cl_tx_time = 0;
				_cl_no_tx = 1;
				serial_poll.events &= ~POLLOUT;
				printf("Stopped transmitting.\n");
			}
		}

		if (_cl_rx_time) {
			if (current.tv_sec - start_time.tv_sec >= _cl_rx_time) {
				_cl_rx_time = 0;
				_cl_no_rx = 1;
				serial_poll.events &= ~POLLIN;
				printf("Stopped receiving.\n");
			}
		}
	}

	tcdrain(_fd);
	dump_serial_port_stats();
	tcflush(_fd, TCIOFLUSH);
	free(_cl_port);

	long long int result = llabs(_write_count - _read_count) + _error_count;

	return (result > 125) ? 125 : (int)result;
}
