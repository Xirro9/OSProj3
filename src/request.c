/*
 * Attribution:
 * This project was developed with help from online resources for educational purposes.
 * Major references include:
 * - MIT Rust Book Chapter 20 Web Server: 
 *   https://web.mit.edu/rust-lang_v1.25/arch/amd64_ubuntu1404/share/doc/rust/html/book/second-edition/ch20-00-final-project-a-web-server.html
 * - Stack Overflow thread on multithreaded web servers in C:
 *   https://stackoverflow.com/questions/61551263/multithreaded-web-server-in-c
 * Other smaller sources were consulted for specific C or pthreads usage.
 */


// Include necessary header files
#include "io_helper.h"
#include "request.h"

// Include standard library headers for threading, memory management, and file operations
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

// Define buffer size and maximum number of requests
#define MAXBUF (8192)
#define MAX_REQUESTS 1024

// Define default values for number of threads, buffer size, and scheduling algorithm
// These values are defined in 'request.h'
int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;

// Define a struct to represent a request
typedef struct {
   int fd; // File descriptor
   char filename[MAXBUF]; // Filename
   int filesize; // File size
} request_t;

// Define a global buffer to store requests
request_t request_buffer[MAX_REQUESTS];
int buffer_count = 0; // Initialize buffer count to 0

// Define synchronization variables for threading
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

// Define a thread-safe random number generator
pthread_key_t rand_state_key;
pthread_once_t rand_state_once = PTHREAD_ONCE_INIT;

// Initialize the random number generator
void init_rand_state(void) {
   pthread_key_create(&rand_state_key, NULL);
}

// Generate a random number using the thread-safe random number generator
int pthread_rand_r(void) {
   int *state = pthread_getspecific(rand_state_key);
   if (state == NULL) {
       state = malloc(sizeof(int));
       *state = rand();
       pthread_setspecific(rand_state_key, state);
   }
   return rand_r(state);
}

// Define a function to serve static requests
void* thread_request_serve_static(void* arg) {
   // Initialize the random number generator
   pthread_once(&rand_state_once, init_rand_state);

   // Loop indefinitely to serve requests
   while (1) {
       // Lock the mutex to access the request buffer
       pthread_mutex_lock(&lock);

       // Wait until there are requests in the buffer
       while (buffer_count == 0) {
           pthread_cond_wait(&buffer_not_empty, &lock);
       }

       // Select the next request to serve
       int target = 0;
       if (scheduling_algo == 1) { // Shortest File First (SFF)
           for (int i = 1; i < buffer_count; ++i) {
               if (request_buffer[i].filesize < request_buffer[target].filesize) {
                   target = i;
               }
           }
       } else if (scheduling_algo == 2) { // Random
           target = pthread_rand_r() % buffer_count;
       }

       // Get the request to serve
       request_t req = request_buffer[target];

       // Move the request to the front of the buffer
       for (int i = target; i < buffer_count - 1; ++i) {
           request_buffer[i] = request_buffer[i + 1];
       }
       buffer_count--;

       // Signal that the buffer is not full
       pthread_cond_signal(&buffer_not_full);

       // Unlock the mutex
       pthread_mutex_unlock(&lock);

       // Serve the request
       request_serve_static(req.fd, req.filename, req.filesize);

       // Close the file descriptor
       close_or_die(req.fd);
   }
   return NULL;
}

// Define a function to handle requests
void request_handle(int fd) {
   int is_static; // Flag to indicate if the request is static
   struct stat sbuf; // Stat buffer to store file information
   char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF]; // Buffers to store request information
   char filename[MAXBUF], cgiargs[MAXBUF]; // Buffers to store filename and CGI arguments

   // Read the request line
   readline_or_die(fd, buf, MAXBUF);

   // Parse the request line
   sscanf(buf, "%s %s %s", method, uri, version);

   // Print the request information
   printf("method:%s uri:%s version:%s\n", method, uri, version);

   // Check if the request method is GET
   if (strcasecmp(method, "GET")) {
       request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
       return;
   }

   // Read the request headers
   request_read_headers(fd);

   // Parse the URI to determine if the request is static or dynamic
   is_static = request_parse_uri(uri, filename, cgiargs);

   // Get the file information
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


       // Directory traversal mitigation: ensure the resolved path starts with "./www"
       char resolved_path[PATH_MAX];
       realpath(filename, resolved_path);
       char root_dir[PATH_MAX];
       realpath("./www", root_dir);
       if (strncmp(resolved_path, root_dir, strlen(root_dir)) != 0) {
           request_error(fd, filename, "403", "Forbidden", "illegal file path");
           return;
       }

       
       // Lock the mutex to access the request buffer
       pthread_mutex_lock(&lock);
       
       // Wait until the buffer is not full
       while (buffer_count == buffer_max_size) {
          pthread_cond_wait(&buffer_not_full, &lock);
       }
       
       // Create a new request structure
       request_t req;
       req.fd = fd; // Set the file descriptor
       strcpy(req.filename, filename); // Set the filename
       req.filesize = sbuf.st_size; // Set the file size
       
       // Add the request to the buffer
       request_buffer[buffer_count++] = req;
       
       // Signal that the buffer is not empty
       pthread_cond_signal(&buffer_not_empty);
       
       // Unlock the mutex
       pthread_mutex_unlock(&lock);
   }
}
