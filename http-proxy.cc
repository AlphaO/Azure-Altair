/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <netdb.h>
#include <pthread.h>

using namespace std;

#define MAX_THREADS 10
#define PROXY_PORT "14805"
#define BACKLOG 10

pthread_cond_t max_threads_cond;
pthread_mutex_t num_threads_mutex;
int num_threads = 0;

int main (int argc, char *argv[])
{
  // Initialize mutexes
  pthread_cond_init(&max_threads_cond, NULL);
  pthread_mutex_init(&num_threads_mutex, NULL);

  struct addrinfo hints, *servinfo;
  memset(&hints, 0, sizeof(hints));
  int server_sock_fd, client_sock_fd;

  if (getaddrinfo(NULL, PROXY_PORT, &hints, &servinfo) != 0) {
    cerr << "Error getting address info" << endl;
    exit(1);
  }

  hints.ai_family = AF_INET;  // IPv4
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;  // Autofill in IP

  // open socket
  cerr << "opening socket" << endl;
  if ((server_sock_fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol)) == -1) {
    cerr << "Error opening socket" << endl;
    exit(1);
  }


  // start listening 
  cerr << "binding socket" << endl;
  if (bind(server_sock_fd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
    cerr << "Error binding socket" << endl;
    close(server_sock_fd);
    exit(1);
  }

  freeaddrinfo(servinfo);

  cerr << "start listening on socket" << endl;
  if (listen(server_sock_fd, BACKLOG) == -1) {
    cerr << "Error listening on socket" << endl;
    close(server_sock_fd);
    exit(1);
  }

  while (1) {
    // Make sure we don't create more than MAX_THREADS threads.
    pthread_mutex_lock(&num_threads_mutex);
    if (num_threads == MAX_THREADS) {
      pthread_cond_wait(&max_threads_cond, &num_threads_mutex);
      num_threads++;
    }
    pthread_mutex_unlock(&num_threads_mutex);

    struct sockaddr_storage client_addr;
    socklen_t client_addr_size = sizeof(client_addr);
    cerr << "waiting to accept connection" << endl;
    client_sock_fd = accept(server_sock_fd, (struct sockaddr*) &client_addr, &client_addr_size);
    cerr << "accepted connection" << endl;

    if (client_sock_fd) {
      cout << "somehow true" << endl;
    }
    //pthread_t service_thread;
    //pthread_create(&service_thread, NULL, serve_request, (void *) &client_sock_fd);
  }
  
  cerr << "exiting" << endl;
  return 0;
}
