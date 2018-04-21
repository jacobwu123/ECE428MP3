#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include "chord.h"

#define BACKLOG 10
#define MAX_DATA_SIZE 128 
#define FILE_NAME "config1.conf"
#define PROC_COUNT 128
#define TTLMSGNUM 50
#define IP "127.0.0.1"

//define global variables 
char* holdBack[TTLMSGNUM];

char PORTS[PROC_COUNT][8];
int socketdrive[PROC_COUNT];
int min_delay, max_delay;

void sigchld_handler(int s)
{
	while(waitpid(-1, NULL, WNOHANG) > 0);
}

void *get_in_addr(struct sockaddr *sa)
{
	if(sa->sa_family == AF_INET){
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void read_configure()
{
	char * line = NULL;
	size_t len = 0;
	FILE *fp = fopen (FILE_NAME, "r");
	int i = 0;
	if(fp!= NULL){
		printf("Setting up configuration...\n");

		fscanf(fp, "%d %d", &min_delay, &max_delay);
		printf("min_delay:%d, max_delay:%d\n",min_delay, max_delay);

		while(i < PROC_COUNT){
			fscanf(fp, "%s", PORTS[i]);
			i++;
		}

		printf("Setting up configuration successfully.\n");
		fclose(fp);
	}
}

int getServer(struct addrinfo* servinfo, struct addrinfo* *p){
	int sockfd;

	for(*p = servinfo; *p!=NULL;*p= (*p)->ai_next){
		if((sockfd = socket((*p)->ai_family, (*p)->ai_socktype, 
			(*p)->ai_protocol)) == -1){
			perror("client:socket");
			continue;
		}
		if(connect(sockfd, (*p)->ai_addr, (*p)->ai_addrlen)==-1){
			close(sockfd);
			perror("client: connect");
			continue;
		}
		break;
	}

	return sockfd;
}

void *setupConnection(void* arg)
{
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int sockfd;
	int dest = (int) arg;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(IP, PORTS[dest], &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		pthread_exit((void *)0);
	}

	sleep(8);//wait for other processes to open
	sockfd = getServer(servinfo, &p);
	if(p == NULL){
		fprintf(stderr, "client: fail to connect\n");
		pthread_exit((void *)0);
	}

	socketdrive[dest] = sockfd;
	freeaddrinfo(servinfo);
	pthread_exit((void *)0);

}

/*
	unicast_send:
	input:
		dest: the destination of the message to send
		message: message
	function:
		1. cast the message with pid
		2. simulate the transmission delay
		3. send the message
*/
void unicast_send(int dest, char* message){
	char casted_message[strlen(message)+2];
	int delay_per_send = rand()%(max_delay - min_delay) + min_delay;;


	strcpy(&(casted_message[2]), message);

	sleep(delay_per_send);
	if(send(socketdrive[dest], casted_message, strlen(casted_message),0) == -1) 
		return;
}

/*
	unicast:
	a thread wrapper of unicast_send(dest, message)
*/
void *unicast(void *arg)
{
	char* message = (char*)arg;
	unicast_send(atoi(&(message[0])), &(message[1]));
	pthread_exit((void *)0);
}

void *multicast(void* arg){
	char* message = (char*)arg;
	char* casted_message;
	pthread_t *tid = malloc(PROC_COUNT* sizeof(pthread_t));
//	strcpy(multi_message, message);
	int i = 0;

	for(i = 0; i < PROC_COUNT;i++){
		if(message[0] == 'o' && i == 0)
			continue;
		casted_message = malloc((strlen(message)+2)*sizeof(char));
		sprintf(casted_message,"%d",i);
		strcpy(&(casted_message[1]), message);
		pthread_create(&tid[i], NULL, unicast,(void*)casted_message);
	}
	for(i = 0; i < PROC_COUNT; i++){
		pthread_join( tid[i], NULL);
	}
	free(tid);

	pthread_exit((void *)0);
}

/*
	stdin_read:
		read from the terminal
		function:
			1. if command is 'put x 1':
				multicast the message in the form: px1
			2. if command is 'get x':
				multicast the message in teh form: gx
			3. if command is 'dump':
				display all the local vaiables
			4. if command is 'delay x':
				put the thread in sleep for x millisecs
*/
void *stdin_client(void *arg){
	char command[16];
	char* message= malloc(4*sizeof(char));
	pthread_t chld_thr;
	int i;

	while(fgets(command, sizeof(command), stdin) > 0){
		command[strlen(command)-1] = '\0';
	}
	pthread_exit((void *)0);
}

/*
	do_client:
		client thread for receiving
*/
void *do_server(void *arg)
{
	int mysocfd = (int) arg;
	char* message = malloc(MAX_DATA_SIZE*sizeof(char));
	message[0] = '\0';	
	char* t_message;
	int i;
	int source;
	int numbytes;
	char act[10];
	int dest;
	pthread_t chld_thr, chld_thr2, chld_thr3;
	char* seqMessage;
	char *token;

	while((numbytes = recv(mysocfd, message, MAX_DATA_SIZE-1, 0))>0){
		
	}

	/* close the socket and exit this thread */
	close(mysocfd);
	pthread_exit((void *)0);
}

/* Thread for creating a Server socket per Process ID in config file */
void * thread_create_server(void * arg){
	int idx = *((int *) arg);
	char port[8];
	strcpy(port,PORTS[idx]);
	printf("THREAD: Server Creation initiated...\n");

	int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address information
	socklen_t sin_size;
	struct sigaction sa;
	int yes = 1;
	char s[INET6_ADDRSTRLEN];
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		//return 1;
		pthread_exit(NULL);
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1) {
			perror("setsockopt");
			//exit(1);
			pthread_exit(NULL);
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("server: bind");
			continue;
			//break;
		}

		break;
	}

	if(p == NULL){
		fprintf(stderr, "server: failed to bind\n");
		pthread_exit(NULL);
	}

	freeaddrinfo(servinfo); // all done with this structure

	if (listen(sockfd, BACKLOG) == -1) {
		perror("listen");
		//exit(1);
		pthread_exit(NULL);
	}

	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
		//exit(1);
		pthread_exit(NULL);
	}

	// handle_this thread will handle connections accepted by local server
	pthread_t handle_this;
	int socket_idx;
	while(1) {
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept");
			continue;
		}

		// Add to array of Server-Client sockets
		socketdrive[socket_idx] = new_fd;
		socket_idx++;

		// Print connector info
		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);

		// Have thread handle the new connection
		int rc;
		rc = pthread_create(&handle_this, NULL, do_server, &new_fd);
		if(rc){
			printf("ERROR W/ THREAD CREATION.\n");
			exit(-1);
		}
	
	}

	// free(cinfo);
	pthread_exit(NULL);
}

int main(int argc, char const *argv[]){

	struct addrinfo hints, *servinfo, *p;
	int rv;
	int sockfd, new_fd;
	int yes =1;
	struct sigaction sa;
	socklen_t sin_size;
	struct sockaddr_storage their_addr;
	int numbytes;
	char buf[MAX_DATA_SIZE];
	pthread_t chld_thr, chld_thr1;
	pthread_t *tid = malloc( PROC_COUNT* sizeof(pthread_t));
	char s[INET6_ADDRSTRLEN];
	int i;

	if(argc != 2){
		fprintf(stderr, "usage: server portnumber\n");
	}


	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	//read-in configure file here
	read_configure();

	if((rv = getaddrinfo(NULL, PORTS[0], &hints, &servinfo)) != 0){
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	for(p = servinfo; p!= NULL; p = p->ai_next){
		if((sockfd=socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1){
			perror("server: socket");
			continue;
		}

		if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1){
			perror("setsockopt");
			continue;
		}

		if(bind(sockfd, p->ai_addr, p->ai_addrlen) == -1){
			close(sockfd);
			perror("server: bind");
			continue;
		}

		break;
	}

	if(p == NULL){
		fprintf(stderr, "server: failed to bind\n");
		return 2;
	}

	freeaddrinfo(servinfo);

	/* set the level of thread concurrency we desire */
	pthread_setconcurrency(5);

	if(listen(sockfd, BACKLOG) == -1){
		perror("listen");
		exit(1);
	}

	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if(sigaction(SIGCHLD, &sa, NULL) == -1){
		perror("sigaction");
		exit(1);
	}

	printf("server: waiting for connections....\n");

	pthread_t servers[PROC_COUNT];
	for(i = 0; i < PROC_COUNT;i++){
		pthread_create(&servers[i], NULL, thread_create_server,(void*)(i));
	}

	for(i = 0; i < PROC_COUNT; i++){
		pthread_join( servers[i], NULL);
	}


	for(i = 0; i < PROC_COUNT;i++){
		pthread_create(&tid[i], NULL, setupConnection,(void*)i);
	}

	for(i = 0; i < PROC_COUNT; i++){
		pthread_join( tid[i], NULL);
	}

	pthread_create(&chld_thr1, NULL, stdin_client, NULL);

	while(1){
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		if(new_fd == -1){
			perror("accept");
			continue;
		}

		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);
		printf("server: got connection from %s\n", s);
		/* create a new thread to process the incomming request */
		pthread_create(&chld_thr, NULL, do_server, (void *)new_fd);
	}

	for(i = 0; i < PROC_COUNT; i++){
		free(tid[i]);
	}

	return 0;
}