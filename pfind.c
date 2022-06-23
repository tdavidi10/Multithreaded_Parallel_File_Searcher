#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h> /* for PATH_MAX value */
#include <errno.h>
#include <dirent.h> /* for readdir... */ 
#include <sys/stat.h> 
#include <threads.h>
#include <unistd.h>

/* variables all know */
mtx_t enqueue_lock;
mtx_t dequeue_lock;
mtx_t main_start_lock;
mtx_t q_empty_lock; /* thread waits if q is empty */
mtx_t file_found_lock; /* counter needs to be sync */
mtx_t sub_wait_thread_lock;
cnd_t ready_threads_cv;
cnd_t main_start_cv; /* main finishes creating  threads and signals them to start */
cnd_t q_empty_cv; 
int files_found;
int num_waiting_threads;
int ready_threads_counter = 0;
int is_thread_error = 0; /* if error occurs in a thread, becomes 1 */
int ended_search = 0; /* if search is finished, becomes 1 */


/* dir struct with path to it */
typedef struct MY_DIR {
    DIR* dir;
    char path[PATH_MAX];
}MY_DIR;

/* node of the linked list that represents the queue */
typedef struct node {
    MY_DIR* my_dir;
    struct node* next;
}node;

/* queue DS has pointer to head and tail of the queue: queue of my_dir*/
typedef struct queue {
    node* head;
    node* tail;
}queue;

/* create new node to be added to the queue */
node* create_node(MY_DIR* my_dir) {
    node* new_node = (node*)malloc(sizeof(node));
    if (new_node == NULL) {
        fprintf(stderr, "Failed to allocate memory for new node\n");
        exit(1);
    }
    new_node->my_dir = my_dir;
    new_node->next = NULL;
    return new_node;
}

/* create new queue */
queue* create_queue() {
    queue* new_queue = (queue*)malloc(sizeof(queue));
    if (new_queue == NULL) {
        fprintf(stderr, "Failed to allocate memory for new queue\n");
        exit(1);
    }
    new_queue->head = NULL;
    new_queue->tail = NULL;
    return new_queue;
}

/* add dir to the end of the queue */
void enqueue(queue* q, MY_DIR* my_dir) {
    /* lock enqueue_lock */
    mtx_lock(&enqueue_lock);
    /* do the enqueue */
    node* node = create_node(my_dir); /* create new node from dir*/
    if (q->head == NULL) { /* if queue is empty */
        q->head = node;
        q->tail = node;
    } 
    else { /* if queue is not empty */
        q->tail->next = node; /* add node to the end of the queue */
        q->tail = node; /* update tail */
    }
    /* unlock enqueue_lock */
    mtx_unlock(&enqueue_lock);
}

/* return the head-dir of the q and remove the head-node from the queue */
MY_DIR* dequeue(queue* q) {
    /* lock dequeue_lock */
    mtx_lock(&dequeue_lock);
    /* do the dequeue */
    if (q->head == NULL) { /* if queue is empty */
        mtx_unlock(&dequeue_lock); /* unlock dequeue_lock */
        return NULL; /* there is no head to return */
    } 
    else { /* if queue is not empty */   
        node* temp = q->head; /* save the head */
        MY_DIR* my_dir = temp->my_dir; /* save the head-dir */
        q->head = q->head->next; /* update head */
        if (q->head == NULL) { /* if head is NULL */
            q->tail = NULL; /* update tail too */
        }
        free(temp); /* free the previous head */
        /* unlock dequeue_lock */
        mtx_unlock(&dequeue_lock);
        return my_dir; /* return the head-dir */
    }
}

/* struct to pass as argument to thread */
typedef struct arguments {
    queue* q;
    char* term;
    int num_threads;
    int id;
}arguments;

/* atomic dec of waiting thread */
void sub_one_waiting_thread() {
    mtx_lock(&sub_wait_thread_lock);
    num_waiting_threads--;
    mtx_unlock(&sub_wait_thread_lock);
}

/* Each searching thread
removes directories from the queue and searches for file names containing the search term */
int thread_fun(void* arguments_param){
    DIR* dir; /* main dir(original path) we search in now */
    MY_DIR* my_dir; 
    DIR* new_dir; /* a dir we see inside the main dir */
    MY_DIR* new_my_dir;
    struct dirent* entry;
    struct stat stat_buffer;
    arguments* args = (arguments*)arguments_param; /* cast to arguments struct that holds q,term */
    queue* q = args->q;
    char* term = args->term;
    int num_threads = args->num_threads;
    char path[PATH_MAX]; /* full path to the new file/dir we see in the main dir - dirty */
    char origin_path[PATH_MAX]; 


    mtx_lock(&main_start_lock);
    /* wait for all threads to be ready */
    ready_threads_counter++; /* now this thread is ready */
    if (ready_threads_counter == num_threads) { /* if all threads are ready */
        cnd_broadcast(&ready_threads_cv); /* signal all that all threads are ready */
    }
    cnd_wait(&main_start_cv, &main_start_lock); /* wait for all threads to be ready */
    mtx_unlock(&main_start_lock);
    
    
    /* start searching */

    /* search as long as you can */
    while (1)
    {
        my_dir = dequeue(q); /* get the first dir from the queue */
        while (my_dir == NULL) { /* if queue is empty */
            /*
                Wait until the queue becomes non-empty. If all other searching threads are already waiting, 
                that means there are no more directories to search. In this case, all searching threads should
                exit
            */
            mtx_lock(&q_empty_lock);
            num_waiting_threads++; /* now this thread waits -> inc num_waiting_threads */
            if (num_waiting_threads == num_threads) { /* if all threads are waiting */
                ended_search = 1;
                cnd_broadcast(&q_empty_cv); /* signal all that all threads are waiting */
                mtx_unlock(&q_empty_lock); /* unlock q_empty_lock */
                thrd_exit(0); /* finish searching */
            }

            cnd_wait(&q_empty_cv, &q_empty_lock); /* waits until queue is not empty */
            if(ended_search == 1){ /* if we finished searching end thread */
                mtx_unlock(&q_empty_lock); /* unlock q_empty_lock */
                thrd_exit(0); /* finish searching */
            }
            /* else: new dir came to queue wake-up */
            sub_one_waiting_thread(); /* this thread is no longer waiting -> dec num_waiting_threads */
            mtx_unlock(&q_empty_lock);
            my_dir = dequeue(q);
        }
        
    
        dir = my_dir->dir; /* get the dir from the my_dir struct */
        strcpy(path, strcat(my_dir->path, "/")); /* update path -> dir/ */
        strcpy(origin_path, path); 
        /* while there are directories in the queue - remove the head dir and search term in it */
        while ((entry = readdir(dir)) != NULL) { /* get the next entry in the head-dir */
            if (strcmp("..", entry->d_name) == 0 || strcmp(".", entry->d_name) == 0) { /* if entry is "." or ".." */
                continue; /* ignore it - go on to next dirent */
            }
                    
            /* path = origin_path/entry->d_name */
            if (strcpy(path, strcat(origin_path, entry->d_name)) == NULL) { /* concatenate the path */
                fprintf(stderr, "Failed to concatenate path\n");
                is_thread_error = 1; 
                thrd_exit(1);
            }  
            if (strcpy(origin_path, my_dir->path) == NULL) /* reset origin_path to my_dir->path */
            {
                fprintf(stderr, "Failed to reset origin_path\n");
                is_thread_error = 1; 
                thrd_exit(1);
            }

            /* if the dirent is for a directory */
            if(stat(path, &stat_buffer) != 0){
                fprintf(stderr, "Failed to stat %s\n", entry->d_name);
                is_thread_error = 1; 
                thrd_exit(1); /* exit with error number 1*/
            }
            if (S_ISDIR(stat_buffer.st_mode)) {
                new_dir = opendir(path); /* open the new found directory */
                if (new_dir == NULL) { /* if the directory can't be searched */
                    printf("Directory %s: Permission denied.\n", path); /* print given message */
                }
                else{ /* if the new-found-dir can be searched add it to end of queue */
                    new_my_dir = (MY_DIR*)malloc(sizeof(MY_DIR));
                    if (new_my_dir == NULL) {
                        fprintf(stderr, "Failed to allocate memory for new_my_dir\n");
                        is_thread_error = 1; 
                        thrd_exit(1); /* exit with error number 1*/
                    }
                    new_my_dir->dir = new_dir;
                    strcpy(new_my_dir->path, path);
                    enqueue(q, new_my_dir);
                    cnd_signal(&q_empty_cv); /* signal that the queue is no longer empty */ 
                }
            }
            else{  /* if the dirent is for a file (not for a directory) */
                if (strstr(entry->d_name, term) != NULL) { /* if the file name contains the search term case-sensitive */
                    printf("%s \n", path); /* print the full path to the file */
                    mtx_lock(&file_found_lock);
                    files_found = files_found + 1; /* update the number of files found */
                    mtx_unlock(&file_found_lock);
                }
            }           
        }
        closedir(dir); /* close the head-dir */  
        if ((q->head != NULL) && (num_waiting_threads > 0))  /* give advantage to those who are waiting */
            sleep(0.005);

    }

}

int main(int argc, char *argv[]){
    char* dir_path;
    char* term;
    int mun_of_threads; /* given > 0 */
    DIR* root_dir;
    MY_DIR* my_dir;
    queue* q;
    thrd_t* threads;
    arguments** args;
    
    if (argc != 4) { /* argc has to be 4 (3 real arguments) */
        fprintf(stderr, "wrong number of arguments\n"); /* wrong number of arguments */
        exit(1);
    }
    /* argumets: root_directory[1], term[2], num_of_threads[3] */
    dir_path = argv[1];
    term = argv[2];
    mun_of_threads = atoi(argv[3]);
    root_dir = opendir(dir_path); /* open the directory */
    if (root_dir == NULL) { /* if cant open the root_dir - dir can't be searched*/
        fprintf(stderr, "cannot open directory - can't be searched \n"); /* print error to stderr */ 
        exit(1);
    }

    /* Allocate memory for the my_DIR */
    my_dir = (MY_DIR*)malloc(sizeof(MY_DIR));
    if (my_dir == NULL) {
        fprintf(stderr, "Failed to allocate memory for my_dir\n");
        exit(1);
    }
    my_dir->dir = root_dir; /* update my_dir->dir */  
    strcpy(my_dir->path, dir_path); /* update my_dir->path */  

    /* locks & conditional-variables init */
    mtx_init(&enqueue_lock, mtx_plain);
    mtx_init(&dequeue_lock, mtx_plain);
    mtx_init(&main_start_lock, mtx_plain);
    mtx_init(&file_found_lock, mtx_plain);
    mtx_init(&q_empty_lock, mtx_plain);
    mtx_init(&sub_wait_thread_lock, mtx_plain);
    cnd_init(&ready_threads_cv);
    cnd_init(&q_empty_cv);
    cnd_init(&main_start_cv);    


    /* create queue that holds directories */
    q = create_queue(); /* empty queue */
    enqueue(q, my_dir); /* add root_dir to the queue */
    /* allocate memory for -mun_of_threads- threads */
    threads = (thrd_t*)malloc(sizeof(thrd_t) * mun_of_threads);
    if (threads == NULL) { /* if memory allocation failed */
        fprintf(stderr, "Failed to allocate memory for threads array\n"); /* print error to stderr */
        exit(1);
    }
    args = (arguments**)malloc(mun_of_threads * sizeof(arguments*)); /* allocate memory for arguments array */
    if (args == NULL) { /* if memory allocation failed */
        fprintf(stderr, "Failed to allocate memory for arguments struct\n"); /* print error to stderr */
        exit(1);
    }
    for(int i=0; i<mun_of_threads; i++){ /* allocate memory for each thread */
        args[i] = (arguments*)malloc(sizeof(arguments));
        if (args[i] == NULL) { /* if memory allocation failed */
            fprintf(stderr, "Failed to allocate memory for arguments struct\n"); /* print error to stderr */
            exit(1);
        }
        /* update arguments struct */
        args[i]->q = q; 
        args[i]->term = term; 
        args[i]->num_threads = mun_of_threads; 
        args[i]->id = 0;
    }
    

    /* create -mun_of_threads- searching threads */
    num_waiting_threads = 0; /* mun_of_threads; */
    mtx_lock(&main_start_lock);
    for (int i = 0; i < mun_of_threads; i++) {
        args[i]->id = i;
        if (thrd_create(&threads[i], thread_fun, (void*)args[i]) != thrd_success) /* create a thread and if error:*/
        {
            fprintf(stderr, "Failed to create thread\n"); /* print error to stderr */
            exit(1);
        }
    }
    /* wait for all searching threads to be created and for the main thread to signal that the searching should start */
    cnd_wait(&ready_threads_cv, &main_start_lock); /* wait for all threads to be ready */
    cnd_broadcast(&main_start_cv); /* tell all thread to start */
    mtx_unlock(&main_start_lock);

    /* wait for all searching threads to finish */
    for (int i = 0; i < mun_of_threads; i++) {
        if (thrd_join(threads[i], NULL) != thrd_success) /* join the threads and if error: */
        {
            fprintf(stderr, "Failed to join thread\n"); /* print error to stderr */
            exit(1);
        }
    }

    printf("Done searching, found %d files\n", files_found); /* print the number of files found */

    /* locks & conditional-variables destroy */
    mtx_destroy(&enqueue_lock);
    mtx_destroy(&dequeue_lock);
    mtx_destroy(&main_start_lock);
    mtx_destroy(&file_found_lock);
    mtx_destroy(&q_empty_lock);
    mtx_destroy(&sub_wait_thread_lock);
    cnd_destroy(&ready_threads_cv);
    cnd_destroy(&main_start_cv);
    cnd_destroy(&q_empty_cv);

    if (is_thread_error == 1) {
        fprintf(stderr, "error in at least one thread\n"); /* print error to stderr */
        exit(1);
    }
    exit(0); /* all good */

}
