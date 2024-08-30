#ifndef _SAFE_QUEUE_HEADER_GUARD
#define _SAFE_QUEUE_HEADER_GUARD

#include <pthread.h>

// Comment this line out to disable debug printing
// #define ENABLE_DEBUG_PRINT 1

#define MIN_PRIORITY 1
#define MAX_PRIORITY 10

#if MIN_PRIORITY <= 0
#define MIN_PRIORITY 1
#endif

struct QueueNode
{
    char *req_str;
    int priority;
    int client_fd;
    int delay;
};

struct SafeQueue
{
    pthread_mutex_t mutex;
    struct QueueNode *nodes;
    int curr_size;
    int max_size;
};

extern pthread_cond_t not_empty;
extern struct SafeQueue safe_queue;

int create_queue(int max_size);
int destroy_queue();

int curr_len(); // TODO: May not need this.

int add_work(struct QueueNode *node);
struct QueueNode *get_work();
struct QueueNode *get_work_nonblocking();

#ifdef ENABLE_DEBUG_PRINT
void print_queue();
#endif

#endif