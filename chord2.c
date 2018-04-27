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
#define SETUP_WAIT  6      // wait time to allow other processes to set up
#define BASE_TEN    10	   // used to denote base ten in format conversion
#define IPADDR_LEN  16     // length of char array to hold ip address
#define PORT_LEN    6	   // length of char array to hold port number

/* Host Machine Info */
char my_port[PORT_LEN];
int my_pid;
int min_delay, max_delay;

/* Array of Socket Descriptors of Server accepting connections */
int serv_sockets[PROC_COUNT];

/* Socket Descriptor to Node's Predecessor */
int pred_sd;

/* State variable & Flags */
bool server_created = false;
bool used_pid[PROC_COUNT] = {false};
int node_id[256];	// array of ids in chord network; entry corresponds to line number in config file
int port_nums[256];
bool expecting_show = false;
bool start_hbeat = false;
bool received_ack = false;

/* Multicast Buffer */
char mc_buf[1024];

/* Mutex */
pthread_mutex_t heartbeat_mutex;


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
    memcpy(msg + bytes, &(add_node->predecessorPort), sizeof(add_node->predecessorPort));
    bytes +=  sizeof(add_node->predecessorPort);
    memcpy(msg + bytes, &(add_node->successor), sizeof(add_node->successor));
    bytes += sizeof(add_node->successor);
    memcpy(msg +bytes, &(add_node->fingerTable), sizeof(add_node->fingerTable));
    bytes += sizeof(add_node->fingerTable);
    memcpy(msg +bytes, &(add_node->fingerTablePorts), sizeof(add_node->fingerTablePorts));
    bytes += sizeof(add_node->fingerTablePorts);
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
	if (send(serv_sockets[sd], &casted_message[1], strlen(casted_message)-1, 0) == -1)
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

/* Set up connection to Predecessor and send message */
void sendToPredecessor(void * arg){
	char *msg = (char *) arg;
	printf("SENDING TO PREDECESSOR...\n");

	int sockfd, numbytes;  
	char buf[MAXDATASIZE];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char connection_port[PORT_LEN];
	sprintf(connection_port, "%d",my_node.predecessorPort);

	if ((rv = getaddrinfo("127.0.0.1", connection_port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		pthread_exit(NULL);
	}

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

	// printf("Client to Server (uf): %s\n", msg);
	if (send(sockfd, msg, MAXDATASIZE, 0) == -1){
				perror("send");
	}

	printf("<3 SENT HEART-- From %d to %d\n", my_node.nodeId, my_node.predecessor);
	close(sockfd);
	return;
}

/* Set up connection to Successor and send message */
void sendHeartAck(int port_ack){
	printf("SENDING ACK TO HEART...\n");

	int sockfd, numbytes;  
	char buf[MAXDATASIZE];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char connection_port[PORT_LEN];
	sprintf(connection_port, "%d",port_ack);

	if ((rv = getaddrinfo("127.0.0.1", connection_port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		pthread_exit(NULL);
	}

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

	if (send(sockfd, "yes", MAXDATASIZE, 0) == -1){
				perror("send");
	}

	// printf("SENT YES-- From %d to port:%d\n", my_node.nodeId, port_ack);
	close(sockfd);
	return;
}

/* Thread to handle receiving messages from remote clients to local server */
void * heartbeat(void* arg){
	while(1){
		char t_hport[8];
		pthread_mutex_lock(&heartbeat_mutex);
		if(start_hbeat){
			pthread_mutex_unlock(&heartbeat_mutex);
			t_hport[0] = 'h';
			strcat(t_hport,my_port); 			// message is hPORT#
			// printf("SENDING THIS TO PRED: t_port: %s\n",t_hport );
			sendToPredecessor(t_hport);
			memset(t_hport,'\0',sizeof(t_hport));
			sleep(3);
			if(received_ack){
				printf("<3<3<3 Received Ack\n");
				received_ack = false;
			}
			else{
				printf("****-------DID NOT RECEIVE ACK!!!!!!!!!!!!--------****\n\n");
			}
		}
		else{
			// no send to predecessor
			pthread_mutex_unlock(&heartbeat_mutex);
		}
		
	}

	// printf("--285 HAS BEEN CLOSED--\n");
	pthread_exit(NULL);
}

/* Thread to handle receiving messages from remote clients to local server */
void * handle_connection(void* sd){
	int numbytes;
	char buf[MAXDATASIZE];
	int new_fd = *(int*) sd;

	while((numbytes = recv(new_fd, buf, MAXDATASIZE, 0)) > 0)
	{
		buf[numbytes] = '\0';
		printf("Server: Received '%s'\n",buf);

		if(expecting_show){
			Node t_node;
			memcpy(&t_node, buf, sizeof(Node));
			printf("Identifier: %d\n", t_node.nodeId);
			for(int i = 0; i < NUMBER_OF_BITS; i++)
				printf("fingerTable[%d] = %d\n",i,t_node.fingerTable[i]);
			for(int i = 0; i < NUMBER_OF_BITS; i++)
				printf("fingerTablePorts[%d] = %d\n",i,t_node.fingerTablePorts[i]);
			for(int i = 0; i < 256; i++)
				printf("keys[%d] = %d\n",i,t_node.keys[i]);

			expecting_show = false;

		}
		else if(buf[0] == 'u' && buf[1] == 'f'){
			printf("Server Updating Finger Table...\n");
			// Update Finger Table
			char *str = &buf[2];
			int new_nodeID;
			int i;
			int new_nodePort;

			if(isdigit(*str))
				new_nodeID = (int)strtol(str, &str, BASE_TEN);
			str++;
			if(isdigit(*str))
				i = (int) strtol(str, &str, BASE_TEN);
			str++;
			if(isdigit(*str))
				new_nodePort = (int) strtol(str, &str, BASE_TEN);

			int a = my_node.fingerTable[i];
			int b = my_node.nodeId ;
			int c = new_nodeID;
			if((c < a && a <= b)||(b<c && c < a) || (a <= b && b <c)){
				my_node.fingerTable[i] = new_nodeID;
				my_node.fingerTablePorts[i]= new_nodePort;

				// Send same message to predecessor
				if(new_nodeID != my_node.predecessor)
					sendToPredecessor(buf);
			}
		}
		else if(buf[0] == 'h' /*&& start_hbeat*/){
			// Received Heart Request (i.e., probing to see if this node is alive)
			char *str = &buf[1];
			int heartPort;
			if(isdigit(*str))
				heartPort = (int)strtol(str, &str, BASE_TEN);

			printf("Received heart...<3<3<3<3<3\n");
			pthread_mutex_lock(&heartbeat_mutex);
			if(start_hbeat || my_node.nodeId == 0){
				sendHeartAck(heartPort);
				printf("Sent Ack from %d\n",my_node.nodeId);
			}
			pthread_mutex_unlock(&heartbeat_mutex);
			
		}
		else if(strcmp(buf, "yes") == 0){
			received_ack = true;
		}



		// printf("-- Messages counter: %d  --\n", messages_counter);
		// printf("r_count: %d\nread_op: %d\n", r_count, read_op);
		// printf("w_count: %d\nwrite_op: %d\n", w_count, write_op);

	}

	close(new_fd);
	// printf("--360 HAS BEEN CLOSED--\n");
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
		if(socket_idx < PROC_COUNT){
			serv_sockets[socket_idx] = new_fd;
			socket_idx++;
		}
		
		// Print connector info
		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);

		// printf("[Server id(%lu)]: Got connection from %s.\n", data->pids[x], s);

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

	while(((numbytes = recv(sockfd, buf, MAXDATASIZE, 0)) > 0))
	{
		buf[numbytes] = '\0';
		printf("Client: Received: '%s'\n",buf);

		if(buf[0] == 'j'){
			// Join Self to Chord Network
			printf("-- Joining Chord Network...\n\n");
			memcpy(&my_node, &buf[1], sizeof(Node));
			my_node.keys[my_node.nodeId] = true;
			printf("my_node.node_id = %d\n", my_node.nodeId);
			printf("my_node.predecessor = %d\n", my_node.predecessor);
			printf("my_node.predecessorPort = %d\n", my_node.predecessorPort);
			printf("my_node.successor = %d\n", my_node.successor);
			for(int i = 0; i < NUMBER_OF_BITS; i++)
				printf("my_node.fingerTable[%d] = %d\n",i,my_node.fingerTable[i]);
			for(int i = 0; i < NUMBER_OF_BITS; i++)
				printf("my_node.fingerTablePorts[%d] = %d\n",i,my_node.fingerTablePorts[i]);
			// for(int i = 0; i < 256; i++)
			// 	printf("my_node.keys[%d] = %d\n",i,my_node.keys[i]);
			

			// if(my_node.predecessor == 0)
			// 	start_hbeat = false;
			// else
			// 	start_hbeat = true;

			pthread_mutex_lock(&heartbeat_mutex);	
			start_hbeat = true;
			pthread_mutex_unlock(&heartbeat_mutex);

		}
		else if(buf[0] == 'u' && buf[1] == 'p'){
			// Update Predecessor
			// printf("BEFORE UPDATE [PREDECESSOR]:\n");
			// printf("my_node.node_id = %d\n", my_node.nodeId);
			// printf("my_node.predecessor = %d\n", my_node.predecessor);
			// printf("my_node.predecessorPort = %d\n", my_node.predecessorPort);
			// printf("my_node.successor = %d\n", my_node.successor);

			// str points to first integer in buffer
			char *str = &buf[2];
			if(isdigit(*str))
				my_node.predecessor = (int)strtol(str, &str, BASE_TEN);
			str++;
			if(isdigit(*str))
				my_node.predecessorPort = (int) strtol(str, &str, BASE_TEN);


			// if(my_node.predecessor == 0)
			// 	start_hbeat = false;
			// else
			// 	start_hbeat = true;

			// start_hbeat = true;

			// printf("AFTER UPDATE [PREDECESSOR]:\n");
			// printf("my_node.node_id = %d\n", my_node.nodeId);
			// printf("my_node.predecessor = %d\n", my_node.predecessor);
			// printf("my_node.predecessorPort = %d\n", my_node.predecessorPort);
			// printf("my_node.successor = %d\n", my_node.successor);
			// }
		}
		else if(buf[0] == 'u' && buf[1] == 'k'){
			// Update Keys
			
			// printf("BEFORE UPDATE [KEYS]:\n");
			// printf("my_node.node_id = %d\n", my_node.nodeId);
			// for(int i = 0; i < 256; i++)
			// 	printf("my_node.keys[%d] = %d\n",i,my_node.keys[i]);
			int new_nodeID = atoi(&buf[2]);
			int i = (my_node.predecessor + 1)%256;
			while(i != (new_nodeID + 1)%256 ){
				my_node.keys[i] = false;
				i = (i+1)%256;
			}
			// printf("AFTER UPDATE [KEYS]:\n");
			// printf("my_node.node_id = %d\n", my_node.nodeId);
			// for(int i = 0; i < 256; i++)
			// 	printf("my_node.keys[%d] = %d\n",i,my_node.keys[i]);

		}
		else if(buf[0] == 'u' && buf[1] == 'f'){
			// Update Finger Table
			printf("Updating Finger table...\n");
			char *str = &buf[2];
			int new_nodeID;
			int i;
			int new_nodePort;
			if(isdigit(*str))
				new_nodeID = (int)strtol(str, &str, BASE_TEN);
			str++;
			if(isdigit(*str))
				i = (int) strtol(str, &str, BASE_TEN);
			str++;
			if(isdigit(*str))
				new_nodePort = (int) strtol(str, &str, BASE_TEN);

			int a = my_node.fingerTable[i];
			int b = my_node.nodeId ;
			int c = new_nodeID;
			if((c < a && a <= b)||(b<c && c < a) || (a <= b && b <c)){
				my_node.fingerTable[i] = new_nodeID;
				my_node.fingerTablePorts[i] = new_nodePort;
				// Send same message to predecessor

				if(new_nodeID != my_node.predecessor)
					sendToPredecessor(buf);
			}
		}
		else if(buf[0] == 's' && buf[5] == 'a'){

		}
		else if(strcmp(buf,"show") == 0){
			char *msg = malloc(sizeof(Node)+1);
			int len = serialize(&my_node, msg);
			msg[len] = '\0';
			// sleep(min_delay + rand()%(max_delay+1 -min_delay));
			if (send(sockfd, msg, sizeof(Node)+1, 0) == -1){
				perror("send");
			}

			free(msg);

		}
		else if(strcmp(buf,"crash") == 0){
			printf("RECEIVED CRASH\n");
			pthread_mutex_lock(&heartbeat_mutex);
			start_hbeat = false;
			pthread_mutex_unlock(&heartbeat_mutex);

		}

		/**
		if find_op
			send (req to highest node in finger table OR to node with id matching key value)
			recv (wait for response)




		*/

	}

	close(sockfd);
	// printf("--676 HAS BEEN CLOSED--\n");
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

	int start = (new_id + 1)%256;
	int i = start;

	/* This loop is used to find succesor/predecessor of new node */
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
				if(i == -1)
					i == 255;
			}	
		}	
	}

	/* For loop to aid in finger table initialization */
	for(int i = 1; i < NUMBER_OF_BITS; i++){
		int t_start = new_id + pow(2,i);
		t_start = t_start%256;
		if(t_start <= add_node.successor && ((new_id + pow(2,i)) < 256) )
			add_node.fingerTable[i] = add_node.successor;
		else{
			int j = t_start;
			while(1){
				// Finding next entry of finger table
				if(j > 255)
					j = j%256;

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

	/* Key Distribution for new node */
	// Find index from which to start transferring keys
	int key_start_idx = add_node.predecessor+1;

	// This loop finds the index of a recent crash most immediately before its own ID
	//  (i.e., the index of what would have been the predecessor if the failed node had not crashed)
	for(int i = add_node.nodeId-1; i >= add_node.predecessor+1; i--){
		if(node_id[i] == -2){
			key_start_idx = i+1;
			break;
		}
	}

	for(int i = 0; i < 256; i++){
		if(i >= key_start_idx && i <= add_node.nodeId )
			add_node.keys[i] = true;
		else
			add_node.keys[i] = false;
	}

	return add_node;
}

/* Thread for taking inputs and multicasting */
void *stdin_client(void * cinfo){
	struct Config_info *data = (struct Config_info *) cinfo;

	while(1){
		// Store inputs in buffer
		char input[MAXDATASIZE];
		fgets(input, MAXDATASIZE, stdin);
		input[strlen(input) -1] = '\0';

		if(input[0] == 'j'){ // join op
			int p = atoi(&input[5]);

			// In node_id array, index is new PID, entry at index is line in config file
			for(int x = 0; x < PROC_COUNT; x++){
				if(used_pid[x] == false){
					used_pid[x] = true;
					node_id[p] = x;
					port_nums[p] = data->ports[x];
					break;
				}
			}
			// for(int i = 0; i < 256; i++)
			// 	printf("node_id[%d] : %d\n", i, node_id[i]);

			/* Initialize finger table for new node */
			Node add_node = init_finger_table(p);
			// printf("add_node.node_id = %d\n", add_node.nodeId);
			// printf("add_node.predecessor = %d\n", add_node.predecessor);
			// printf("add_node.successor = %d\n", add_node.successor);

			/* Initialize predecessor port for new node */
			add_node.predecessorPort = port_nums[add_node.predecessor];
			
			/* For loop to aid in finger table ports initialization */
			/* NOTE: this is the table used in place of IP Addresses */
			for(int i = 0; i < NUMBER_OF_BITS; i++){
				int finger_id = add_node.fingerTable[i];
				add_node.fingerTablePorts[i] = port_nums[finger_id];
				// add_node.fingerTablePorts[i] = data->ports[finger_id];
			}

			// Send Struct to new node
			char *msg = malloc(sizeof(Node)+3);
			sprintf(msg,"%c",(char)(node_id[p]+offset));
			msg[0] = 'j';
			int len = serialize(&add_node, &msg[1]);
			msg[len] = '\0';
			sleep(min_delay + rand()%(max_delay+1 -min_delay));
			if (send(serv_sockets[node_id[p]], msg, sizeof(Node)+1, 0) == -1){
				perror("send");
			}

			// Tell Successor of new node to update keys
			// (successor should set to false [successor.predecessor+1, new_id])
			msg[0] = '\0';
			sprintf(msg,"%c",(char)(node_id[add_node.successor]+offset));
			strcpy(&(msg[1]), "uk");
			sprintf(&(msg[3]),"%d",(p));
			unicast_send(msg);

			// Tell successor of new node to update it's predecessor
			msg[0] = '\0';
			sprintf(msg,"%c",(char)(node_id[add_node.successor]+offset));
			strcpy(&(msg[1]), "up");
			sprintf(&(msg[3]),"%d",p);
			strcat(msg," ");
			char t_port[6];
			sprintf(t_port,"%d",port_nums[p]);
			strcat(msg,t_port);
			unicast_send(msg);

			// Tell other nodes to update their finger tables (per Pg.6 of chord_sigcomm.pdf)
			for(int i = 0; i < NUMBER_OF_BITS; i++){
				// Find last node whose ith finger might be p,i.e., new node joining
				int pred_val = ((int)(p - pow(2, i)))%256;
				if(pred_val < 0){
					pred_val += 256;
				}

				// printf("pred_val : %d\n", pred_val);
				int nnew_pred;
				if(i == 0)
				 	nnew_pred = add_node.predecessor;
				else{
					int j = pred_val;

					// printf("j: %d p:%d\n",j,p);
					while(j != (p-1%256)){
						// printf("node_id[%d] = %d\n", j, node_id[j]);
						if(node_id[j] != -1){
							// printf("node_id[%d]:%d\n", j,node_id[j]);
							nnew_pred = j;
							break;
						}
						j = (j-1)%256;
					}
				}

				// printf("nnew_pred:%d\n", nnew_pred);
				// Do not send update finger table message to new node
				// because it already has updated finger table
				if(nnew_pred == p)
					continue;

				msg[0] = '\0';
				sprintf(msg,"%c",(char)(node_id[nnew_pred]+offset));

				strcpy(&msg[1], "uf");

				// Add new node id (p) and iteration number i
				sprintf(&(msg[3]),"%d",p);
				strcat(msg," ");
				char t_iteration[2];
				sprintf(t_iteration,"%d",i);
				strcat(msg, t_iteration);
				strcat(msg," ");
				char t_port[6];
				sprintf(t_port, "%d",port_nums[p]);
				strcat(msg, t_port);
				// printf("UPDATE FINGER TABLE MSG: %s\n", msg);
				unicast_send(msg);
			}
			pthread_mutex_lock(&heartbeat_mutex);
			start_hbeat = true;
			pthread_mutex_unlock(&heartbeat_mutex);

			free(msg);
		}
		else if(input[0] == 's' && input[5] == 'a'){
			// Show all (request information from all nodes in Chord Network)

		}
		else if(input[0] == 's'){
			// Show p (request information from node p in Chord Network)

			// Check if p is in Chord Network
			int req_node = atoi(&input[5]);
			if(node_id[req_node] == -1){
				printf("%d does not exist.\n", req_node);
				continue;
			}

			// If p is in Chord Network, request Node information
			expecting_show = true;
			int req_sd = node_id[req_node];
			if (send(serv_sockets[req_sd], "show", 5, 0) == -1){
				perror("send");
			}
		}
		else if(input[0] == 'c'){
			// Crash operation
			int p = atoi(&input[6]);

			// Check if Node p is in Chord Network
			if(node_id[p] < 0){
				printf("%d does not exist.\n", p);
				continue;
			}

			// Send message to Node p to crash
			int p_sd = node_id[p];
			if (send(serv_sockets[p_sd], "crash", 5, 0) == -1){
				perror("send");
			}

			// Set node status to crashed
			node_id[p] = -2;
			// Set node to unused
			used_pid[p] = false;
		}
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
	my_node.nodeId = 0;
	my_node.predecessor = 0;
	my_node.successor = 0;
	my_node.fingerTable[0] = 0;
	my_node.fingerTable[1] = 0;
	my_node.fingerTable[2] = 0;
	my_node.fingerTable[3] = 0;
	my_node.fingerTable[4] = 0;
	my_node.fingerTable[5] = 0;
	my_node.fingerTable[6] = 0;
	my_node.fingerTable[7] = 0;

	// Initialize boolean array of keys
	if(my_pid == 0){
		for(int x = 0; x < 256; x++)
			my_node.keys[x] = true;
	}
	else
		for(int x = 0; x < 256; x++)
			my_node.keys[x] = false;

	// Used_pid array initialization (i.e., everyone connected to Node 0)
	used_pid[0] = true;
	for(int i = 0; i < 256; i++){
		node_id[i] = -1;
	}
	node_id[0] = 0;
	port_nums[0] = 3490;


	struct Client_info *cl_info;
	if(my_pid == 0){
		for(int x = 0; x < PROC_COUNT; x++){
			cl_info = malloc(sizeof(struct Client_info));
			cl_info->port = ports[x];
			cl_info->ip_addr = sep_ips[x];
			pthread_create(&proc_client[x], NULL, thread_create_client, (void*)cl_info);
		}
		pthread_create(&stdin_thread, NULL, stdin_client, cinfo);
	}
	else{
		// Only connect to Node 0
		cl_info = malloc(sizeof(struct Client_info));
		cl_info->port = ports[0];
		cl_info->ip_addr = sep_ips[0];
		pthread_create(&proc_client[0], NULL, thread_create_client, (void*)cl_info);
	}


	pthread_t heart_;
	pthread_create(&heart_, NULL, heartbeat, NULL);

	pthread_exit(NULL);
	return 0;
}

