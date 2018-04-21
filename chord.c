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


#define BACKLOG 10
#define MAX_DATA_SIZE 128 
#define FILE_NAME "config1.conf"
#define PROC_COUNT 7
#define TTLMSGNUM 50

//define global variables 
char* holdBack[TTLMSGNUM];
int CurMsgNum = 0;
int numDelivered = 0;
int sequenceNum = 0;


char PORTS[PROC_COUNT][16];
char IP[PROC_COUNT][16];
int socketdrive[PROC_COUNT];
int process_id;
int delay[PROC_COUNT][PROC_COUNT];

//mutex:
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t seq = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;


int keys[26];

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

void setup_delay(int min_delay, int max_delay){
	int i,j;
		// setting up delay 
	srand(time(NULL));
	while(i < PROC_COUNT){
		delay[i][i] = 0;
		for(j = i+1; j < PROC_COUNT; j++){
			delay[i][j] = rand()%(max_delay - min_delay) + min_delay;
			delay[j][i] = delay[i][j];
		}
		i ++;
	}
}

void read_configure()
{
	char * line = NULL;
	size_t len = 0;
	FILE *fp = fopen (FILE_NAME, "r");
	int i = 0;
	int min_delay, max_delay;
	if(fp!= NULL){
		printf("Setting up configuration...\n");

		fscanf(fp, "%d %d", &min_delay, &max_delay);
		printf("min_delay:%d, max_delay:%d\n",min_delay, max_delay);

		while(i < PROC_COUNT){
			fscanf(fp, "%s %s", IP[i], PORTS[i]);
			// printf("Process %d: IP:%s, ",i,IP[i]);
			// printf("PORT:%s\n", PORTS[i]);
			i++;
		}

		printf("Setting up configuration successfully.\n");
		fclose(fp);
	}
	setup_delay(min_delay, max_delay);
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

	if ((rv = getaddrinfo(IP[dest], PORTS[dest], &hints, &servinfo)) != 0) {
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
	int delay_per_send = delay[process_id][dest];

	sprintf(casted_message," %d",process_id);
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
	pthread_mutex_lock(&send_lock);
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
	pthread_mutex_unlock(&send_lock);

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
void *stdin_read(void *arg){
	char command[16];
	char* message= malloc(4*sizeof(char));
	pthread_t chld_thr;
	int i;

	while(fgets(command, sizeof(command), stdin) > 0){
		command[strlen(command)-1] = '\0';
		if(command[0] == 'p'){
			/*Perform a totally-ordered multicast of Write(X, v).*/
			message[0] = 'p';
			message[1] = command[4];
			message[2] = command[6];
			message[3] = '\0';
			pthread_create(&chld_thr,NULL,multicast,(void*)message);
		}
		else if(command[0] == 'g'){
			/*Perform a totally-ordered multicast of Read(X).*/
			message[0] = 'g';
			message[1] = command[4];
			message[2] = '\0';
			pthread_create(&chld_thr,NULL,multicast,(void*)message);
		}
		else if(command[0] == 'd' && command[1] == 'e'){
			strcpy(message, &(command[6]));
			printf("sleeping for %s milliseconds...\n", message);
			usleep(atoi(message)*1000);
			printf("waking up...\n");
		}
		else if(command[0] == 'q') 
		{
			//print out the hold back queue
			for(i = 0; i < TTLMSGNUM;i++){
				if(holdBack[i]!=NULL){
					printf("%s\n", holdBack[i]);
				}
			}
			printf("numDelivered:%d\n", numDelivered);
			printf("CurMsgNum: %d\n", CurMsgNum);
		}
	}
	pthread_exit((void *)0);
}

/*
	do_client:
		client thread for receiving
*/
void *do_client(void *arg)
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
	pthread_t *tid = malloc( PROC_COUNT* sizeof(pthread_t) );
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
	process_id = (int)(*argv[1])-48;

	if((rv = getaddrinfo(NULL, PORTS[process_id], &hints, &servinfo)) != 0){
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

	for(i = 0; i < PROC_COUNT;i++){
		pthread_create(&tid[i], NULL, setupConnection,(void*)i);
	}

	for(i = 0; i < PROC_COUNT; i++){
		pthread_join( tid[i], NULL);
	}

	pthread_create(&chld_thr1, NULL, stdin_read, NULL);

	//initialize key variables
	for(i = 0; i < 26; i++){
		keys[i] = 0;
	}

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
		pthread_create(&chld_thr, NULL, do_client, (void *)new_fd);
	}

	for(i = 0; i < PROC_COUNT; i++){
		free(tid[i]);
	}

	return 0;
}