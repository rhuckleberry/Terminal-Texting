#include <assert.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <semaphore.h>

#include <sys/utsname.h>

#include "csapp.h"



#define MAXCLIENTS 16
#define VERBOSE 0
#define SIZE 100 //initial readline size

/*
 * A struct to hold connection fd  and client socket.
 */
typedef struct
{
	int connfd;
	struct sockaddr_in clientaddr;
	socklen_t clientlen;
} client_info_t;

/*
* Doubly Linked List for our list of clients
*/
struct client
{
    client_info_t *cinfop; //Current client
    char *username; //Client username
    struct client *prev;    //Previous client
    struct client *next;    //Next client
};

/*
* Struct to pass client info to client_receive thread
*/
struct client_receive_param
{
    int hostfd;
    char *filename;
};

sem_t *mutex; //client array mutex lock
char *sem_name = "/clientArray";
struct client *client_arr; //global client array

//Client array methods
struct client * init_client_sentinel();
struct client * init_client(client_info_t *cinfop);
bool            is_clientarr_empty(struct client *clientp);
bool            is_clientarr_one(struct client *clientp);
void            insert_client(struct client *clientarr_p, struct client *new_client);
struct client * loop_remove_client(struct client *client_iter);
int             remove_client(struct client *clientarr_remove);
int             remove_client_sentinel(struct client *sentinel);
void            print_client_array(struct client *sentinel);

//Server and Client Functionality
int     main(int argc, char **argv);
void    main_client(char *username, char *filename, char *port);
void    connecting_client(char *username, char *filename, char *host, char *port);
void *  client_receive(void *args);
void *  server_connect(void *args);
void *  server_process_messages(void *args);
void 		handler_cleanup(int signum);
char *  readline(int fd);
void    broadcast(char *message);
char *  gethost();

int
main(int argc, char **argv)
{
    char *filename, *username, *host, *port;

    //Parse input: username, host, port
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "main client usage: %s <username> <filename> <port>\n", argv[0]);
        fprintf(stderr, "connecting client usage: %s <username> <filename> <host> <port>\n", argv[0]);
		exit(1);
    }
    username = argv[1];
    filename = argv[2];

    //1. Connecting client: username + filename + host + port
    if (argc == 5) {
        host = argv[3];
        port = argv[4];
        connecting_client(username, filename, host, port);
    }

    //2. Main client (client + server): username + file
    if (argc == 3) {
        port = "80"; //default port if not included
        main_client(username, filename, port);
    }

    if (argc == 4) {
        port = argv[3]; //provided port value
        main_client(username, filename, port);
    }

    return (0);
}



void
main_client(char *username, char *filename, char *port)
{
    /* Initialize client array */
    client_arr = init_client_sentinel();

    /* Initialize Mutex */
		int errno;
    if ((mutex = sem_open(sem_name, O_CREAT, S_IRWXU, 1)) == SEM_FAILED) {
			printf("[ERROR] Semaphore Open Failed: %d", errno);
			exit(1);
		}

		/* Cleanup sigaction for ctrl+c canceling */
		struct sigaction action;
		action.sa_handler = handler_cleanup;
		action.sa_flags = SA_RESTART;
		Sigemptyset(&action.sa_mask);
		if (sigaction(SIGINT, &action, NULL) < 0) {
			unix_error("[ERROR] sigaction Failed");
		}

    /* Set up server accepting connections */
    int listen_fd = Open_listenfd(port); // Open listener for clients.
    pthread_t tid;
    Pthread_create(&tid, NULL, server_connect, (void *) &listen_fd);

    /* Set up server processing messages */
    Pthread_create(&tid, NULL, server_process_messages, NULL);

    /* Connect main client to the server */
    char *host = gethost();
		if (VERBOSE)
			printf("Host: %s\n", host);
    connecting_client(username, filename, host, port);
    Free(host);
}


void *
server_process_messages(void *args)
{
    (void) args;

    //Select
    int maxnfd;
    fd_set read_set;
    int fd;
    struct client *client_iter;
    char *message;
    struct timeval tv;

    if (VERBOSE)
        printf("Process Begin\n");


    /* Initialize Select Timeval */
    tv.tv_sec = 3; //should be relatively minimal number of connections (don't want to restart too often?)
    tv.tv_usec = 0;

    while (true) {
        //1. Initialize Select
        FD_ZERO(&read_set);
        maxnfd = 0;

        if (VERBOSE)
            printf("Initialize Select\n");

				//TODO: Thread-safe exit

        //mutex lock!!
				int errno;
        if (sem_wait(mutex) == -1) {
					printf("[ERROR] Semaphore Wait Failed: %d", errno);
					exit(1);
				}

        for (client_iter = client_arr->next; client_iter != client_arr; client_iter = client_iter->next) {
            fd = client_iter->cinfop->connfd;
            FD_SET(fd, &read_set); //Read set changes over time
            maxnfd = maxnfd > fd ? maxnfd : fd; // max function

            if (VERBOSE)
                printf("Read Set fd: %d\n", fd);
        }

        if (sem_post(mutex) == -1) {
					printf("[ERROR] Semaphore Post Failed: %d", errno);
					exit(1);
				}

        if (VERBOSE)
            printf("Read Set maxnfd: %d\n", maxnfd);

        //TODO: SOLVE ISSUE OF STALLING FOREVER WHEN SHOULD RESET WITH NEW CONNECTION ADDED
        //CURRENT: Added a 1 second timer to select
        //LATER: Do a "self-pipe" trick. Will learn this next year :)
        //https://stackoverflow.com/questions/384391/how-to-signal-select-to-return-immediately

        //2. Select to read messages sent to server
        select(maxnfd+1, &read_set, NULL, NULL, &tv);

        if (VERBOSE)
                    printf("Selected\n");



        //3. Send messages received to all connections (delete connection if disconnected)

        //mutex lock!!
				int errno;
        if (sem_wait(mutex) == -1) {
					printf("[ERROR] Semaphore Wait Failed: %d", errno);
					exit(1);
				}

        for (client_iter = client_arr->next; client_iter != client_arr; client_iter = client_iter->next) {
             fd = client_iter->cinfop->connfd;
            if (FD_ISSET(fd, &read_set)) {
                // a) Read Message
                if ((message = readline(fd)) == NULL) {
                    printf("[ERROR] Message not newline completed: Client Disconnected!\n");

                    /* Remove Client from linked list */
                    client_iter = loop_remove_client(client_iter);
                    broadcast("<<<<< User Disconnected >>>>>\n");

                    if (VERBOSE)
                        printf("Client Removed from Linked List\n");

                    continue;
                }

                if (VERBOSE)
                    printf("Read Select Message: \'%s\'\n", message);

                // b) Broadcast Message to all other connected clients
                broadcast(message);
                Free(message);

            }
        }
				if (sem_post(mutex) == -1) {
					printf("[ERROR] Semaphore Post Failed: %d", errno);
					exit(1);
				}

    }


    return (NULL);
}


/*
* Requires: global variable listenfd be initialized
*/
void *
server_connect(void *args)
{
    client_info_t *client_p;

    /* Parse args */
    int listen_fd = *(int *)args;

    /* Detach thread */
    Pthread_detach(Pthread_self());

    //TODO: ADD MAX CLIENT SIZE FOR LINKED LIST

    while (true) {
        /* Create new client info struct. */
        client_p = (client_info_t *) Malloc(sizeof(client_info_t));
        client_p->clientlen = sizeof(client_p->clientaddr);
        client_p->connfd = Accept(listen_fd,
                    (struct sockaddr *) &client_p->clientaddr,
                    &client_p->clientlen
                );

        if (VERBOSE)
            printf("Connected\n");

        /* Add client to client array */
        struct client *new_client = init_client(client_p);

        //mutex lock!!
				int errno;
        if (sem_wait(mutex) == -1) {
					printf("[ERROR] Semaphore Wait Failed: %d", errno);
					exit(1);
				}

        insert_client(client_arr, new_client);

        //Send connect message
        broadcast("<<<<< User Connected >>>>>\n");
        if (sem_post(mutex) == -1) {
					printf("[ERROR] Semaphore Post Failed: %d", errno);
					exit(1);
				}

        if (VERBOSE)
            print_client_array(client_arr);


        if (VERBOSE)
            printf("Inserted\n");
    }

    Close(listen_fd);

    if (VERBOSE)
            printf("Closed listener\n");

    return (NULL);
}


void
connecting_client(char *username, char *filename, char *host, char *port)
{
    (void) username, (void) filename,(void) host, (void)port;

    int hostfd;
    //rio_t rioc;
    char usertag[strlen(username + 3)]; //colon + space
    char *message;
    ssize_t size;

    /* Connect to host */
		if (VERBOSE)
			printf("Host: '%s', port: '%s'\n", host, port);
    hostfd = Open_clientfd(host, port);

    /* Thread to Receive Server Response */
    pthread_t tid;
    struct client_receive_param args;
    args.hostfd = hostfd;
    args.filename = filename;
    Pthread_create(&tid, NULL, client_receive, (void *) &args);

    /* Initialize rio */
    //rio_readinitb(&rioc, hostfd);

    /* Username Tag */
    strcpy(usertag, username);
    usertag[strlen(username)] = ':';
    usertag[strlen(username)+1] = ' ';
    usertag[strlen(username)+2] = '\0';

    if (VERBOSE)
        printf("Usertag: \'%s\'\n", usertag);

    while(true) {
        /* Client Forms Message */
        printf("Text Message (Type \\Commands for commands): \n");
        if ((message = readline(0)) == NULL) {
            printf("[ERROR] Message is NULL\n");
            exit(1);
        }

        if(VERBOSE)
            printf("Message: %s\n", message);


        //[EXTRA] TODO: ADD MESSAGE MARKERS?


        /* Check if Client Requests Commands */
        if (!strncmp(message, "\\Commands", strlen("\\Commands"))) {
            printf("\nCommands:\n");
            printf("\\Anonymous - Request Anonymous Usertag\n");
            printf("\\Past - Request All Past Chat Messages\n");
            printf("\\Quit - Leave Text Chat\n\n");
            continue;
        }

        /* Check if Client Quit */
        //TODO: Client Quit Functionality

        /* Check if Client Requests Anonymity */
        if (!strncmp(message, "\\Anonymous", strlen("\\Anonymous"))) {
            printf("~You are now anymous to the text chat~\n\n");
            strcpy(usertag, "[????]: ");
            continue;
        }

        /* Check if Client Requests All Past Chat Messages */
        //TODO: Client Requests All Past Chat Messages Functionality

        /* Write Message to server */
        char write_msg[strlen(usertag) + strlen(message) + 1];
        strcpy(write_msg, usertag);
        strcat(write_msg, message);

        //send message (ignore SIGPIPE signals)
        if ((size = send(hostfd, write_msg, strlen(write_msg), MSG_NOSIGNAL)) == -1) {

            //Remove client because it disconnected
            if (errno == EPIPE) {
                printf("Host disconnected\n");
                exit(1);

            } else { //Other error occured...
                printf("[ERROR] Broadcast Failed: %d\n", errno);
                exit(1);
            }

        }

        if(VERBOSE)
            printf("Write Message: \'%s\'\n", write_msg);

        Free(message);
    }

    Close(hostfd);
}

/*
* Requires:
*   hostfd is a valid connection file
*
* Effects:
*   Received message from server and puts them into
*   given file for user to read.
*
*   Creates file if it is not already made. Appends
*   messages to file if its already created.
*/
void *
client_receive(void *args)
{
    FILE *client_file;
    char *message;
    int hostfd;
    char *filename;
    fd_set host_set;
    struct client_receive_param *recv_args;

    //Detach thread
    Pthread_detach(Pthread_self());

    //Parse args
    recv_args = (struct client_receive_param *) args;
    hostfd = recv_args->hostfd;
    filename = recv_args->filename;


    //Open receive file
    if ((client_file = fopen(filename, "a")) == NULL) {
        printf("[ERROR] Client File Failed to Open\n");
        exit(1);
    }

    if (VERBOSE)
        printf("Opened Client File\n");

    //Initialize select: only host set
    FD_ZERO(&host_set);
    FD_SET(hostfd, &host_set);

    while (true) {
        //No need to re-initialize host_set because select simply
        //blocks on it being valid to read

        //Select hostfd: stalls till hostfd is ready to read
        if (select(hostfd+1, &host_set, NULL, NULL, NULL) == 0) {
            printf("[ERROR] Select should stall till hostfd is ready???\n");
            exit(1);
        }

        if (VERBOSE)
            printf("Selected hostfd\n");

        //Read message sent from hostfd
        if ((message = readline(hostfd)) == NULL) {
            printf("[ERROR] Client Received Empty Message: Server Disconnected\n");

            //Server Disconnected Message
            if (fputs("<<<<< Server Disconnected >>>>>\n", client_file) < 0){
                printf("[ERROR] Write to Client File\n");
                exit(1);
            }
            /* Flush write to client file */
            fflush(client_file);

            exit(1);
        }

        if (VERBOSE)
            printf("Read hostfd message: '%s'\n", message);

        //Write message to client_file for user to see
        if (fputs(message, client_file) < 0){
            printf("[ERROR] Write to Client File\n");
            exit(1);
        }

        /* Flush write to client file */
        fflush(client_file);

        Free(message);

        if (VERBOSE)
            printf("Put message into client file: %s\n", filename);

    }

    return (NULL);
}


/*
* Requires:
*   names semaphore is initialized aka open-ed
*
* Effects:
*   Cleans up names semaphore before program terminates with ctrl+c
*/
void
handler_cleanup(int signum)
{
		if (signum == SIGINT) {
			int errno;

			//TODO: Make async-signal-safe??

			//Close semaphore
			if (sem_close(mutex) < 0) {
				printf("[ERROR] Failed to Close Semaphore: %d", errno);
			}

			//Unlink semaphore
			if (sem_unlink(sem_name) < 0) {
				printf("[ERROR] Failed to Unlink Semaphore: %d", errno);
			}

			//End program
			_exit(0);

		} else {
			printf("[ERROR] Received Unexpected Signal: %d", signum);
		}
}

/*
* Requires:
*   client_arr is defined and initialized
*   client_arr is mutex locked
*
* Effects:
*   Sends message to all connected client file descriptors
*   If file descriptor is unconnected, it removes it from client_arr
*/
void
broadcast(char *message)
{
    struct client *client_iter;
    int clientfd;
    int errno;
    ssize_t size;

    //iterate through all connected clients
    for (client_iter = client_arr->next; client_iter != client_arr; client_iter = client_iter->next) {
        clientfd = client_iter->cinfop->connfd; //get client fd

        //send message (ignore SIGPIPE signals)
        if ((size = send(clientfd, message, strlen(message), MSG_NOSIGNAL)) == -1) {

            //Remove client because it disconnected
            if (errno == EPIPE) {
                client_iter = loop_remove_client(client_iter);
                broadcast("<<<<< User Disconnected >>>>>\n");

                if (VERBOSE)
                    printf("Client %d disconnected\n", clientfd);

                continue;

            } else { //Other error occured...
                printf("[ERROR] Broadcast Failed: %d\n", errno);
                exit(1);
            }

        }

        if (VERBOSE)
            printf("Broadcast Message: '%s', Message Size: %zu, Size: %zu\n", message, strlen(message), size);
    }
}


/*
* Requires:
*   SIZE label is defined
*   File already has data written into it (use select function)
*
* Effects:
*   Reads null terminated memory allocated line from given file (including '\n').
*   Returns NULL if reaches end of file before reaches '\n' or errors
*/
char *
readline(int fd)
{
    uint size = SIZE;
    uint index = 0;
    char *message = Malloc(size);

    int retval;

    //Read line from file
    while(((retval = read(fd, &message[index++], 1)) == 1) //Read same as recv here. No crash on terminal null lines
            && (message[index-1] != '\n')){

        //Realloc if needed
        if (index >= size) {
            size <<= 1; //multiply by 2
            message = Realloc(message, size);
        }
    }

    if (retval < 1) { //EOF or error before newline
        Free(message);
        return (NULL);
    }

    message[index++] = '\0';

    //Fit exact message size
    message = Realloc(message, index);

    return (message);
}


/*
* Effects:
*   Returns host name of current device.
*/
char *
gethost()
{
    char hostname[MAXLINE];
    int errno;
    if (gethostname(hostname, MAXLINE) == -1) {
        printf("[ERROR] Failed to get host name: %d\n", errno);
        exit(1);
    }

    char *host = Malloc(sizeof(strlen(hostname) + 1));
    host = strcpy(host, hostname);

    return (host);
}







/*
* Effects:
*   Returns pointer to memory allocated sentinel - start of
*   client circular linked list
*/
struct client *
init_client_sentinel()
{
    struct client *sentinel = malloc(sizeof(struct client));
    sentinel->cinfop = NULL;
    sentinel->prev = sentinel;
    sentinel->next = sentinel;
    return(sentinel);
}


/*
* Requires:
*   Input cinfop is not a NULL pointer.
*
* Effects:
*   Returns pointer to memory allocated client with given cinfop dat.
*   Returns NULL pointer if input cinfop is a NULL pointer (use sentinel!)
*/
struct client *
init_client(client_info_t *cinfop)
{
    if (cinfop == NULL)
        return (NULL);

    struct client *clientp = malloc(sizeof(struct client));
    clientp->cinfop = cinfop;
    clientp->prev = clientp;
    clientp->next = clientp;
    return (clientp);
}


/*
* Requires:
*   Input should be sentinel client array start
*
* Effects:
*   Returns true if array is empty; false otherwise.
*/
bool
is_clientarr_empty(struct client *sentinel)
{
    return (sentinel->cinfop == NULL &&
            sentinel->prev == sentinel &&
            sentinel->next == sentinel);
}

/*
* Requires:
*   Input should be sentinel client array start
*
* Effects:
*   Returns true if array has exactly one element; false otherwise.
*/
bool
is_clientarr_one(struct client *sentinel)
{
    return (sentinel->cinfop != NULL &&
            sentinel->prev != sentinel &&
            sentinel->prev == sentinel->next);
}


/*
* Requires:
*   Array should be mutex locked!
*   Input should be sentinel client array start
*
* Effects:
*   Inserts given client into sentinel
*/
void
insert_client(struct client *sentinel, struct client *new_client)
{
    if (is_clientarr_empty(sentinel)) { //Empty Array
        sentinel->prev = new_client;
        sentinel->next = new_client;
        new_client->prev = sentinel;
        new_client->next = sentinel;

    } else { //More than one element
        new_client->prev = sentinel;
        new_client->next = sentinel->next;
        sentinel->next = new_client;
        new_client->next->prev = new_client;
    }

}

/*
* Requires:
*   Array should be mutex locked!
*   Array should be empty (only sentinel)
*
* Effects:
*   Removes client sentinel.
*   Returns error -1 if array not empty
*/
int
remove_client_sentinel(struct client *sentinel)
{
    //Error if array not empty
    if (!is_clientarr_empty(sentinel)) {
        return (-1);
    }

    Free(sentinel->cinfop);
    Free(sentinel);

    return (0);
}


/*
* Requires:
*   Array should be mutex locked!
*   Array should not be empty (only sentinel)
*
* Effects:
*   Removes given client from client array. Preserves loop iteration
*   Returns previous node to preserve loop iteration.
*   Crashes process if linked list is empty.
*/
struct client *
loop_remove_client(struct client *client_iter)
{
    struct client *client_prev = client_iter->prev;
    if (remove_client(client_iter) == -1){
        printf("[ERROR] Removed from empty linked list\n");
        exit(1);
    }
    //TODO: Put user disconnect broadcast in here?
    return(client_prev); //move iter to prev to preserve for-loop (current node removed)
}


/*
* Requires:
*   Array should be mutex locked!
*   Array should not be empty (only sentinel)
*
* Effects:
*   Removes given client from clientarr_p
*   Returns error -1 if array is empty
*/
int
remove_client(struct client *clientarr_remove)
{
    //Error if array has no elements
    if (is_clientarr_empty(clientarr_remove)) {
        return (-1);
    }

    clientarr_remove->prev->next = clientarr_remove->next;
    clientarr_remove->next->prev = clientarr_remove->prev;
    Free(clientarr_remove->cinfop);
    Free(clientarr_remove);

    return (0);
}

/*
* Requires:
*   Mutex lock names mutex is initialized
*   Input has to be sentinel start of client array
*
* Effects:
*   Prints client array
*/
void
print_client_array(struct client *sentinel)
{
		int errno;
		if (sem_wait(mutex) == -1) {
			printf("[ERROR] Semaphore Wait Failed: %d", errno);
			exit(1);
		}

    struct client *client_iter;

    printf("\n=========\n");
    printf("Sentinel: %p\n", sentinel->cinfop);
    for (client_iter = sentinel->next; client_iter != sentinel; client_iter = client_iter->next) {
        printf("Client Connfd: %d\n", client_iter->cinfop->connfd);
    }
    printf("=========\n\n");

		if (sem_post(mutex) == -1) {
			printf("[ERROR] Semaphore Post Failed: %d", errno);
			exit(1);
		}
}
