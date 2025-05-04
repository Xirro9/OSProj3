#include "io_helper.h"
#include "request.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>

#define MAXBUF (8192)
#define MAX_REQUESTS 1024

// below default values are defined in 'request.h'
int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;	



//
//	TODO: add code to create and manage the shared global buffer of requests
//	HINT: You will need synchronization primitives.
//		pthread_mutuex_t lock_var is a viable option.
//

typedef struct {
    int fd;
    char filename[MAXBUF];
    int filesize;
} request_t;

request_t request_buffer[MAX_REQUESTS];
int buffer_front = 0;
int buffer_rear = 0;
int buffer_count = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
	readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, ".%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
	strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
	strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
	strcpy(filetype, "image/jpeg");
    else 
	strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg) {
    // TODO: write code to actually respond to HTTP requests
    // Pull from global buffer of requests

    // CHECKPOINT 4: Consume request from buffer (consumer logic)
    while (1) {
        pthread_mutex_lock(&lock);
        while (buffer_count == 0) {
            pthread_cond_wait(&buffer_not_empty, &lock);
        }

        // CHECKPOINT 6: Implement scheduling policy (FIFO, SFF, and Random)
        int target_index = buffer_front;
        if (scheduling_algo == 1) { // SFF
            int smallest_index = buffer_front;
            for (int i = 1; i < buffer_count; ++i) {
                int current_index = (buffer_front + i) % MAX_REQUESTS;
                if (request_buffer[current_index].filesize < request_buffer[smallest_index].filesize) {
                    smallest_index = current_index;
                }
            }
            target_index = smallest_index;
        } else if (scheduling_algo == 2) { // Random
            int offset = rand() % buffer_count;
            target_index = (buffer_front + offset) % MAX_REQUESTS;
        }

        request_t req = request_buffer[target_index];

        // Shift buffer contents to fill the gap left by the extracted request
        for (int i = target_index; i != buffer_rear; i = (i + 1) % MAX_REQUESTS) {
            int next_index = (i + 1) % MAX_REQUESTS;
            request_buffer[i] = request_buffer[next_index];
        }
        buffer_rear = (buffer_rear - 1 + MAX_REQUESTS) % MAX_REQUESTS;
        buffer_count--;

        

        
        pthread_cond_signal(&buffer_not_full);
        pthread_mutex_unlock(&lock);

        
        request_serve_static(req.fd, req.filename, req.filesize);
        close_or_die(req.fd);
    }
}

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

        // TODO: directory traversal mitigation
        char resolved_path[PATH_MAX];
        if (realpath(filename, resolved_path) == NULL) {
            request_error(fd, filename, "403", "Forbidden", "server could not resolve file path");
            return;
        }
        if (strstr(resolved_path, "/..") != NULL || strstr(resolved_path, "//") != NULL) {
            request_error(fd, filename, "403", "Forbidden", "directory traversal attempt blocked");
            return;
        }
        // TODO: write code to add HTTP requests in the buffer

        pthread_mutex_lock(&lock);
        while (buffer_count == buffer_max_size) {
            pthread_cond_wait(&buffer_not_full, &lock);
        }

        request_t req;
        req.fd = fd;
        strcpy(req.filename, filename);
        req.filesize = sbuf.st_size;

        request_buffer[buffer_rear] = req;
        buffer_rear = (buffer_rear + 1) % MAX_REQUESTS;
        buffer_count++;

        pthread_cond_signal(&buffer_not_empty);
        pthread_mutex_unlock(&lock);

    } else {
        request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}
