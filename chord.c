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
#include <sys/ioctl.h>
// #include <stropts.h>
#include <ctype.h>
#include <stdbool.h>
#include "chord.h"

#define BACKLOG 10
#define MAX_DATA_SIZE 128 
#define FILE_NAME "config1.conf"
#define PROC_COUNT 32
#define TTLMSGNUM 50
#define PORT_LEN    6	   // length of char array to hold port number
#define SETUP_WAIT  10      // wait time to allow other processes to set up
#define BASE_TEN    10	   // used to denote base ten in format conversion
#define IPADDR_LEN  16     // length of char array to hold ip address
#define PORT_LEN    6	   // length of char array to hold port number
#define IP "127.0.0.1"

//define global variables 
char* holdBack[TTLMSGNUM];

/* Host Machine Info */
char my_port[PORT_LEN];
int my_pid;
bool server_created = false;

char PORTS[PROC_COUNT][8];
int socketdrive[PROC_COUNT];
int min_delay, max_delay;

/* Struct to pass to thread to create server */
typedef struct Config_info{
	long *pids;
	char *ip_addresses;
	long *ports;
} Config_info;

/* Struct to pass to threads to create clients */
typedef struct Client_info{
	long port;
	char* ip_addr;
} Client_info;

/* Reap all dead processes */
void sigchld_handler(int s)
{
	while(waitpid(-1, NULL, WNOHANG) > 0);
}

/* Get sockaddr, IPv4 or IPv6: */
void *get_in_addr(struct sockaddr *sa)
{
	if(sa->sa_family == AF_INET){
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


/* Creating clients */
void *setupConnection(void* arg)
{
	struct Client_info *data = (struct Client_info *) arg;
	printf("THREAD: Client Creation initiated to connect to %s...\n", data->ip_addr);

	int sockfd, numbytes;  
	char buf[MAX_DATA_SIZE];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char connection_port[PORT_LEN];
	sprintf(connection_port, "%ld",data->port);

	if ((rv = getaddrinfo(data->ip_addr, connection_port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		//return 1;
		pthread_exit(NULL);
	}

	// Sleep temporarily to allow other machines to finish setup
	sleep(SETUP_WAIT);	

	// Loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			//perror("client: connect");
			close(sockfd);
			continue;
		}
		break;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			s, sizeof s);
	printf("client: connecting to %s\n", s);
	freeaddrinfo(servinfo); // all done with this structure

	while(((numbytes = recv(sockfd, buf, MAX_DATA_SIZE-1, 0)) > 0)){

	}

	close(sockfd);
	printf("--CONNECTION HAS BEEN CLOSED--\n");
	pthread_exit(NULL);
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
void * thread_create_server(void * cinfo){
	struct Config_info *data = (struct Config_info *) cinfo;
	printf("THREAD: Server Creation initiated...\n");

	// Parse IP Addresses from Struct passed as Argument
	char sep_ips[PROC_COUNT][IPADDR_LEN];
	int i = 0;
	int x;
	for(x = 0; x < PROC_COUNT; x++){
		int j = 0;
		while(data->ip_addresses[i] != ' ' && data->ip_addresses[i] != '\0'){
			sep_ips[x][j] = data->ip_addresses[i];
			j++;
			i++;
		}
		sep_ips[x][j] = '\0';
		i++;
	}

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

	// Select Available Port to Bind to.
	for(x = 0; x < PROC_COUNT; x++){

		// Bind to first unused port on local machine in config file
		sprintf(my_port, "%ld",data->ports[x]);

		if ((rv = getaddrinfo(NULL, my_port, &hints, &servinfo)) != 0) {
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

		// If did not bind because pointer is NULL, then continue.
		if(p == NULL)
			continue;
		else
			break;
	}

	printf("Binded to port: %s\n", my_port);
	my_pid = (int) data->pids[x];
	server_created = true;

	freeaddrinfo(servinfo); // all done with this structure

	if (p == NULL)  {
		fprintf(stderr, "server: failed to bind\n");
		//exit(1);
		pthread_exit(NULL);
	}

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

	printf("[PID:%lu] - Server now waiting for connections...\n\n", data->pids[x]);

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

		printf("[Server id(%lu)]: Got connection from %s.\n", data->pids[x], s);

		// Have thread handle the new connection
		pthread_create(&handle_this, NULL, do_server, &new_fd);
	}
	
	// free(cinfo);
	pthread_exit(NULL);
}


/* MAIN THREAD */
int main(int argc, char *argv[])
{
	/********  CONFIG FILE PARSING ********/
	FILE *file = fopen("config.txt", "r");
	char *file_buf = NULL;
	char *str = NULL;
	int file_len;
	int parse_sel= 0;
	int i = 0;

	if(file != NULL){
		// Get size of file
		if(fseek(file, 0L, SEEK_END) == 0){
			long size = ftell(file);
			if(size == -1){
				printf("Error with file size.\n");
				return -1;
			}

			file_buf = malloc(sizeof(char) * (size+1));
			if(fseek(file, 0L, SEEK_SET) != 0){
				printf("Error with file seek_set.\n");
				return -1;
			}

			// Read file into buffer
			size_t new_size = fread(file_buf, sizeof(char), size, file);
			file_buf[new_size++] = '\0';
		}
		fclose(file);
	}
	str = file_buf;
	file_len = sizeof(file_buf) / sizeof(file_buf[0]);

	// Arrays to hold config file's 
	long pids[PROC_COUNT];
	char ip_addresses[PROC_COUNT * IPADDR_LEN];
	long ports[PROC_COUNT];
	char *str_ip;
	// Reset values to re-read file buffer
	str = file_buf;
	str_ip = ip_addresses;
	i = 0;

	while(*str != '\0'){
		// Parsing
		if(parse_sel == 0){
			// Parse Channel Delays
			if(isdigit(*str)){
				min_delay = strtol(str, &str, BASE_TEN);
			}
			str++;

			if(isdigit(*str)){
				max_delay = strtol(str, &str, BASE_TEN);
			}
			str++;

			parse_sel++;
		}
		else if(parse_sel == 1){
			// Parse Process ID
			if(isdigit(*str)){
				long val = strtol(str, &str, BASE_TEN);
				pids[i] = val;
			}
			str++;

			// Parse IP Address
			while(*str != ' '){
				*str_ip = *str;
				str++;
				str_ip++;
			}
			if(i == PROC_COUNT-1)
				*str_ip = '\0';
			else
				*str_ip = ' ';
			str_ip++;
			str++;
			
			// Parse Port Number
			if(isdigit(*str)){
				long val = strtol(str, &str, BASE_TEN);
				ports[i] = val;
			}

			if(*str == '\n'){
				i++;
				str++;
			}
		}
	}
	free(file_buf);
	/******** END CONFIG FILE PARSING ********/

	// Parse through and separate IP Addresses
	char sep_ips[PROC_COUNT][IPADDR_LEN];
	i = 0;
	for(int x = 0; x < PROC_COUNT; x++){
		int j = 0;
		while(ip_addresses[i] != ' ' && ip_addresses[i] != '\0'){
			sep_ips[x][j] = ip_addresses[i];
			j++;
			i++;
		}
		sep_ips[x][j] = '\0';
		i++;
	}

	// for(int x = 0; x< PROC_COUNT; x++)
	// 	printf("sep_ips[%d] = %s\n", x, sep_ips[x]);

	/* THREAD CREATIONS */
	pthread_t proc_server;
	pthread_t proc_client[PROC_COUNT];
	pthread_t stdin_thread;
	int rc;

	// Create Server Thread
	struct Config_info *cinfo;
	cinfo =  malloc(sizeof(struct Config_info));
	cinfo->pids = pids;
	cinfo->ip_addresses = ip_addresses;
	cinfo->ports = ports;
	rc = pthread_create(&proc_server, NULL, thread_create_server, (void*)cinfo);
	if(rc){
		printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
		exit(-1);
	}
	
	// Create Client Threads
	while(!server_created);
	struct Client_info *cl_info;
	if(my_pid == 0){
		for(int x = 0; x < PROC_COUNT; x++){
			cl_info = malloc(sizeof(struct Client_info));
			cl_info->port = ports[x];
			cl_info->ip_addr = sep_ips[x];
			pthread_create(&proc_client[x], NULL, setupConnection, (void*)cl_info);
		}
		pthread_create(&stdin_thread, NULL, stdin_client, NULL);
	}
	else{
		// Only connect to Node 0
		cl_info = malloc(sizeof(struct Client_info));
		cl_info->port = ports[0];
		cl_info->ip_addr = sep_ips[0];
		pthread_create(&proc_client[0], NULL, setupConnection, (void*)cl_info);
	}

	pthread_exit(NULL);
	return 0;
}