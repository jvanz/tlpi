#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "logger.h"

static int sfd; //socket file descriptor
static int epoll; // epoll instance
/** File descriptor where the log should be written*/
static int file;

static const char* LOG_FILE = "/tmp/logger.log";
static const int BACKLOG = 128;
static const int MAX_EVENTS = 10;
static const unsigned int MAX_CHILD = 2;

/* Prototypes */
static void init(void);
static void cleanup(void);
static void write_message(struct log_entry, char* message);
static const char* get_level_str(enum log_level);
static void run(void);

static void run(void)
{
	struct epoll_event events[MAX_EVENTS];
	int ready;
	for (;;){
		ready = epoll_wait(epoll, events, MAX_EVENTS, -1);
		if (ready == -1){
			printf("PID(%d): epoll_wait: %s\n", getpid(), strerror(errno));
			continue;
		}
		for (int i = 0; i < ready; i++){
			if (events[i].data.fd == sfd){
				printf("PID(%d): New connection coming...\n", getpid());
				// seems to be a new connection. Let's add it
				// in the epoll
				int cfd = accept(events[i].data.fd, NULL, NULL);
				if (cfd == -1){
					printf("PID(%d): Cannot accept connection: %s\n", getpid(), strerror(errno));
				} else {
					struct epoll_event ev;
					ev.events = EPOLLIN;
					ev.data.fd = cfd;
					if (epoll_ctl(epoll, EPOLL_CTL_ADD, cfd, &ev) == -1)
						printf("PID(%d): Cannot add new fd(%d):\t%s\n", getpid(),
							cfd, strerror(errno));
					else
						printf("PID(%d): added new fd(%d)\n", getpid(), cfd);
				}
			} else {
				if (events[i].events & EPOLLERR){
					printf("fd %d:\tEPOLLERR\n", events[i].data.fd);
				}
				if (events[i].events & EPOLLHUP){
					if (epoll_ctl(epoll, EPOLL_CTL_DEL, events[i].data.fd, NULL) == -1){
						printf("PID(%d): Cannot delete fd(%d):\t%s\n", getpid(),
							events[i].data.fd, strerror(errno));
					} else{
						printf("PID(%d): removed fd(%d)\n", getpid(), events[i].data.fd);
						if (close(events[i].data.fd) == -1)
							printf("PID(%d): cannot close fd(%d):\t%s\n", getpid(), events[i].data.fd, strerror(errno));
					}
				}
				if (events[i].events & (EPOLLIN | EPOLLPRI)){
					struct log_entry log;
					ssize_t bytes = read(events[i].data.fd, &log, sizeof(log));
					if (bytes == -1)
						continue;
					char* message = malloc(log.length+1);
					memset(message, 0, log.length+1);
					bytes = read(events[i].data.fd, message, log.length);
					if (bytes == -1){
						free(message);
						continue;
					}
					write_message(log, message);
					free(message);
				}
			}
		}
	}
}

static void write_message(struct log_entry log, char* message)
{
	char* time = ctime(&log.time);
	time[24] = '\0';
	dprintf(file, "%s (%d) %s: %s\n", time, getpid(), get_level_str(log.level), message);
}

static const char* get_level_str(enum log_level level)
{
	if (level == DEBUG)
		return "DEBUG";
	if (level == INFO)
		return "INFO";
	if (level == WARN)
		return "WARN";
	if (level == ERROR)
		return "ERROR";
	return "UNKNOWN";
}

/**
 * Function to initialize all resources pessary to the logger work properly
 */
static void init(void)
{
	//make sure the socket are closed before try to create it again
	cleanup();

	//open file to write the log messages
	file = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND,  S_IRUSR | S_IWUSR
		| S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH );
	if (file == -1)
		err(1,NULL);

	//create the unix socket
	struct sockaddr_un addr;
	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sfd == -1)
		err(1,NULL);
	printf("PID(%d): Socket created\n", getpid());

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKNAME, sizeof(addr.sun_path) -1);

	if (bind(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
		err(1,NULL);
	printf("PID(%d): Socket binded\n", getpid());

	if (listen(sfd, BACKLOG) == -1)
		err(1,NULL);
	printf("PID(%d): Listening...\n", getpid());

	//create the epoll instance to monitor the fds used by the logger
	epoll = epoll_create1(0);
	if (epoll < 0)
		err(1,NULL);
	printf("PID(%d): epoll created\n", getpid());

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = sfd;
	if (epoll_ctl(epoll, EPOLL_CTL_ADD, sfd, &ev) == -1)
		err(1,NULL);
	printf("PID(%d): epoll setup\n", getpid());
}

/**
 * Function to cleanup all resources used by logger daemon
 */
static void cleanup(void)
{
	//remove the socket
	if (unlink(SOCKNAME) == -1)
		printf(strerror(errno));
	// close log file descriptor
	close(file);
}

int main(void)
{
	init();
	// launch children process
	int is_parent = 1;
	for (unsigned int i = 0; i < MAX_CHILD; ++i){
		pid_t pid = fork();
		if (pid == -1)
			err(1,NULL);
		// children process should not spawn new process
		if (!pid){
			is_parent = 0;
			break;
		}
	}
	if (is_parent){
		// wait for the children process
		unsigned int has_child = MAX_CHILD;
		while (has_child){
			int wstatus = 0;
			pid_t pid =  wait(&wstatus);
			if (pid > 0)
				has_child--;
			else
				printf("wait: %s", strerror(errno));
		}
	} else {
		// children process should handle the income connection/events
		run();
	}
	cleanup();
}
