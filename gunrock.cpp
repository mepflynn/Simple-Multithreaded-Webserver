#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <sstream>
#include <deque>
#include <queue>

#include "HTTPRequest.h"
#include "HTTPResponse.h"
#include "HttpService.h"
#include "HttpUtils.h"
#include "FileService.h"
#include "MySocket.h"
#include "MyServerSocket.h"
#include "dthread.h"

using namespace std;

int PORT = 8080;
int THREAD_POOL_SIZE = 1;
int BUFFER_SIZE = 1;
string BASEDIR = "static";
string SCHEDALG = "FIFO";
string LOGFILE = "/dev/null";

vector<HttpService *> services;

// Create this struct as a wrapper for the arguments passed to the consumer threads
// upon their creation.
//
// In this case we still only pass one thing, but casting a void* back to a queue<MySocket*> is not an option
// so it is still helpful to follow this struct wrapper approach.
struct ThreadArgs {
  queue<MySocket*> buffer;
};

// Create the lock and conditional variables that will be used to manage access to the requestBuffer
// These are global so that they are accessible to the producer thread in main and the consumers
// which live in handle_request or some lower scope therein
pthread_mutex_t bufferLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t bufferFill = PTHREAD_COND_INITIALIZER;
pthread_cond_t bufferEmpty = PTHREAD_COND_INITIALIZER;

HttpService *find_service(HTTPRequest *request) {
   // find a service that is registered for this path prefix
  for (unsigned int idx = 0; idx < services.size(); idx++) {
    if (request->getPath().find(services[idx]->pathPrefix()) == 0) {
      return services[idx];
    }
  }

  return NULL;
}


void invoke_service_method(HttpService *service, HTTPRequest *request, HTTPResponse *response) {
  stringstream payload;
  
  // invoke the service if we found one
  if (service == NULL) {
    // not found status
    response->setStatus(404);
  } else if (request->isHead()) {
    payload << "HEAD " << request->getPath();
    sync_print("invoke_service_method", payload.str());
    cout << payload.str() << endl;
    service->head(request, response);
  } else if (request->isGet()) {
    payload << "GET " << request->getPath();
    sync_print("invoke_service_method", payload.str());
    cout << payload.str() << endl;
    service->get(request, response);
  } else {
    // not implemented status
    response->setStatus(405);
  }
}

void handle_request(MySocket* client) {


  HTTPRequest *request = new HTTPRequest(client, PORT);
  HTTPResponse *response = new HTTPResponse();
  stringstream payload;

  
  // read in the request
  bool readResult = false;
  try {
    payload << "client: " << (void *) client;
    sync_print("read_request_enter", payload.str());
    readResult = request->readRequest();
    sync_print("read_request_return", payload.str());
  } catch (...) {
    // swallow it
  }    
    
  if (!readResult) {
    // there was a problem reading in the request, bail
    delete response;
    delete request;
    sync_print("read_request_error", payload.str());
    return;
  }

  
  HttpService *service = find_service(request);



  invoke_service_method(service, request, response);

  // send data back to the client and clean up
  payload.str(""); payload.clear();
  payload << " RESPONSE " << response->getStatus() << " client: " << (void *) client;
  sync_print("write_response", payload.str());
  cout << payload.str() << endl;
  client->write(response->response());
    
  delete response;
  delete request;

  payload.str(""); payload.clear();
  payload << " client: " << (void *) client;
  sync_print("close_connection", payload.str());
  client->close();
  delete client;
}

void pushToBuffer(MySocket* client, ThreadArgs* bufferPtr) {
  // A new request has come in. Acquire the lock to try and add it to buffer
    dthread_mutex_lock(&bufferLock);

    // Check for full buffer, and if so wait on the conditional variable bufferEmpty
    // The while loop here ensures that if the producer thread wakes, 
    // it checks its condition again before proceeding.
    while(bufferPtr->buffer.size() == (unsigned int)BUFFER_SIZE) dthread_cond_wait(&bufferEmpty, &bufferLock);

    // Here, the buffer must not be full, so we can push a request to it
    bufferPtr->buffer.push(client);

    // Finally, signal the consumer threads that an element has been added to the buffer
    // If they are all busy, this signal won't necessarily do anything, but if any are 
    // sleeping this will wake one thread which will find the buffer non-empty and pursue that task
    dthread_cond_signal(&bufferFill);

    // Finally, release the associated lock so that some consumer thread can pick it up
    dthread_mutex_unlock(&bufferLock);
}

void *getFromBuffer(void *buffer_struct_ptr) {
  // Arg has to come in as a void*, so cast it back to ptr to struct so that we can get at its queue member buffer
  ThreadArgs* bufferPtr = (ThreadArgs*)buffer_struct_ptr;

  // From here on out, the rest of this function will be an infinite loop, so that the worker thread can keep
  // sleeping until it has a job to do, then sleep again after a job is done

  while(true) {

    // Enter a critical section where we attempt to read a new job from the shared buffer
    dthread_mutex_lock(&bufferLock);

    // Check for buffer empty, if so wait()
    // Specifically, wait on the cond_t bufferFill which is what the producer will signal when it adds to the buffer
    while (bufferPtr->buffer.size() == 0) {
      dthread_cond_wait(&bufferFill, &bufferLock);
    }

    // Buffer no longer empty, and lock is re-acquired by this worker/consumer thread
    // Get this job from the buffer and then pop() it
    MySocket* client = bufferPtr->buffer.front();
    bufferPtr->buffer.pop();

    // That was the extent of our critical section, signal to the producer that an element has been extracted
    // from the buffer and then release the lock
    dthread_cond_signal(&bufferEmpty);
    dthread_mutex_unlock(&bufferLock);

    // Now that lock is reliquished and we have gotten a buffer element into client, pass it off to handler
    handle_request(client);
  }
}

int main(int argc, char *argv[]) {

  signal(SIGPIPE, SIG_IGN);
  int option;

  while ((option = getopt(argc, argv, "d:p:t:b:s:l:")) != -1) {
    switch (option) {
    case 'd':
      BASEDIR = string(optarg);
      break;
    case 'p':
      PORT = atoi(optarg);
      break;
    case 't':
      THREAD_POOL_SIZE = atoi(optarg);
      break;
    case 'b':
      BUFFER_SIZE = atoi(optarg);
      break;
    case 's':
      SCHEDALG = string(optarg);
      break;
    case 'l':
      LOGFILE = string(optarg);
      break;
    default:
      cerr<< "usage: " << argv[0] << " [-p port] [-t threads] [-b buffers]" << endl;
      exit(1);
    }
  }

  set_log_file(LOGFILE);

  sync_print("init", "");
  MyServerSocket *server = new MyServerSocket(PORT);
  MySocket *client;

  services.push_back(new FileService(BASEDIR));

  // Create an empty FIFO queue of MySocket*, this is our job buffer
  // It is wrapped within the struct ThreadArgs, and it is the struct member called 'buffer'
  struct ThreadArgs* bufferPtr = new struct ThreadArgs;
  // This is a bounded buffer, and that bounding will be implemented by a conditional variable and 
  // a while loop condition to check current buffer size against BUFFER_SIZE


  // Spawn the number of worker/consumer threads indicated by the user
  // Give them a fcn pointer to the consumer fcn getFromBuffer and an argument containing a pointer to the buffer
  vector<pthread_t> threads(THREAD_POOL_SIZE); // Create a vector of predetermined size to store the pthread_t, although in current implementation this is unnecessary since the while(true) loop can never exit so we never need to join() or detatch()
  for (int i = 0; i < THREAD_POOL_SIZE; i++) {
    dthread_create(&threads[i], NULL, getFromBuffer, (void *)bufferPtr);
  }
  
  while(true) {
    sync_print("waiting_to_accept", "");
    client = server->accept();
    
    pushToBuffer(client, bufferPtr);


    sync_print("client_accepted", "");
    //handle_request(client);
  }
}
