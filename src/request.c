#include "io_helper.h"
#include "request.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#define MAXBUF (8192)
#define MAX_REQUESTS 1024


// below default values are defined in 'request.h'
int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;

// Global buffer and sync variables
typedef struct {
    int fd;
    char filename[MAXBUF];
    int filesize;
} request_t;

request_t request_buffer[MAX_REQUESTS];
int buffer_count = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

// Thread-safe random number generator
pthread_key_t rand_state_key;
pthread_once_t rand_state_once = PTHREAD_ONCE_INIT;

void init_rand_state(void) {
    pthread_key_create(&rand_state_key, NULL);
}

int pthread_rand_r(void) {
    int *state = pthread_getspecific(rand_state_key);
    if (state == NULL) {
        state = malloc(sizeof(int));
        *state = rand();
        pthread_setspecific(rand_state_key, state);
    }
    return rand_r(state);
}

// ... (rest of the code remains the same)

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg)
{
    pthread_once(&rand_state_once, init_rand_state);
    while (1) {
        pthread_mutex_lock(&lock);
        while (buffer_count == 0) {
            pthread_cond_wait(&buffer_not_empty, &lock);
        }

        int target = 0;
        if (scheduling_algo == 1) { // SFF
            for (int i = 1; i < buffer_count; ++i) {
                if (request_buffer[i].filesize < request_buffer[target].filesize) {
                    target = i;
                }
            }
        } else if (scheduling_algo == 2) { // Random
            target = pthread_rand_r() % buffer_count;
        }

        request_t req = request_buffer[target];
        for (int i = target; i < buffer_count - 1; ++i) {
            request_buffer[i] = request_buffer[i + 1];
        }
        buffer_count--;

        pthread_cond_signal(&buffer_not_full);
        pthread_mutex_unlock(&lock);

        request_serve_static(req.fd, req.filename, req.filesize);
        close_or_die(req.fd);
    }
    return NULL;
}

//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];

    // get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method:%s uri:%s version:%s\n", method, uri, version);

    // verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
        request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
        return;
    }
    request_read_headers(fd);

    // check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);

    // get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
        request_error(fd, filename, "404", "Not found", "server could not find this file");
        return;
    }

    // verify if requested content is static
    if (is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            request_error(fd, filename, "403", "Forbidden", "server could not read this file");
            return;
        }

        // directory traversal mitigation
        char resolved_path[PATH_MAX];
        realpath(filename, resolved_path);
        if (strstr(resolved_path, "./www") != resolved_path) {
            request_error(fd, filename, "403", "Forbidden", "illegal file path");
            return;
        }

        pthread_mutex_lock(&lock);
        while (buffer_count == buffer_max_size) {
            pthread_cond_wait(&buffer_not_full, &lock);
        }

        request_t req;
        req.fd = fd;
        strcpy(req.filename, filename);
        req.filesize = sbuf.st_size;

        request_buffer[buffer_count++] = req;

        pthread_cond_signal(&buffer_not_empty);
        pthread_mutex_unlock(&lock);
    }