/* server.c

   Sample code of 
   Assignment L1: Simple multi-threaded key-value server
   for the course MYY601 Operating Systems, University of Ioannina 

   (c) S. Anastasiadis, G. Kappes 2016

*/
#include <stdbool.h>
#include <sys/time.h>

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <unistd.h>

#include <signal.h>
#include <sys/stat.h>
#include "utils.h"
#include "kissdb.h"

#define MY_PORT                 6767
#define BUF_SIZE                1160
#define KEY_SIZE                 128
#define HASH_SIZE               1024
#define VALUE_SIZE              1024
#define MAX_PENDING_CONNECTIONS   10
#define NTHREADS				  10
#define QUEUE_SIZE                 3

// Definition of the operation type.
typedef enum operation {
  PUT,
  GET
} Operation; 

// Definition of the request.
typedef struct request {
  Operation operation;
  char key[KEY_SIZE];  
  char value[VALUE_SIZE];
} Request;

struct descriptor{
	struct timeval startTime;
	int connfd;
};

time_t begin,end;
time_t service_time,waiting_time,total_waiting_time,total_service_time,average_waiting_time,average_service_time;


int head = -1;
int tail = -1; 
int size = 0;
bool non_empty_queue=false;
bool non_full_queue=true;
bool flag = true;

int writer_count= 0;
int reader_count = 0;

int completed_requests = 0;

struct timeval start,END,wait1,wait2;

struct descriptor q[QUEUE_SIZE];

pthread_t tid[NTHREADS];
pthread_mutex_t mutex1,mutex2,write_mutex,read_mutex,readwrite_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  condition_var1,condition_var2,reader_cond,writer_cond = PTHREAD_COND_INITIALIZER;

void *thread_function(void *);

// Definition of the database.
KISSDB *db = NULL;

/**
 * @name parse_request - Parses a received message and generates a new request.
 * @param buffer: A pointer to the received message.
 *
 * @return Initialized request on Success. NULL on Error.
 */
Request *parse_request(char *buffer) {
  char *token = NULL;
  Request *req = NULL;
  
  // Check arguments.
  if (!buffer)
    return NULL;
  
  // Prepare the request.
  req = (Request *) malloc(sizeof(Request));
  memset(req->key, 0, KEY_SIZE);
  memset(req->value, 0, VALUE_SIZE);

  // Extract the operation type.
  token = strtok(buffer, ":");    
  if (!strcmp(token, "PUT")) {
    req->operation = PUT;
  } else if (!strcmp(token, "GET")) {
    req->operation = GET;
  } else {
    free(req);
    return NULL;
  }
  
  // Extract the key.
  token = strtok(NULL, ":");
  if (token) {
    strncpy(req->key, token, KEY_SIZE);
  } else {
    free(req);
    return NULL;
  }
  
  // Extract the value.
  token = strtok(NULL, ":");
  if (token) {
    strncpy(req->value, token, VALUE_SIZE);
  } else if (req->operation == PUT) {
    free(req);
    return NULL;
  }
  return req;
}

/*
 * @name process_request - Process a client request.
 * @param socket_fd: The accept descriptor.
 *
 * @return
 */
void process_request(const int socket_fd) {
  char response_str[BUF_SIZE], request_str[BUF_SIZE];
    int numbytes = 0;
    Request *request = NULL;

    // Clean buffers.
    memset(response_str, 0, BUF_SIZE);
    memset(request_str, 0, BUF_SIZE);
    
    // receive message.
    numbytes = read_str_from_socket(socket_fd, request_str, BUF_SIZE);
    
    // parse the request.
    if (numbytes) {
      request = parse_request(request_str);
      if (request) {
        switch (request->operation) {
         case GET:
		    pthread_mutex_lock(&readwrite_mutex);
		    while(writer_count>0){
				pthread_cond_wait(&reader_cond,&readwrite_mutex);
			}
			reader_count++;
			pthread_mutex_unlock(&readwrite_mutex);
			
            // Read the given key from the database.
            if (KISSDB_get(db, request->key, request->value))
              sprintf(response_str, "GET ERROR\n");
            else
              sprintf(response_str, "GET OK: %s\n", request->value);
		  
			pthread_mutex_lock(&readwrite_mutex);
			reader_count--;
			if(reader_count==0)
				pthread_cond_signal(&writer_cond);
			pthread_mutex_unlock(&readwrite_mutex);
            break;
			
          case PUT:
			pthread_mutex_lock(&readwrite_mutex);
			while(writer_count>0||reader_count>0){
				pthread_cond_wait(&writer_cond,&readwrite_mutex);
			}
			writer_count = 1;
            // Write the given key/value pair to the database.
            if (KISSDB_put(db, request->key, request->value)) 
              sprintf(response_str, "PUT ERROR\n");
            else
              sprintf(response_str, "PUT OK\n");
	
			writer_count--;
			
			pthread_cond_signal(&writer_cond);
			pthread_cond_broadcast(&reader_cond);
			pthread_mutex_unlock(&readwrite_mutex);
			
			break;
          default:
            // Unsupported operation.
            sprintf(response_str, "UNKOWN OPERATION\n");

        }
        // Reply to the client.
        write_str_to_socket(socket_fd, response_str, strlen(response_str));
        if (request)
          free(request);
        request = NULL;
        return;
      }
    }
    // Send an Error reply to the client.
    sprintf(response_str, "FORMAT ERROR\n");
    write_str_to_socket(socket_fd, response_str, strlen(response_str));
}

void sighandler(int sig){
	signal(SIGTSTP, sighandler);
	flag=false;
	pthread_cond_broadcast(&condition_var1);
}

/*
 * @name main - The main routine.
 *
 * @return 0 on success, 1 on error.
 */
int main() {

  int socket_fd,              // listen on this socket for new connections
      new_fd;                 // use this socket to service a new connection
  socklen_t clen;
  struct sockaddr_in server_addr,  // my address information
                     client_addr;  // connector's address information

  // create socket
  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    ERROR("socket()");

  // Ignore the SIGPIPE signal in order to not crash when a
  // client closes the connection unexpectedly.
  signal(SIGPIPE, SIG_IGN);
  
  // create socket adress of server (type, IP-adress and port number)
  bzero(&server_addr, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);    // any local interface
  server_addr.sin_port = htons(MY_PORT);
  
  // bind socket to address
  if (bind(socket_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1)
    ERROR("bind()");
  
  // start listening to socket for incomming connections
  listen(socket_fd, MAX_PENDING_CONNECTIONS);
  fprintf(stderr, "(Info) main: Listening for new connections on port %d ...\n", MY_PORT);
  clen = sizeof(client_addr);

  // Allocate memory for the database.
  if (!(db = (KISSDB *)malloc(sizeof(KISSDB)))) {
    fprintf(stderr, "(Error) main: Cannot allocate memory for the database.\n");
    return 1;
  }
  
  // Open the database.
  if (KISSDB_open(db, "mydb.db", KISSDB_OPEN_MODE_RWCREAT, HASH_SIZE, KEY_SIZE, VALUE_SIZE)) {
    fprintf(stderr, "(Error) main: Cannot open the database.\n");
    return 1;
  }
  signal(SIGTSTP, sighandler);
  time_t now;
  struct tm * timeinfo;
  int i,j;
  for(i=0; i < NTHREADS; i++){
  	pthread_create( &tid[i], NULL, thread_function, NULL );
  }
  struct sigaction sact;
  sact.sa_handler = sighandler; //our handler to catch CTRL-Z
  sigemptyset(&sact.sa_mask);
  sact.sa_flags=0;
  if(sigaction(SIGTSTP,&sact,NULL)<0)
  	perror("could not set action for SIGSTP");
  // main loop: wait for new connection/requests
  while (flag) { 
    // wait for incomming connection
    if ((new_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &clen)) == -1) {
	  if(!flag){
		break;
	  }else{
		  ERROR("accept()");
	  }
    }
	struct descriptor reque;
	time(&now);
	timeinfo = localtime(&now);
	pthread_mutex_lock( &mutex1 );
	while((head == tail + 1) || (head == 0 && tail == QUEUE_SIZE-1)){
		pthread_cond_wait(&condition_var2,&mutex1);
	}
	if(head==-1){
		head = 0;
	}
	tail = (tail + 1)%QUEUE_SIZE;
    // got connection, serve request
    fprintf(stderr, "(Info) main: Got connection from '%s'\n", inet_ntoa(client_addr.sin_addr));
	//struct descriptor reque;
	reque.connfd = new_fd;
	gettimeofday(&wait1, NULL);
	reque.startTime = wait1;
	q[tail] = reque;
	size = size + 1;
	printf("size is %d\n",size);
	pthread_cond_signal(&condition_var1);
	pthread_mutex_unlock( &mutex1 );
  } 
  for(j=0;j<NTHREADS;j++){
 	pthread_join(tid[j],NULL);
  }
  // Destroy the database.
  // Close the database.
  KISSDB_close(db);

  // Free memory.
  if (db)
    free(db);
  db = NULL;
  //printf("13\n");
  printf("the completed_requests are %d\n",completed_requests);
  printf("the total waiting time was: %lf\n", (double)total_waiting_time/(double)1000000);
  printf("the total service time was %lf\n", (double)total_service_time/(double)1000000); //microsecond
  average_waiting_time = (double)total_waiting_time/(double)completed_requests;
  average_service_time = (double)total_service_time/(double)completed_requests;
  printf("the total average waiting time was: %lf\n", (double)average_waiting_time/(double)1000000);
  printf("the total average service time was %lf\n", (double)average_service_time/(double)1000000); //microsecond
  return 0; 
}
void *thread_function(void *dummyPtr){
	int fd;
	struct timeval dok;
	while(flag){
		pthread_mutex_lock(&mutex1);
		while(head==-1){
			if(!flag){
				break;
			}
			pthread_cond_wait(&condition_var1,&mutex1);
		}
		if(!flag){
			break;
		}
		printf("head is %d\n",head);
		size = size - 1;
		fd = q[head].connfd;
		dok = q[head].startTime;
		if(head==tail){
			head = -1;
			tail = -1;
		}else{
			head = (head + 1) % QUEUE_SIZE;
		}
		gettimeofday(&wait2, NULL);
		pthread_cond_signal(&condition_var2);
		pthread_mutex_unlock(&mutex1);
		gettimeofday(&start, NULL);
		process_request(fd);
		gettimeofday(&END, NULL);
		completed_requests ++;
		close(fd);
		end = time(NULL);
		waiting_time = (wait2.tv_sec - dok.tv_sec) * 1000000 + wait2.tv_usec - dok.tv_usec;
		service_time = (END.tv_sec - start.tv_sec) * 1000000 + END.tv_usec - start.tv_usec;
		printf("the waiting time was: %lf\n", (double)waiting_time/(double)1000000);
		printf("the service time was %lf us\n", (double)service_time/(double)1000000); //microsecond
		total_waiting_time += waiting_time;
		total_service_time += service_time;
	}
	pthread_exit(NULL);
	return 0;
}

