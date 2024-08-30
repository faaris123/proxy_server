#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "proxyserver.h"
#include "safequeue.h"

/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000

/*
 * Global configuration variables.
 * Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int num_listener;
int *listener_ports;
int num_workers;
char *fileserver_ipaddr;
int fileserver_port;
int max_queue_size;

void send_error_response(int client_fd, status_code_t err_code, char *err_msg)
{
    http_start_response(client_fd, err_code);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    char *buf = malloc(strlen(err_msg) + 2);
    sprintf(buf, "%s\n", err_msg);
    http_send_string(client_fd, buf);
    return;
}

/*
 * forward the client request to the fileserver and
 * forward the fileserver response to the client
 */
void serve_request(struct QueueNode *job)
{
    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1)
    {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }

    // create the full fileserver address
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;
    fileserver_address.sin_port = htons(fileserver_port);

    // connect to the fileserver
    int connection_status = connect(fileserver_fd, (struct sockaddr *)&fileserver_address,
                                    sizeof(fileserver_address));
    if (connection_status < 0)
    {
        // failed to connect to the fileserver
        printf("Failed to connect to the file server\n");
        send_error_response(job->client_fd, BAD_GATEWAY, "Bad Gateway");
        return;
    }

    char *buffer = calloc(RESPONSE_BUFSIZE, sizeof(char));
    if (buffer == NULL)
    {
        printf("Failed to allocate data");
        return;
    }

    int ret = http_send_data(fileserver_fd, job->req_str, strlen(job->req_str));
    if (ret < 0)
    {
        printf("Failed to send request to the file server\n");
        send_error_response(job->client_fd, BAD_GATEWAY, "Bad Gateway");
    }
    else
    {
        // forward the fileserver response to the client
        while (1)
        {
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);
            if (bytes_read <= 0) // fileserver_fd has been closed, break
                break;
            ret = http_send_data(job->client_fd, buffer, bytes_read);
            if (ret < 0)
            { // write failed, client_fd has been closed
                break;
            }
        }
    }

    // close the connection to the fileserver
    shutdown(fileserver_fd, SHUT_WR);
    close(fileserver_fd);

    // Free resources and exit
    free(buffer);
}

struct ThreadArg
{
    int serv_fd;
    int port_id;
};

int *server_fds;
/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *server_fd, int port_id)
{

    // create a socket to listen
    *server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (*server_fd == -1)
    {
        perror("Failed to create a new socket");
        exit(errno);
    }
    server_fds[port_id] = *server_fd;

    // manipulate options for the socket
    int socket_option = 1;
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1)
    {
        perror("Failed to set socket options");
        exit(errno);
    }

    int proxy_port = listener_ports[port_id];
    // create the full address of this proxyserver
    struct sockaddr_in proxy_address;
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(proxy_port); // listening port

    // bind the socket to the address and port number specified in
    if (bind(*server_fd, (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) == -1)
    {
        perror("Failed to bind on socket");
        exit(errno);
    }

    // starts waiting for the client to request a connection
    if (listen(*server_fd, 1024) == -1)
    {
        perror("Failed to listen on socket");
        exit(errno);
    }

    printf("Listening on port %d...\n", proxy_port);

    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);
    int client_fd;
    while (1)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("L: Waiting for connection.\n");
#endif
        client_fd = accept(*server_fd,
                           (struct sockaddr *)&client_address,
                           (socklen_t *)&client_address_length);
        if (client_fd < 0)
        {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n",
               inet_ntoa(client_address.sin_addr),
               client_address.sin_port);

#ifdef ENABLE_DEBUG_PRINT
        printf("L: Parsing client request (fd = %d).\n", client_fd);
#endif
        struct QueueNode node;
        parse_client_request(client_fd, &node);
        struct http_request *req_parse = http_request_parse(node.req_str);
#ifdef ENABLE_DEBUG_PRINT
        printf("L: Parsed client request (fd = %d).\n", client_fd);
#endif
        if (strncmp(req_parse->path, GETJOBCMD, strlen(req_parse->path)) == 0)
        {
#ifdef ENABLE_DEBUG_PRINT
            printf("L: Processing GetJob request (fd = %d).\n", client_fd);
#endif
            // Handle GetJob request
            struct QueueNode *job = get_work_nonblocking();
            if (job == (struct QueueNode *)0)
            {
                send_error_response(node.client_fd, QUEUE_EMPTY, "Priority Queue is Empty");
            }
            else
            {
                struct http_request *job_parse = http_request_parse(job->req_str);
                http_start_response(client_fd, OK);
                http_send_header(client_fd, "Content-Type", "text/html");
                http_end_headers(client_fd);
                http_send_string(client_fd, job_parse->path);
                http_send_string(client_fd, "\n");
                free(job_parse);
            }

            shutdown(client_fd, SHUT_WR);
            close(client_fd);
#ifdef ENABLE_DEBUG_PRINT
            printf("L: Processed GetJob request (fd = %d).\n", client_fd);
#endif
        }
        else
        {
#ifdef ENABLE_DEBUG_PRINT
            printf("L: Adding to the queue (fd = %d).\n", client_fd);
#endif
            // Enqueue
            switch (add_work(&node))
            {
            case -4: // Invalid priority
                send_error_response(node.client_fd, BAD_REQUEST, "Priority is invalid.");
                shutdown(node.client_fd, SHUT_WR);
                close(node.client_fd);
                break;
            case -3: // Failing to release mutex
                send_error_response(node.client_fd, SERVER_ERROR, "Failed to release mutex on queue.");
                shutdown(node.client_fd, SHUT_WR);
                close(node.client_fd);
                break;
            case -2: // Failing to acquire mutex
                send_error_response(node.client_fd, SERVER_ERROR, "Failed to acquire mutex on queue.");
                shutdown(node.client_fd, SHUT_WR);
                close(node.client_fd);
                break;
            case -1: // Full queue
                send_error_response(node.client_fd, QUEUE_FULL, "Priority Queue is Full");
                shutdown(node.client_fd, SHUT_WR);
                close(node.client_fd);
                break;
            default:
                // Success case
                break;
            }
#ifdef ENABLE_DEBUG_PRINT
            printf("L: Added to the queue (fd = %d).\n", client_fd);
#endif
        }

        free(req_parse);
    }

    shutdown(*server_fd, SHUT_RDWR);
    close(*server_fd);
}

/*
 * Default settings for in the global configuration variables
 */
void default_settings()
{
    num_listener = 1;
    listener_ports = (int *)malloc(num_listener * sizeof(int));
    listener_ports[0] = 8000;

    num_workers = 1;

    fileserver_ipaddr = "127.0.0.1";
    fileserver_port = 3333;

    max_queue_size = 100;
}

void print_settings()
{
    printf("\t---- Setting ----\n");
    printf("\t%d listeners [", num_listener);
    for (int i = 0; i < num_listener; i++)
        printf(" %d", listener_ports[i]);
    printf(" ]\n");
    printf("\t%d workers\n", num_listener);
    printf("\tfileserver ipaddr %s port %d\n", fileserver_ipaddr, fileserver_port);
    printf("\tmax queue size  %d\n", max_queue_size);
    printf("\t  ----\t----\t\n");
}

void signal_callback_handler(int signum)
{
    printf("Caught signal %d: %s\n", signum, strsignal(signum));
    for (int i = 0; i < num_listener; i++)
    {
        if (close(server_fds[i]) < 0)
            perror("Failed to close server_fd (ignoring)\n");
    }
    free(listener_ports);
    destroy_queue();
    exit(0);
}

char *USAGE =
    "Usage: ./proxyserver [-l 1 8000] [-n 1] [-i 127.0.0.1 -p 3333] [-q 100]\n";

void exit_with_usage()
{
    fprintf(stderr, "%s", USAGE);
    exit(EXIT_SUCCESS);
}

void *listen_thread(void *args)
{
    struct ThreadArg *t_arg = (struct ThreadArg *)args;
    int port_index = t_arg->port_id;
    int s_fd = t_arg->serv_fd;
    serve_forever(&s_fd, port_index);
    return NULL;
}

void *worker_thread(void *args)
{
    while (1)
    {
        struct QueueNode *job = get_work();
        if (job->delay > 0)
        {
            sleep(job->delay);
        }
        serve_request(job);

        free(job->req_str);
        free(job);

        shutdown(job->client_fd, SHUT_WR);
        close(job->client_fd);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_callback_handler);

    /* Default settings */
    default_settings();

    for (int i = 1; i < argc; i++)
    {
        if (strcmp("-l", argv[i]) == 0)
        {
            num_listener = atoi(argv[++i]);
            free(listener_ports);
            listener_ports = (int *)malloc(num_listener * sizeof(int));
            for (int j = 0; j < num_listener; j++)
            {
                listener_ports[j] = atoi(argv[++i]);
            }
        }
        else if (strcmp("-w", argv[i]) == 0)
        {
            num_workers = atoi(argv[++i]);
        }
        else if (strcmp("-q", argv[i]) == 0)
        {
            max_queue_size = atoi(argv[++i]);
        }
        else if (strcmp("-i", argv[i]) == 0)
        {
            fileserver_ipaddr = argv[++i];
        }
        else if (strcmp("-p", argv[i]) == 0)
        {
            fileserver_port = atoi(argv[++i]);
        }
        else
        {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            exit_with_usage();
        }
    }
    print_settings();

    create_queue(max_queue_size);

    pthread_t *l_threads = malloc(num_listener * sizeof(pthread_t));
    pthread_t *w_threads = malloc(num_workers * sizeof(pthread_t));

    struct ThreadArg *args = malloc(num_listener * sizeof(struct ThreadArg));
    server_fds = malloc(num_listener * sizeof(int));

    for (int i = 0; i < num_listener; i++)
    {
        args[i].port_id = i;
        if (pthread_create(&l_threads[i], NULL, listen_thread, (void *)&args[i]) < 0)
        {
#ifdef ENABLE_DEBUG_PRINT
            printf("Failed to create listener thread %d. Exiting...\n", i);
#endif
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < num_workers; i++)
    {
        if (pthread_create(&w_threads[i], NULL, worker_thread, NULL) < 0)
        {
#ifdef ENABLE_DEBUG_PRINT
            printf("Failed to create worker thread %d. Exiting...\n", i);
#endif
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < num_listener; i++)
    {
        if (pthread_join(l_threads[i], (void **)0) != 0)
        {
#ifdef ENABLE_DEBUG_PRINT
            printf("Failed to join listener thread %d. Exiting...\n", i);
#endif
            exit(EXIT_FAILURE);
        }
    }
    for (int i = 0; i < num_workers; i++)
    {
        if (pthread_join(w_threads[i], (void **)0) != 0)
        {
#ifdef ENABLE_DEBUG_PRINT
            printf("Failed to join worker thread %d. Exiting...\n", i);
#endif
            exit(EXIT_FAILURE);
        }
    }

    free(l_threads);
    free(w_threads);
    free(args);

    return EXIT_SUCCESS;
}