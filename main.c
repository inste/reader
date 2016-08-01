#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#define BUFFER			2048
#define SMALL			32
#define ITEMS_COUNT		25
#define BAUDRATE		B9600

#define OUTDATE_TIMEOUT		10	// Seconds
#define MICROSLEEP		20000 // 20 us

#define LOG_PORT		4999

#define DEVICE			"/dev/ttyUSB0"

#define __DEBUG__


struct readings {
	char	key[SMALL];
	char	value[SMALL];
};

struct readings	global_data[ITEMS_COUNT];
time_t		last_update = 0;
time_t		start_time;


int dev_exists(char * filename) {
	struct stat	buffer;
	return ((stat (filename, &buffer) == 0) && S_ISCHR(buffer.st_mode));
}

int open_device(char * device) {
	struct termios tio;
	int tty_fd;

	if (!dev_exists(device)) {
		return -ENXIO;
	}

	memset(&tio, 0, sizeof(tio));
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_cflag = CS8 | CREAD | CLOCAL;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 5;

	if ((tty_fd = open(device, O_RDWR)) == -1) {
		return -1;
	}

	cfsetospeed(&tio, BAUDRATE);
	cfsetispeed(&tio, BAUDRATE);
	tcsetattr(tty_fd, TCSANOW, &tio);

	return tty_fd;
}

void write_log(int log_fd, int log_sock_fd, char * log) {
	size_t size;
	char buffer[BUFFER];

	size = snprintf(buffer, BUFFER, "[%u] %s\r\n", (unsigned)time(NULL), log);
	if (log_sock_fd > 0)
		write(log_sock_fd, buffer, size);

	/* if (log_fd > -1) {
		write(log_fd, buffer, size);
		fdatasync(log_fd);
	} */
}

int process_recv(int log_fd, int log_sock_fd, char * recv) {
	int retval = -1;
	size_t pos = ITEMS_COUNT;
	char logtmp[BUFFER];

	char * p = strstr(recv, "R=");

	if (NULL != p) {
		char * ke = strchr(recv + 2, ':');

		if (NULL != ke) {
			char temp[SMALL];
			char * s = p + 2;
			size_t i = 0;

			memset(temp, 0, SMALL);

			while (s != ke) {
				temp[i++] = *s++;
			}

			for (i = 0; i < ITEMS_COUNT; ++i) {
				if (!strcmp(temp, global_data[i].key) ) {
					pos = i;

					break;
				}
			}

			if( ITEMS_COUNT == pos ) {
				for (i = 0; i < ITEMS_COUNT; ++i) {
					if (global_data[i].key[0] == '\0') {
						pos = i;
						strcpy(global_data[i].key, temp);

						break;
					}
				}
			}

			if (pos < ITEMS_COUNT) {
				char * pp = strstr(ke, "T=");

				if (NULL != pp) {
					char * de = strchr(pp + 2, ';');
					char * ss = pp + 2;
					char tempdata[SMALL];
					size_t j = 0;

					if (NULL != de) {
						memset(tempdata, 0, SMALL);

						while (ss != de) {
							tempdata[j++] = *ss++;
						}

						strcpy(global_data[pos].value, tempdata);

						retval = 0;

						snprintf(logtmp, BUFFER, "Updated R=[%s], T=%s *C",
							global_data[pos].key, global_data[pos].value);
						write_log(log_fd, log_sock_fd, logtmp);

					} else {
						snprintf(logtmp, BUFFER, "Unable to find data end delimiter");
						write_log(log_fd, log_sock_fd, logtmp);
					}
				} else {
					snprintf(logtmp, BUFFER, "Unable to find data sequence");
					write_log(log_fd, log_sock_fd, logtmp);
				}
			} else {
				snprintf(logtmp, BUFFER, "No empty sensors slots");
				write_log(log_fd, log_sock_fd, logtmp);
			}
		} else {
			snprintf(logtmp, BUFFER, "Unable to file key delimiter");
			write_log(log_fd, log_sock_fd, logtmp);
		}
	} else {
		snprintf(logtmp, BUFFER, "Unable to find start sequence");
		write_log(log_fd, log_sock_fd, logtmp);
	}

	last_update = time(NULL);

	return retval;
}

int get_max(int tty_fd, int listenfd, int ll_fd, int log_fd, int * sock_fd, int sock_fd_count) {
	int i;
	int max = 0;

	for (i = 0; i < sock_fd_count; ++i) {
		if (sock_fd[i] > max)
			max = sock_fd[i];
	}

	max = (ll_fd > max) ? ll_fd : max;
	max = (log_fd > max) ? log_fd : max;
	max = (tty_fd > max) ? tty_fd : max;
	return (listenfd > max) ? listenfd : max;
}

int open_socket(int port) {
	int sock_fd;
	struct sockaddr_in serv_addr;

	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&serv_addr, 0, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	bind(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	return sock_fd;
}


#define ZBXPADOFFSET	13
#define ZBXLENPAD		5
#define ZBXD		"ZBXD\x01"

void process_zabbix(char * request, int sock_fd) {
	int padlen;

	char * buffer = (char *)malloc(sizeof(char) * BUFFER);
	char * data = buffer + ZBXPADOFFSET;

	memset(buffer, 0, sizeof(char) * BUFFER);

	if (!strcmp("agent.hostname", request)) {
		strcpy(data, "Temperature sensors aggregator");
		goto do_write;
	}

	if (!strcmp("agent.uptime", request)) {
		snprintf(data, SMALL, "%u", (unsigned int)(time(NULL) - start_time));
		goto do_write;
	}

	if (!strcmp("agent.update", request)) {
		snprintf(data, SMALL, "%u", (unsigned int)(last_update));
		goto do_write;
	}

	if (!strncmp("sensor", request, 6)) {
		char * apos, * rpos;

		apos = strchr(request + 6, '[');
		rpos = strchr(request + 6, ']');

		if (apos && rpos && rpos > apos && (rpos - apos) < SMALL) {
			size_t i = 0;

			*rpos = '\0';
			apos++;

			for (i = 0; i < ITEMS_COUNT; ++i) {
				if( !global_data[i].key[0] == '\0' &&
					!strcmp(apos, global_data[i].key) ) {

					snprintf(data, SMALL, "%s", global_data[i].value);
					goto do_write;
				}
			}

			snprintf(data, SMALL, "-100");
		}
	}


/*	Push data to be written in 'data' variable (zero-trailing, and no more than 255 chars) and call to do_write label */
do_write:
	padlen = strlen(buffer + ZBXPADOFFSET);
	strcpy(buffer, ZBXD);
	*((unsigned char *)(buffer + ZBXLENPAD)) = padlen & 0xFF;
	write(sock_fd, buffer, ZBXPADOFFSET + padlen);

	free(buffer);
	return;
}


int event_loop(char * device_file, int port, int log_fd) {
	int tty_fd, is_dev_opened;
	char data[BUFFER], device_data[BUFFER];
	int count, data_count;
	int i, j;

	int listen_fd;
	int log_listen_fd;
	int log_sock_fd = -1;
	int sock_fd[BUFFER];
	int sock_fd_count = 0;

	struct sockaddr_in	sa = { 0 };
	socklen_t		sl = sizeof(sa);

	int finished;

	time_t open_time;

	char * reqs, * logtmp;

	fd_set rfds;
	struct timeval tv;
	int retval;

	start_time = time(NULL);

	listen_fd = open_socket(port);
	listen(listen_fd, 25);

	log_listen_fd = open_socket(LOG_PORT);
	listen(log_listen_fd, 2);

	reqs = (char *)malloc(sizeof(char) * BUFFER * (SMALL + 1));
	logtmp = (char *)malloc(sizeof(char) * BUFFER);

	for (i = 0; i < ITEMS_COUNT; ++i) {
		memset(global_data[i].key, 0, SMALL);
		memset(global_data[i].value, 0, SMALL);
	}

	is_dev_opened = 0;

	snprintf(logtmp, BUFFER, "Process started");
	write_log(log_fd, log_sock_fd, logtmp);

	while (1) {

		if (!is_dev_opened) {
			tty_fd = open_device(device_file);
			++is_dev_opened;
			open_time = time(NULL);

			if (tty_fd > 0) {
				last_update = time(NULL);
				usleep(MICROSLEEP);
			}
			if (-ENXIO == tty_fd) {
				snprintf(logtmp, BUFFER, "Device file %s doesn't exist", device_file);
				write_log(log_fd, log_sock_fd, logtmp);
				is_dev_opened = 0;
				usleep(MICROSLEEP);
			}
			if (!tty_fd) {
				snprintf(logtmp, BUFFER, "Device file %s can't be opened", device_file);
				write_log(log_fd, log_sock_fd, logtmp);
				is_dev_opened = 0;
				usleep(MICROSLEEP);
			}
		} else {
			open_time = time(NULL);
		}

		tv.tv_sec = 0;
		tv.tv_usec = MICROSLEEP;

		FD_ZERO(&rfds);
		FD_SET(listen_fd, &rfds);
		FD_SET(log_listen_fd, &rfds);

		if (is_dev_opened)
			FD_SET(tty_fd, &rfds);

		if (log_sock_fd > 0)
			FD_SET(log_sock_fd, &rfds);

		for (i = 0; i < sock_fd_count; ++i)
			FD_SET(sock_fd[i], &rfds);

		retval = select(
				get_max(tty_fd,
					listen_fd,
					log_listen_fd,
					log_sock_fd,
					sock_fd,
					sock_fd_count) + 1,
				&rfds,
				NULL,
				NULL,
				&tv);
		if (retval) {

			if (is_dev_opened && FD_ISSET(tty_fd, &rfds)) {
				char temp1[BUFFER];
				count = read(tty_fd, temp1, BUFFER);

				if (count + data_count > BUFFER) {
					goto device_reading_cleanup;
				}

				finished = 0;
				for (i = 0; i < count; ++i) {
					if (';' == temp1[i]) {
						++finished;
						break;
					}
				}

				memcpy(device_data + data_count, temp1, count * sizeof(char));
				data_count += count;

				if ((time(NULL) - open_time) > OUTDATE_TIMEOUT) { // Device vanished during reading, 10 sec timeout
					snprintf(logtmp, BUFFER, "Device  %s vanished during reading, closing", device_file);
					write_log(log_fd, log_sock_fd, logtmp);

					goto device_reading_cleanup;
				}

				if (finished) {
					device_data[data_count] = '\0';

					if (-1 == process_recv(log_fd, log_sock_fd, device_data)) {
						last_update = 0;
					}

					goto device_reading_cleanup;
				}

				goto device_reading_exit;

device_reading_cleanup:
				data_count = 0;
				continue;
device_reading_exit:
				;;
			}

			if (FD_ISSET(listen_fd, &rfds)) {
				sock_fd[sock_fd_count] = accept(listen_fd, (struct sockaddr *)&sa, &sl);
				getpeername(sock_fd[sock_fd_count], (struct sockaddr *)&sa, &sl);
				memset(reqs + sock_fd_count * (SMALL + 1), 0, sizeof(char) * (SMALL + 1));
				++sock_fd_count;

				snprintf(logtmp, BUFFER,
						"Accepted data connection from %s, has %d conns",
						inet_ntoa(sa.sin_addr), sock_fd_count);
				write_log(log_fd, log_sock_fd, logtmp);
				continue;
			}
			if (FD_ISSET(log_listen_fd, &rfds)) {
				if (log_sock_fd > 0) { // Closing previous connection
					close(log_sock_fd);
				}

				log_sock_fd = accept(log_listen_fd, (struct sockaddr *)&sa, &sl);
				getpeername(log_sock_fd, (struct sockaddr *)&sa, &sl);
				snprintf(logtmp, BUFFER, "Accepted log connection from %s", inet_ntoa(sa.sin_addr));
				write_log(log_fd, log_sock_fd, logtmp);
				continue;
			}
			if (log_sock_fd > -1 && FD_ISSET(log_sock_fd, &rfds)) {
				j = 0;
				ioctl(log_sock_fd, FIONREAD, &j);
				if (!j) { // Socket was closed on client
					close(log_sock_fd);
					log_sock_fd = -1;

					snprintf(logtmp, BUFFER, "Log connection closed");
					write_log(log_fd, log_sock_fd, logtmp);
				} else {// Data arrived to read should be ignored on this sock
					char temp1[BUFFER];
					read(log_sock_fd, temp1, BUFFER);
				}
			}
			for (i = 0; i < sock_fd_count; ++i) {
				if (FD_ISSET(sock_fd[i], &rfds)) {
					j = 0;
					ioctl(sock_fd[i], FIONREAD, &j);

					if (!j) { // Socket was closed, cleaning up
						close(sock_fd[i]);
						memmove(sock_fd + i, sock_fd + i + 1, sizeof(int) * sock_fd_count - i - 1);
						--sock_fd_count;

						snprintf(logtmp, BUFFER, "Closed, has %d conns", sock_fd_count);
						write_log(log_fd, log_sock_fd, logtmp);
						continue;
					} else {
						count = read(sock_fd[i], data, SMALL);

						if (count + *(reqs + i * (SMALL + 1)) <= SMALL) { // Unexpectedly big request?
							memcpy(reqs + i * (SMALL + 1) + *(reqs + i * (SMALL + 1)) + 1, data, count);
							*(reqs + i * (SMALL + 1)) += count;

							finished = 0;
							for (j = 0; j < *(reqs + i * (SMALL + 1)); ++j) {
								if ('\n' == *(reqs + i * (SMALL + 1) + 1 + j)) {
									finished = 2;  // Zabbix protocol
									break;
								}
							}

							if (finished) {
								*(reqs + i * (SMALL + 1) + j + 1) = 0x00;
								process_zabbix(reqs + i * (SMALL + 1) + 1, sock_fd[i]);
								*(reqs + i * (SMALL + 1)) = 0x00;
							}
						} else {
								count = snprintf(data, SMALL, "BIG_REQUEST\r");
								write(sock_fd[i], data, count);
						}
					}
				}
			}
		}

		if (!retval) {
			if (is_dev_opened && time(NULL) - last_update > OUTDATE_TIMEOUT) {
				data_count = 0;
				close(tty_fd);
				is_dev_opened = 0;

				snprintf(logtmp, BUFFER, "Endpoint communication was lost during reading (%u %u), reopen",
					(unsigned int)time(NULL), (unsigned int)last_update);
				write_log(log_fd, log_sock_fd, logtmp);
			}
		}
	}

	free(reqs);
	free(logtmp);

	return 0;
}

int main(int argc, char ** argv) {

#ifndef __DEBUG__
	pid_t pid, sid;
#endif

	char device[SMALL], port[SMALL];
	int log_fd, port_int = 0;

	if (argc > 1 && strlen(argv[1]) > 0)
		strncpy(device, argv[1], SMALL);
	else
		strcpy(device, DEVICE);

	if (argc > 2 && strlen(argv[2]) > 0)
		strncpy(port, argv[2], SMALL);
	else
		strcpy(port, "5000");

	umask(S_IWGRP | S_IWOTH);

	/* Checking correctness of specified port */
	port_int = strtol(port, NULL, 10);

	if (port_int < 10 || port_int > 65535) {
		printf("Unexpected port: %s\n", port);
		exit(EXIT_FAILURE);
	}

#ifndef __DEBUG__
	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}

	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	umask(S_IWGRP | S_IWOTH);

	sid = setsid();
	if (sid < 0) {
		exit(EXIT_FAILURE);
	}

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

#endif /* __DEBUG__ */

	log_fd = -1;

	memset(global_data, 0, sizeof(global_data));

	event_loop(device, port_int, log_fd);

	return 0;
}

