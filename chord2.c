/*
** main.c -- stream (TCP) base
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>
// #include <stropts.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/time.h>
#include <math.h>
#include "chord.h"

#define BACKLOG 10
#define MAXDATASIZE 1024 
#define FILE_NAME "config.txt"
#define PROC_COUNT 4
#define TTLMSGNUM 50
#define PORT_LEN    6	   // length of char array to hold port number
#define SETUP_WAIT  5      // wait time to allow other processes to set up
#define BASE_TEN    10	   // used to denote base ten in format conversion
#define IPADDR_LEN  16     // length of char array to hold ip address
#define PORT_LEN    6	   // length of char array to hold port number

/* Host Machine Info */
char my_port[PORT_LEN];
int my_pid;
int min_delay, max_delay;

/* Array of Socket Descriptors */
int sockets[PROC_COUNT];

/* State variable & Flags */
bool server_created = false;
bool used_pid[PROC_COUNT] = {false};
int node_id[256];

/* Multicast Buffer */
char mc_buf[1024];

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

/* Global Struct */
Node my_node;

/* Reap all dead processes */
void sigchld_handler(int s){
	(void)s; // quiet unused variable warning

	// waitpid() might overwrite errno, so we save and restore it:
	int saved_errno = errno;

	while(waitpid(-1, NULL, WNOHANG) > 0);

	errno = saved_errno;
}

/* Get sockaddr, IPv4 or IPv6: */
void *get_in_addr(struct sockaddr *sa){
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/* Serialize Node Struct */
int serialize(const Node* add_node, char* msg){
    int bytes = 0;
    memcpy(msg + bytes, &(add_node->nodeId), sizeof(add_node->nodeId));
    bytes += sizeof(add_node->nodeId);
    memcpy(msg + bytes, &(add_node->predecessor), sizeof(add_node->predecessor));
    bytes +=  sizeof(add_node->predecessor);
    memcpy(msg + bytes, &(add_node->successor), sizeof(add_node->successor));
    bytes += sizeof(add_node->successor);
    memcpy(msg +bytes, &(add_node->fingerTable), sizeof(add_node->fingerTable));
    bytes += sizeof(add_node->fingerTable);
    memcpy(msg + bytes , &(add_node->keys), sizeof(add_node->keys));
    bytes +=  sizeof(add_node->keys);
    return bytes;
}

/*  */
int offset = 32;
/* Unicast Functionality with delay */
void unicast_send(char* message){
	char * casted_message = (char*) message;
	int sd = (int)casted_message[0] - offset;
	// int sd = atoi(&sd_char);

	sleep(min_delay + rand()%(max_delay+1 -min_delay));
	if (send(sockets[sd], &casted_message[1], strlen(casted_message)-1, 0) == -1)
				perror("send");
	return;	
}

/* Unicast: a thread wrapper of unicast_send(message) */
void *unicast(void *arg)
{
	char* message = (char*)arg;
	unicast_send(message);
	pthread_exit(NULL);
}

/* Multicast Functionality */
void *multicast(void* arg){
	char* message = (char*)arg;
	char* casted_message;
	pthread_t *tid = malloc(PROC_COUNT* sizeof(pthread_t));
	int i = 0;
	for(i = 0; i < PROC_COUNT;i++){

		casted_message = malloc((strlen(message)+2)*sizeof(char));
		sprintf(casted_message,"%c",(char)(i+offset));
		strcpy(&(casted_message[1]), message);
		pthread_create(&tid[i], NULL, unicast,(void*)casted_message);
	}
	for(i = 0; i < PROC_COUNT; i++){
		pthread_join( tid[i], NULL);
	}
	free(tid);

	pthread_exit((void *)0);
}


/* Thread to handle receiving messages from remote clients to local server */
void * handle_connection(void* sd){
	int numbytes;
	char buf[MAXDATASIZE];
	int new_fd = *(int*) sd;

	while((numbytes = recv(new_fd, buf, MAXDATASIZE-1, 0)) > 0)
	{
		buf[numbytes] = '\0';
		printf("Server: Received '%s' --\n",buf);


		// printf("-- Messages counter: %d  --\n", messages_counter);
		// printf("r_count: %d\nread_op: %d\n", r_count, read_op);
		// printf("w_count: %d\nwrite_op: %d\n", w_count, write_op);

	}

	close(new_fd);
	printf("--CONNECTION HAS BEEN CLOSED--\n");
	pthread_exit(NULL);
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
		pthread_exit(NULL);
	}

	if (listen(sockfd, BACKLOG) == -1) {
		perror("listen");
		pthread_exit(NULL);
	}

	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
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
		sockets[socket_idx] = new_fd;
		socket_idx++;

		// Print connector info
		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);

		printf("[Server id(%lu)]: Got connection from %s.\n", data->pids[x], s);

		// Have thread handle the new connection
		int rc;
		rc = pthread_create(&handle_this, NULL, handle_connection, &new_fd);
		if(rc){
			printf("ERROR W/ THREAD CREATION.\n");
			exit(-1);
		}
	
	}

	// free(cinfo);
	pthread_exit(NULL);
}

/* Thread for creating a Client socket per Process ID */
void * thread_create_client(void * cl_info){
	struct Client_info *data = (struct Client_info *) cl_info;
	printf("THREAD: Client Creation initiated to connect to %s...\n", data->ip_addr);

	int sockfd, numbytes;  
	char buf[MAXDATASIZE];
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

	while(((numbytes = recv(sockfd, buf, MAXDATASIZE-1, 0)) > 0))
	{
		buf[numbytes] = '\0';
		printf("Client: Received: '%s'\n",buf);

		if(buf[0] == 'j'){
			memcpy(&my_node, &buf[1], sizeof(Node));
			printf("my_node.node_id = %d\n", my_node.nodeId);
			printf("my_node.predecessor = %d\n", my_node.predecessor);
			printf("my_node.successor = %d\n", my_node.successor);
			for(int i = 0; i < NUMBER_OF_BITS; i++)
				printf("my_node.finger[%d] = %d\n",i,my_node.fingerTable[i]);
		}
		

	}

	close(sockfd);
	printf("--CONNECTION HAS BEEN CLOSED--\n");
	pthread_exit(NULL);
}

// /* Thread to keep multicasting when Multicast buffer has message */
// void * thread_mcast(void* m){
// 	char * mcast_msg = (char *)m;
// 	char * temp_msg = malloc(strlen(mcast_msg) * sizeof(char));
// 	strcpy(temp_msg, mcast_msg);
// 	multicast(temp_msg);
// 	pthread_exit(NULL);
// }
// int closest_preceding_finger(int id){

// }

// int find_predecessor(int id){
// 	int n_prime = my_node.nodeId;
// 	int successor = ;
// 	//find 
// 	while(id < n_prime || id > successor){
// 		n_prime = closest_preceding_finger(id);
// 	}
// 	return n_prime;
// }

// int find_successor(int id){
// 	int predecessor = find_predecessor(id);

// }

Node init_finger_table(int new_id){
	Node add_node;
	add_node.nodeId = new_id;

	int start = new_id + 1;
	int i = start;

	bool forward = true;
	while(1){

		if(forward) {
			// Finding successor
			if(node_id[i] >= 0){
				add_node.fingerTable[0]= i;
				add_node.successor = i;
				forward = false;
				i = new_id - 1;
			}
			else
				i++;
			if(i == 256)
				i = 0;
		}
		else{
			// Finding predecessor
			if( (i != new_id) && (i < new_id) && (node_id[i] >= 0)){
				add_node.predecessor = i;
				break;
			}
			else{
				i--;
				if(i == 0)
					i == 255;
			}	
		}	
	}


	for(int i = 1; i < NUMBER_OF_BITS; i++){
		int t_start = new_id + pow(2,i);
		if(t_start <= add_node.successor)
			add_node.fingerTable[i] = add_node.successor;
		else{
			int j = t_start;
			while(1){
				// Finding next entry of finger table
				if(j > 255)
					j = j%255;

				if(node_id[j] >= 0){
					add_node.fingerTable[i]= j;
					break;
				}
				else
					j++;
				if(j == 256)
					j = 0;
			}
		}
	}
	return add_node;
}

/* Thread for taking inputs and multicasting */
void *stdin_client(void *arg){
	while(1){
		// Store inputs in buffer
		char input[MAXDATASIZE];
		fgets(input, MAXDATASIZE, stdin);
		input[strlen(input) -1] = '\0';

		if(input[0] == 'j'){ // join op
			int p = atoi(&input[5]);
			int unused_idx;
			for(int x = 0; x < PROC_COUNT; x++){
				if(used_pid[x] == false){
					used_pid[x] = true;
					unused_idx = x;
					node_id[p] = x;
					break;
				}
			}

			for(int i = 0; i < 256; i++)
				printf("node_id{%d] : %d\n", i, node_id[i]);

			/* Initialize finger table for new node */
			Node add_node = init_finger_table(p);
			printf("add_node.node_id = %d\n", add_node.nodeId);
			printf("add_node.predecessor = %d\n", add_node.predecessor);
			printf("add_node.successor = %d\n", add_node.successor);
			for(int i = 0; i < NUMBER_OF_BITS; i++)
				printf("add_node.finger[%d] = %d\n",i,add_node.fingerTable[i]);
			
			// Send Struct to new node
			char *msg = malloc(sizeof(Node)+3);
			sprintf(msg,"%c",(char)(node_id[p]+offset));
			msg[0] = 'j';
			int len = serialize(&add_node, &msg[1]);
			msg[len] = '\0';
			sleep(min_delay + rand()%(max_delay+1 -min_delay));
			if (send(sockets[node_id[p]], msg, sizeof(Node)+1, 0) == -1){
				perror("send");
			}
			
			// Tell successor of new node to update it's predecessor
			msg[0] = '\0';
			sprintf(msg,"%c",(char)(node_id[add_node.predecessor]+offset));
			strcpy(&(msg[1]), "up");
			sprintf(&(msg[3]),"%d",p);
			printf("join:%s\n", msg);
			unicast_send(msg);
		}


		// Set up Multicast buffer
		// mc_buf[0] = '\0';
		// strcat(mc_buf, input);
		// pthread_t mcast_me;
		// int rc = pthread_create(&mcast_me, NULL, multicast, (void*)mc_buf);
		// if(rc){
		// 	printf("ERROR W/ THREAD FOR SERVER CREATION.\n");
		// 	exit(-1);
		// }

		// // Exit STDIN Thread
		// if(strcmp(input, "quit") == 0){
		// 	printf("Quitting stdin thread...\n");
		// 	break;
		// }
	}
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
			if(isdigit(*str))
				min_delay = strtol(str, &str, BASE_TEN);
			str++;

			if(isdigit(*str))
				max_delay = strtol(str, &str, BASE_TEN);
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

	// Initialize global Node struct
	my_node.nodeId = my_pid;
	my_node.predecessor = my_pid;
	my_node.successor = my_pid;
	my_node.fingerTable[0] = my_pid;
	my_node.fingerTable[1] = my_pid;
	my_node.fingerTable[2] = my_pid;
	my_node.fingerTable[3] = my_pid;
	my_node.fingerTable[4] = my_pid;
	my_node.fingerTable[5] = my_pid;
	my_node.fingerTable[6] = my_pid;
	my_node.fingerTable[7] = my_pid;

	// Initialize boolean array of keys
	if(my_pid == 0)
		for(int x = 0; x < 256; x++)
			my_node.keys[x] = 1;
	else
		for(int x = 0; x < 256; x++)
			my_node.keys[x] = 0;

	// Used_pid array initialization (i.e., everyone connected to Node 0)
	used_pid[0] = true;
	for(int i = 0; i < 256; i++){
		node_id[i] = -1;
	}
	node_id[0] = 0;


	struct Client_info *cl_info;
	if(my_pid == 0){
		for(int x = 0; x < PROC_COUNT; x++){
			cl_info = malloc(sizeof(struct Client_info));
			cl_info->port = ports[x];
			cl_info->ip_addr = sep_ips[x];
			pthread_create(&proc_client[x], NULL, thread_create_client, (void*)cl_info);
		}
		pthread_create(&stdin_thread, NULL, stdin_client, NULL);
	}
	else{
		// Only connect to Node 0
		cl_info = malloc(sizeof(struct Client_info));
		cl_info->port = ports[0];
		cl_info->ip_addr = sep_ips[0];
		pthread_create(&proc_client[0], NULL, thread_create_client, (void*)cl_info);
	}

	pthread_exit(NULL);
	return 0;
}

