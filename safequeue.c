#include <pthread.h>
#include <stdlib.h>

#include "safequeue.h"

#ifdef ENABLE_DEBUG_PRINT
#include <stdio.h>
#endif

struct SafeQueue safe_queue;
pthread_cond_t not_empty;

#ifdef ENABLE_DEBUG_PRINT
static void print_queue_no_lock()
{
    printf("[");
    if (safe_queue.curr_size > 0)
    {
        printf("%d", safe_queue.nodes[0].priority);
        for (int i = 1; i < safe_queue.curr_size; i++)
        {
            printf(", %d", safe_queue.nodes[i].priority);
        }
    }
    printf("]\n");
}

void print_queue()
{
    pthread_mutex_lock(&safe_queue.mutex);
    print_queue_no_lock();
    pthread_mutex_unlock(&safe_queue.mutex);
}
#endif

int create_queue(int max_size)
{
    safe_queue.curr_size = 0;
    safe_queue.max_size = max_size;

    safe_queue.nodes = calloc(max_size, sizeof(struct QueueNode));
    if (safe_queue.nodes == NULL)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to allocate space for priority queue\n");
#endif
        exit(EXIT_FAILURE);
    }

    if (pthread_mutex_init(&safe_queue.mutex, NULL) < 0)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to initialize mutex lock for priority queue\n");
#endif
        exit(EXIT_FAILURE);
    }

    if (pthread_cond_init(&not_empty, NULL) < 0)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to initialize condition variable for priority queue\n");
#endif
        exit(EXIT_FAILURE);
    }

    return 0;
}

int destroy_queue()
{
    free(safe_queue.nodes);
    if (pthread_mutex_destroy(&safe_queue.mutex) != 0)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to destroy the mutex lock.\n");
#endif
        return -1;
    }

    if (pthread_cond_destroy(&not_empty) != 0)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to destroy the condition variable.\n");
#endif
        return -1;
    }

    return 0;
}

static int lock_queue()
{
    if (pthread_mutex_lock(&safe_queue.mutex) < 0)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to acquire the lock");
#endif
        return -1; //? Exit instead?
    }
    return 0;
}
static int unlock_queue()
{
    if (pthread_mutex_unlock(&safe_queue.mutex) < 0)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to release the lock");
#endif
        return -1; //? Exit instead?
    }
    return 0;
}

int curr_len()
{
    if (lock_queue() < 0)
    {
        exit(EXIT_FAILURE);
    }

    int len = safe_queue.curr_size;

    if (unlock_queue() < 0)
    {
        exit(EXIT_FAILURE);
    }

    return len;
}

// Assumes that safe_queue.mutex is held before calling.
int add_work(struct QueueNode *node)
{
    if (lock_queue() < 0)
    {
        return -2;
    }

    if (safe_queue.curr_size >= safe_queue.max_size)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("The queue is full.\n");
#endif
        if (unlock_queue() < 0)
        {
            return -3;
        }
        return -1;
    }
    else if (!(MIN_PRIORITY <= node->priority && node->priority <= MAX_PRIORITY))
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Priority value must be between %d and %d (inclusive).\n", MIN_PRIORITY, MAX_PRIORITY);
#endif
        if (unlock_queue() < 0)
        {
            return -3;
        }
        return -4;
    }

#ifdef ENABLE_DEBUG_PRINT
    printf("Inserting priority %d\n", node->priority);
#endif

    switch (safe_queue.curr_size)
    {
    case 0:
        safe_queue.nodes[0] = *node;
        break;
    default:
        int found = 0;
        for (int i = 0; i < safe_queue.curr_size; i++)
        {
            if (node->priority <= safe_queue.nodes[i].priority)
            {
                continue;
            }
            else
            {
                found = 1;
                for (int j = safe_queue.max_size - 2; j >= i; j--)
                {
                    safe_queue.nodes[j + 1] = safe_queue.nodes[j];
                }
                safe_queue.nodes[i] = *node;
                break;
            }
        }

        if (!found)
        {
            safe_queue.nodes[safe_queue.curr_size] = *node;
        }

        break;
    }

    // Update the current size
    safe_queue.curr_size++;

    // Signal any one of the threads blocked by get_work()
    pthread_cond_signal(&not_empty);

#ifdef ENABLE_DEBUG_PRINT
    print_queue_no_lock();
#endif

    if (unlock_queue() < 0)
    {
        return -3;
    }
    return 0;
}

static struct QueueNode *get_work_helper(int block)
{
    struct QueueNode *data = malloc(sizeof(struct QueueNode));
    if (data == NULL)
    {
#ifdef ENABLE_DEBUG_PRINT
        printf("Failed to allocate space for a new node (get_work).\n");
#endif
        exit(EXIT_FAILURE);
    }

    if (lock_queue() < 0)
    {
        return (struct QueueNode *)0; //? Exit instead?
    }

    // If the queue is empty, block the current thread until some data becomes
    // available (signaled by add_work).
    while (safe_queue.curr_size <= 0)
    {
        if (block)
        {
            pthread_cond_wait(&not_empty, &safe_queue.mutex);
        }
        else
        {
            free(data);
            if (unlock_queue() < 0)
            {
                exit(EXIT_FAILURE);
            }
            return (struct QueueNode *)0;
        }
    }

    // Copy the head's data to return node
    *data = safe_queue.nodes[0];
    // Shift all nodes in queue up by 1.
    for (int i = 1; i < safe_queue.curr_size; i++)
    {
        safe_queue.nodes[i - 1] = safe_queue.nodes[i];
    }

    // Clear the last node's data to prevent dangling duplicate at the end.
    safe_queue.nodes[safe_queue.curr_size].req_str = NULL;
    safe_queue.nodes[safe_queue.curr_size].priority = 0;
    safe_queue.nodes[safe_queue.curr_size].client_fd = 0;
    safe_queue.nodes[safe_queue.curr_size].delay = 0;

    // Update the current size
    safe_queue.curr_size--;

#ifdef ENABLE_DEBUG_PRINT
    printf("Dequeued priority %d.\n", data->priority);
    print_queue_no_lock();
#endif

    if (unlock_queue() < 0)
    {
        return (struct QueueNode *)0; //? Exit instead?
    }

    return data;
}

struct QueueNode *get_work()
{
    return get_work_helper(1);
}

struct QueueNode *get_work_nonblocking()
{
    return get_work_helper(0);
}