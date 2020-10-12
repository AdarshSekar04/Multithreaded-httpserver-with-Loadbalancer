#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <err.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h> //used for pthread_cond_timedwait()
#include <errno.h>
#include <stdbool.h>

#define HEALTHCHECK "GET /healthcheck HTTP/1.1\r\n\r\n"
#define INTERNALERROR "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"

struct workerThread{
    pthread_t worker_id;
    int id;
    pthread_mutex_t* lock; //Lock to get cfd
    pthread_mutex_t* dlock;
    pthread_mutex_t* rlock;//Lock to increment the number of requests
    pthread_mutex_t* hlock; //lock for healthcheck
    uint16_t* numreq; //Pointer to the number of requests
    uint16_t recheck; //Number of requests at which we call a healthcheck
    uint16_t* portarr; //Array storing all the server ports
    uint16_t numports; //Gives the number of ports, as it may nto equal array length
    uint64_t* serversCount; //This will be a pointer to a 2D array of servers, where each index will refer to the count of the requests to the servers
    int cfd;
    int* free;
    pthread_cond_t cond_var;
    pthread_cond_t* healthsig; //Condition variable used to signal the health check thread to wake up
    pthread_cond_t* workersig; //Condition variable I will use for the health check to signal the worker threads that it is done with the healthcheck
    pthread_cond_t* dissignal;
    bool* healthcheck; //A pointer to a bool so that worker threads can check if there is a health check currently happening, and wait for it to be over before continuing to process requests
};

//seperate struct for the healthcheck thread
struct healthThread{
    pthread_t health_id;
    pthread_mutex_t* hlock; //lock for the health thread
    pthread_cond_t* healthsig; //condition variable we will use to signal the health thread
    pthread_cond_t* workersig; //Condition variable we will use to signal the worker thread to wake up.
    uint64_t* servers;
    uint16_t* portarr; //Array storing all the server ports
    uint16_t numports; //Gives the number of ports, as it may nto equal array length
    uint16_t* numreq; //Pointer to the number of requests so we can reset it
    bool* healthcheck; //bool we have a pointer to to let worker threads know there is a healthcheck going on

};
/*
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * returns: valid socket if successful, -1 otherwise
 */
int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0)
        return -1;
    return connfd;
}

/*
 * server_listen takes a port number and creates a socket to listen on 
 * that port.
 * port: the port number to receive connections
 * returns: valid socket if successful, -1 otherwise
 */
int server_listen(int port) {
    int listenfd;
    int enable = 1;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        return -1;
    if (bind(listenfd, (struct sockaddr*) &servaddr, sizeof servaddr) < 0)
        return -1;
    if (listen(listenfd, 500) < 0)
        return -1;
    return listenfd;
}

/*
 * bridge_connections send up to 100 bytes from fromfd to tofd
 * fromfd, tofd: valid sockets
 * returns: number of bytes sent, 0 if connection closed, -1 on error
 */
int bridge_connections(int fromfd, int tofd) {
    char recvline[4096];
    int n = 0;
    //while((n = recv(fromfd, recvline, 4096, 0))){
	n = recv(fromfd, recvline, 4096, 0);
	if (n < 0) {
	    perror("connection error receiving\n");
	    return -1;
	} else if (n == 0) {
	    printf("receiving connection ended\n");
	    return 0;
	}
	recvline[n] = '\0';
	//printf("%s", recvline);;
	n = send(tofd, recvline, n, 0);
	if (n < 0) {
	    printf("connection error sending\n");
	    return -1;
	} else if (n == 0) {
	    printf("sending connection ended\n");
	    return 0;
	}
	//}
    return n;
}

/*
 * bridge_loop forwards all messages between both sockets until the connection
 * is interrupted. It also prints a message if both channels are idle.
 * sockfd1, sockfd2: valid sockets
 */
int bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;
	//int i = 0;
    int fromfd, tofd;
    bool conngoing = false;
   	bool conncoming = false;
    //We shoould break out of the loop if the timeout expires, that is if the case is 0.
    //So, I'll have variables ready for that, so that we break out of the loop
    while(1) {
    	//printf("Reading %d\n", i);
		//i++;
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, 5 seconds, 0 microseconds
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                printf("error during select, exiting\n");
                return -1;
            case 0:
            	if (conngoing == true && conncoming == false){
            		//We send the INTERNALERROR to the client, as we're taking too long to send data
            		dprintf(sockfd1, INTERNALERROR);
            		return -1;
            	}
            	else if (conncoming == true){
            		//If there's nothing else to recv, then we return 0 to show a successful connection
            		if (bridge_connections(fromfd, tofd) == 0){
            			return 0;
            		}
            		//Otherwise, we wait for the message to come as we're already getting the response
            		//dprintf(sockfd1, INTERNALERROR);
            		//return -1;
            		continue;
            	}
                printf("both channels are idle, waiting again\n");
                return 0;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                	//from client to server
                    fromfd = sockfd1;
                    tofd = sockfd2;
                    conngoing = true;
                } else if (FD_ISSET(sockfd2, &set)) {
                	//from server to client
                    fromfd = sockfd2;
                    tofd = sockfd1;
                    conncoming = true;
                } else {
                    printf("this should be unreachable\n");
                    return -1;
                }
        }
        int n = bridge_connections(fromfd, tofd);
        if (n <= 0){
        	//Will have to return an INTERNALERROR message
        	if (n < 0){
        		dprintf(sockfd1, INTERNALERROR);
        		return -1;
        	}
            return 0;
        }
    }
}

//Function that will return the server to use
int bestServer(uint64_t* serverCount, uint16_t numports){
    //Assume the server at the 0'th index is the best
    int best = -1;
    //printf("Numports = %d\n", numports);
    //For all the servers
    for(int i = 0; i < numports; i++){
    	//printf("Entered for loop\n");
        //if the port is invalid this round, we skip it
        printf("Server = %d, Num Request = %ld, Num Errors = %ld, Validity = %ld\n", i, *(serverCount+i*3), *(serverCount +i*3 +1), *(serverCount +i*3 +2));
        if (*(serverCount +i*3 +2) == 1){
            continue;
        }
        //if the best is -1, then we set it to the first valid port we find
        if (best == -1){
            best = i;
            continue;
        }
        //Otherwise, we check if it is better than our best port
        if (*(serverCount +i*3 +0) < *(serverCount +best*3 +0)){
            best = i;
        }
        //if the new port has the same number of requests, we check if the errors are less
        else if (*(serverCount +i*3 +0) == *(serverCount +best*3 +0) && *(serverCount +i*3 +1) < *(serverCount +best*3 + 1)){
            best = i;
        }
        //Otherwise, we don't change best
    }
    //Now, if best is still -1, that means that none of the servers were good, so we exit failure
    //If best isn't -1, I return it
    return best;
}

void* worker(void* data){
    struct workerThread* this_thread = (struct workerThread*) data;
    while (true){
        //printf("Worker thread %d is ready for instruction\n", this_thread->id);
        //Have to lock the mutex before waiting
        int error = pthread_mutex_lock(this_thread->lock);
        if (error){
            perror("Error with pthread_mutex_lock");
            //exit(EXIT_FAILURE);
            continue;
        }
        //If we've done the specified number of requests between healthchecks (this_thread->recheck), we signal the health thread, and set healthcheck to true
        if (*this_thread->numreq == this_thread->recheck && this_thread->recheck > 0){
        	*this_thread->healthcheck = true;
        	error = pthread_cond_signal(this_thread->healthsig);
        	if (error){
        		perror("Error with pthread_cond_signal()");
        		exit(EXIT_FAILURE);
        	}
        	//*this_thread->healthcheck = true;
        }
        //if there is a healthcheck going on
        while(*this_thread->healthcheck == true){      	
        //we put the current thread to sleep until the healthcheck is done
        	error = pthread_cond_wait(this_thread->workersig, this_thread->hlock);
        	if (error){
        		perror("Error with pthread_cond_wait()\n");
        		exit(EXIT_FAILURE);
        	}
        }
        //printf("Got here\n");
        //Get the server socket descriptor
        //printf("Thread %d has entered critical region\n", this_thread->id);
        *this_thread->free = this_thread->id;
        error = pthread_cond_signal(this_thread->dissignal);
        if (error){
            perror("Error with pthread_cond_signal");
            exit(EXIT_FAILURE);
        }
        //Now we lock dispatcher mutex
        while(this_thread->cfd < 0){
            //printf("Thread %d waiting to be woken up\n", this_thread->id);
            //wait on our condition variable
            error = pthread_cond_wait(&this_thread->cond_var, this_thread->dlock);
            if (error){
                perror("Problem with pthread_cond_wait\n");
                exit(EXIT_FAILURE);
            }
        }
        *this_thread->free = -1;
        *(this_thread->numreq) += 1;
        int index = bestServer(this_thread->serversCount, this_thread->numports);
        if(index == -1){
        	//put thread to sleep
        	//error = pthread_cond_wait(&this_thread->cond_var, this_thread->dlock);
        	error = pthread_mutex_unlock(this_thread->lock);
		    if(error){
		        perror("Error with pthread_mutex_unlock\n");
		        exit(EXIT_FAILURE);
		    }
		    printf("Sent here\n");
		    //index = bestServer(this_thread->serversCount, this_thread->numports);
		    dprintf(this_thread->cfd, INTERNALERROR);
        	this_thread->cfd = -1;
		    continue;
        }
        uint16_t server = this_thread->portarr[index];
        printf("Thread %d using server %d\n", this_thread->id, server);
        *(this_thread->serversCount +index*3) += 1;
        //Unlock the mutex as we've exited out critical section
        //printf("Thread %d connected to socket %d\n", this_thread->id, this_thread->cfd);
        error = pthread_mutex_unlock(this_thread->lock);
        if(error){
            perror("Error with pthread_mutex_unlock\n");
            exit(EXIT_FAILURE);
        }
        int servfd = client_connect(server);
        if (servfd < 0){
        	printf("Error here\n");
        	//Want to send an internal server error response
        	dprintf(this_thread->cfd, INTERNALERROR);
        	*(this_thread->serversCount + index*3 +2) = 1;
        	this_thread->cfd = -1;
        	continue;
        }
        //bridges the client and the server
        printf("About to connect client and server\n");
        int bad = bridge_loop(this_thread->cfd, servfd);
        error = pthread_mutex_lock(this_thread->rlock);
        if (error){
            perror("Error with pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        //If the connection was not completed successfully, then we increment the errors for that server
        if (bad < 0){
        	*(this_thread->serversCount +index*3 +1) += 1; 
        	*(this_thread->serversCount +index*3 +2) += 1; 
        }
        error = pthread_mutex_unlock(this_thread->rlock);
        if (error){
            perror("Error with pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }
        error = close(this_thread->cfd);
        if (error){
        	perror("Error closing client\n");
        	exit(EXIT_FAILURE);
        }
        error = close(servfd);
        if (error){
        	perror("Error closing server\n");
        	exit(EXIT_FAILURE);
        }
        printf("Done connecting client and server\n");
        this_thread->cfd = -1;
    }    
}

//Function for healthcheck
void* healthcheck(void* data){
    struct healthThread* health_thread = (struct healthThread*) data;
    struct timespec ts;
    struct timeval curr_time;
    int error = 0;
    while(true){
    	//printf("While in health thread\n");
        //First lock the mutex 
        //Now I can proceed to call healthcheck to each of the servers.
        //int numreq = 0;
        //int numerr = 0;
        //int best_port = -1;
        for (int i = 0; i < health_thread->numports; i++){
        	curr_time.tv_sec = 5;
        	curr_time.tv_usec = 0;
        	//printf("Entered for loop %d\n", i);
            int servfd = client_connect(health_thread->portarr[i]);
            //If the server is invalid, we mark that port as invalid, and continue
            if (servfd < 0){
                //Server is invalid
                *(health_thread->servers + i*3 + 2) = 1;
                printf("Errno at servfd = %d", errno);
                errno = 0;
                continue;
            }
            setsockopt(servfd, SOL_SOCKET, SO_RCVTIMEO, &curr_time, sizeof(curr_time));
            char buffer[4096];
            memset(&buffer, 0, 4096);
            //Send the healthcheck request to the server
            sprintf((char*)&buffer, "%s", HEALTHCHECK);
            error = send(servfd, &buffer, strlen(buffer), 0);
            if (error < 0){
            	//*(health_thread->servers + i*3 + 2) = 1;
            	printf("Errno = %d\n", errno);
            	perror("Problem with send\n");
            	continue;
            }
            int bytes_read = 0;
            int total_bytes= 0;
            //gettimeofday(&curr_time, NULL);
			//ts.tv_sec = curr_time.tv_sec + 2;
            while((bytes_read = recv(servfd, &buffer + total_bytes , 4096, 0))){
            	//printf("Entered while\n");
                if (bytes_read < 0){
                    perror("Problem with recv()\n");
                    //exit(EXIT_FAILURE); 
                    //Mark the server as invalid
                    *(health_thread->servers + i*3 + 2) = 1;
                    break;
                    
                }
                /*if (curr_time.tv_sec == ts.tv_sec){
                	*(health_thread->servers + i*3 + 2) = 1;
                	bytes_read = -1;
                }*/
                total_bytes += bytes_read;
            }
            //If there was an unsuccessful recv(), I simply continue to the next index in the for loop, as there is no point in continuing through the rest of the loop
            if (bytes_read < 0){
            	continue;
            }
            //Need to add timeout for recv
            //Now, we want to call sscanf on our buffer to find the data we want
            int tempreq = 0;
            int temperr = 0;
            //Dont care about content length, but we need to store it somewhere
            int clen = 0;
            error = sscanf((char*)&buffer, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%d\r\n%d", &clen, &temperr, &tempreq);
            //Now we check if i is 0, as that means we are only in our first iteration of the for loop
            if (error != 3){
                //Server is invalid
                *(health_thread->servers + i*3 + 2) = 1;
                continue;
            }
            //Change the value of num requests and num error of the server in heatlh_thread->servers
            *(health_thread->servers +i*3 +2) = 0;
            *(health_thread->servers +i*3 +1) = temperr;
            *(health_thread->servers +i*3 +0) = tempreq;
            close(servfd);
            printf("Errno at end = %d\n", errno);
        }
        //When we're done with the for loop, we should have the best server port in best_port
        //So, we set the workers server fd to that
        //if (best_port != -1){
        	//*health_thread->server = best_port;
        //}
        *health_thread->healthcheck = false;
        *health_thread->numreq = 0;
        //Now, I signal the worker thread to wake up
        error = pthread_cond_signal(health_thread->workersig);
        if (error){
        	perror("Error with pthread_cond_signal\n");
        	exit(EXIT_FAILURE);
        }
        //printf("Done with healthcheck\n");
        error = pthread_mutex_lock(health_thread->hlock);
        if (error){
            perror("Error with mutex locking\n");
            exit(EXIT_FAILURE);
        }
        //printf("error at mutex lock = %d\n", error);
        //Below lines of ocde are used to intialise the time for pthread_cond_timedwait
		memset(&ts, 0, sizeof(ts));
		gettimeofday(&curr_time, NULL);
		ts.tv_sec = curr_time.tv_sec + 2;
		//printf("Errno = %d\n", errno);
        //Set the thread to sleep for a certain time or until it is awoken by a worker thread
        error = pthread_cond_timedwait(health_thread->healthsig, health_thread->hlock, &ts);
        if (error){
        	//printf("error = %d\n", error);
        	if (errno != ETIMEDOUT && errno != 0){
        		printf("Errno = %d\n", errno);
        		perror("Problem with pthread_cond_timedwait()\n");
            	//exit(EXIT_FAILURE);
            	continue;
        	}	
        }
        printf("Done waiting in health thread\n");
        //Now, we will set health_thread->healthcheck to true, so that the worker threads know that they shouldn't process any new client requests
        *health_thread->healthcheck = true;
        error = pthread_mutex_unlock(health_thread->hlock);
        if (error){
        	perror("Problem with pthread_mutex_unlock()\n");
        	exit(EXIT_FAILURE);
        }
    }
}
int main(int argc,char **argv) {
    //int connfd, listenfd, acceptfd;
    int listenfd;
    //uint16_t connectport, listenport;
    if (argc < 3) {
        perror("missing arguments: need atleast a port to listen to, and a port to send requests to\n");
        return EXIT_FAILURE;
    }
    //array for ports
    uint16_t* ports = malloc((sizeof(uint16_t))*(argc -2));
    uint16_t lbport = 0;
    //int for the number of ports
    int count = 0;
    int numthreads = 4;
    int numrequests = 5;
    bool loadbalancer = false;
   	char c = 0;
   	//printf("Before for loop\n");
    //while((c = getopt(argc, argv, "N:l:")) != -1){
    for (int i = 0; i < argc; i++){
        c = getopt(argc, argv, "-:N:R:");
        //if we've parsed all the options, break
        if (c == -1){
            break;
        }
        //printf("Entered for\n");
        switch(c){
            case '\1':
                if(atoi(optarg)){
                    //If this is the first port number we see, it's the loadbalancers port
                    if (loadbalancer == false){
                        lbport = atoi(optarg);
                        loadbalancer = true;
                        break;
                    }
                    ports[count] = atoi(optarg);
                    count++;
                    break;
                }
                perror("Invalid argument\n");
                return EXIT_FAILURE;
                break;
            case 'N':
                //printf("%s\n", optarg);
                if (atoi(optarg)){
                	//printf("Entered if\n");
                    numthreads = atoi(optarg);
                    //printf("Numthread = %d\n", numthreads);
                    break;
                }
                perror("Invalid number of threads\n");
                return EXIT_FAILURE;
                break;
            case 'R':
                //printf("%s\n", optarg);
                if (atoi(optarg)){
                    numrequests = atoi(optarg);
                    break;
                }
                perror("Invalid number of threads\n");
                return EXIT_FAILURE;
                break;
            case '?':
                //printf("%s\n", argv[optind - 1]);
                /*if (optopt == 'N' || optopt == 'l'){
                    perror("Option -N and -l require an argument\n");
                    return EXIT_FAILURE;
                }*/
                perror("Invalid argument\n");
                return EXIT_FAILURE;
            case ':':
                if (optopt == 'N' || optopt == 'R'){
                    perror("Option -N and -l require an argument\n");
                    return EXIT_FAILURE;
                }
        }

    }
    //printf("Numthreads = %d\n", numthreads);
    //Make sure we got a load balancer port
    if (loadbalancer == false || lbport <= 1024){
        perror("No port or invalid port for loadbalancer provided\n");
        return EXIT_FAILURE;
    }
    //Make sure that we atleast got one port for the  
    if (ports[0] == 0){
        perror("No server port provided\n");
        return EXIT_FAILURE;
    }
    for (int j = 0; j < count; j++){
    	if (ports[j] <= 1024){
    		perror("Invalid port\n");
    		return EXIT_FAILURE;
    	}
    }
    if (numthreads <= 0){
    	perror("Invalid number of threads. Number of threads must be atleast 1\n");
    	return EXIT_FAILURE;
    }
    if (numrequests <= 0){
    	perror("Invalid number of requests. -R must be atleast 1");
    	return EXIT_FAILURE;
    }
    //Listen to the port
    listenfd = server_listen(lbport);
    if (listenfd < 0){
        perror("Failed listening\n");
        return EXIT_FAILURE;
    }
    //Initialising a 2D array for the servers
    uint64_t* servers = calloc(count * 3, sizeof(uint64_t));
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t dlock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
    struct workerThread workers[numthreads];
    int free_thread = -1;
    pthread_cond_t free = PTHREAD_COND_INITIALIZER;
    pthread_cond_t workersig = PTHREAD_COND_INITIALIZER; 
    //Initialise the healthcheck thread
    struct healthThread health_thread;
    //Set the clock of this condition variable to CLOCK_MONOTONIC
    //error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    //Initialise the condition variable with those attributes
    pthread_cond_t hsignal = PTHREAD_COND_INITIALIZER;
    //pthread_cond_init(&hsignal, &attr);
    health_thread.hlock = &hlock;
    health_thread.healthsig = &hsignal;
    health_thread.workersig = &workersig;
    health_thread.portarr = ports;
    health_thread.numports = count;
    uint16_t numreq = 0;
    health_thread.numreq = &numreq;
    bool health = true; //a variable that will let threads know if a healthcheck is currently occuring
    health_thread.healthcheck = &health;
    //uint16_t server = ports[0];
    health_thread.servers = servers;
    int error = pthread_create(&health_thread.health_id, NULL, healthcheck, (void *) &health_thread);
    //sleep(0.5);
    //Signal the healthcheck thread so it does the first healthcheck
    //*health_thread.healthcheck = true;
    //error = pthread_cond_signal(&hsignal);
    if (error){
    	perror("Problem with first health check\n");
    	return EXIT_FAILURE;
    }
    //sleep(0.5);
    //health thread should be initialised now
    //Now, we can start making our worker threads
    for(int i = 0; i < numthreads; i++){
        workers[i].cfd = -1;
        workers[i].cond_var = (pthread_cond_t) PTHREAD_COND_INITIALIZER;
        workers[i].dissignal = &free;
        workers[i].id = i;
        workers[i].free = &free_thread;
        workers[i].lock = &lock;
        workers[i].dlock = &dlock;
        workers[i].rlock = &rlock;
        workers[i].hlock = &hlock;
        workers[i].healthsig = &hsignal;
        workers[i].workersig = &workersig;
        workers[i].numreq = &numreq;
        workers[i].recheck = numrequests;
        workers[i].healthcheck = &health;
        workers[i].serversCount = servers;
        workers[i].portarr = ports;
        workers[i].numports = count;
        //printf("ERROR HERE\n");
        error = pthread_create(&workers[i].worker_id, NULL, worker, (void *) &workers[i]);
        if (error){
            printf("Error with pthread_create() in dispatcher\n");
            exit(EXIT_FAILURE);
        }
    }
    struct sockaddr client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    //Now, 
    while(true){
        printf("Waiting for client\n");
        int client_sockd = accept(listenfd, &client_addr, &client_addrlen);
        //If there was an error accepting the client connection, we simply continue
        if (client_sockd < 0){
            printf("Entered bad client_sockd. Errno = %d\n", errno);
            continue;
        }
        printf("Recieved client: %d\n", client_sockd);
        //While we don't have a free_thread, we make our dispatcher sleep on our condition variable free.
        error = pthread_mutex_lock(&dlock);
        if (error){
            perror("Error locking mutex\n");
            exit(EXIT_FAILURE);
        }
        while(free_thread < 0){
            pthread_cond_wait(&free, &dlock);
        }
        printf("Free_thread = %d\n", free_thread);
        //Now that we have a free thread, we set the free threads cfd to client_sockd
        workers[free_thread].cfd = client_sockd;
        //Then we signal the free thread, so it can wake up and parse the request
        pthread_cond_signal(&workers[free_thread].cond_var);
        error = pthread_mutex_unlock(&dlock);
        if (error){
            perror("Error unlocking mutex\n");
            exit(EXIT_FAILURE);
        }
    }
    // Remember to validate return values
    // You can fail tests for not validating
    /*connectport = atoi(argv[1]);
    listenport = atoi(argv[2]);
    if ((connfd = client_connect(connectport)) < 0)
        err(1, "failed connecting");
    if ((listenfd = server_listen(listenport)) < 0)
        err(1, "failed listening");
    if ((acceptfd = accept(listenfd, NULL, NULL)) < 0)
        err(1, "failed accepting");

    // This is a sample on how to bridge connections.
    // Modify as needed.
    bridge_loop(acceptfd, connfd);*/
    return EXIT_SUCCESS;
}
