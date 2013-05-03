/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <netdb.h>
#include <stdio.h>
#include <boost/thread.hpp>
#include "http-request.h"
#include "http-response.h"
#include <map>
#include <ctime>
#include <pthread.h>

using namespace std;

#define MAX_THREADS 10
#define MAX_CONNECTIONS 100
#define PROXY_PORT "14806"
#define BACKLOG 10
#define BUF_SIZE 100000

int thread_count = 0;
int connection_count = 0;

class CachedPage
{
public:
  CachedPage(const char* page_data, string url, HttpResponse* response, int content_len) {
    cerr << "CREATING NEW CACHEDPAGE" << endl;
    cerr << page_data << endl;
    cerr << "ABOVE IS PAGE DATA" << endl;
    this->page_data = new char[content_len];
    memcpy(this->page_data, page_data, content_len);
    cerr << this->page_data << endl;
    cerr << "ABOVE IS COPIED DATA with length " << content_len << endl;
    length = content_len;
    page_url = url;
    this->response = *response;
  }

  ~CachedPage() {
  };

  void delete_data() {
    delete [] page_data;
  }

  int expired() {
    string expires = response.FindHeader("Expires");
    struct tm tm;
    time_t t;
    time_t now;
    strptime(expires.c_str(), "%a, %d %b %Y %H:%M:%S %Z", &tm);
    t = mktime(&tm);
    now = time(0);
    now = mktime(gmtime(&now));
    cerr << "NOW IS " << asctime(gmtime(&now)) << endl;
    cerr << "EXPIRES IS " << asctime(&tm) << endl;
    if (t < (now - 3600)) {
      cerr << "IT IS EXPIRED" << endl;
      cerr << t << " t" << endl;
      cerr << now - 3600 << " now" << endl;
      return 1;
    } else {
      cerr << t << " t" << endl;
      cerr << now - 3600 << " now" << endl;
      return 0;
    }
  }

  void set_expires(string expires) {
    response.ModifyHeader("Expires", expires);
  }

  string get_last_modified() {
    return response.FindHeader("Last-Modified");
  }

  int get_response(char* response_buf) {
    cerr << "COPYING CACHE DATA" << endl;
    cerr << page_data << endl;
    response.FormatResponse(response_buf);
    cerr << response_buf << endl;
    memcpy(response_buf + response.GetTotalLength(), page_data, length);
    return response.GetTotalLength() + length;
  }
private:
  string page_url;
  HttpResponse response;
  char* page_data;
  int length;
};

class WebCache
{
public:
  WebCache() {}
  ~WebCache() {
    cache.clear();
  }

  void add_page(const char* page_data, string url, HttpResponse* response, int content_len) {
    cerr << "ADDING PAGE TO CACHE" << endl;
    cache.insert(map<string, CachedPage>::value_type(url, CachedPage(page_data, url, response, content_len)));
  }

  void remove_page(string URL) {
    cache.erase(URL);
  }

  CachedPage* find_page(string url) {
    map<string, CachedPage>::iterator iter;
    iter = cache.find(url);
    if (iter == cache.end()) {
      return NULL;
    } else {
      return &iter->second;
    }
  }
private:
  map<string, CachedPage> cache;
};

WebCache proxy_cache;

class Proxy 
{
public:
  Proxy() {}
  ~Proxy() {}

  void start_listening(string port) {
    struct addrinfo hints, *servinfo, *p;
    memset(&hints, 0, sizeof(hints));
    int yes = 1;

    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;  // Autofill IP

    int error;
    if ((error = getaddrinfo(NULL, port.c_str(), &hints, &servinfo)) != 0) {
      fprintf(stderr, "Error during getaddrinfo for listen socket: %s\n", gai_strerror(error));
      exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
      // try to open socket
      if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
        fprintf(stderr, "Error opening listen socket\n");
        continue;
      }
      fprintf(stderr, "opened listen socket\n");

      // use socket even if it is in use
      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
          fprintf(stderr, "Error with setsockopt for listen socket\n");
          exit(1);
      }

      // bind address and port to socket
      if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
          close(sockfd);
          fprintf(stderr, "Error binding with listen socket\n");
          continue;
      }
      fprintf(stderr, "binded to listen socket\n");

      break;  // break out of loop if everything is successful
    }

    if (p == NULL)  {
      fprintf(stderr, "Failure to create and bind socket\n");
      exit(1);
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, BACKLOG) == -1) {
        fprintf(stderr, "Error trying to listen on socket");
        close(sockfd);
        exit(1);
    }
    fprintf(stderr, "started listening on listen socket\n");
  }

  void stop_listening() {
    close(sockfd);
  }

  int accept_connection() {
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size = sizeof(client_addr);
    fprintf(stderr, "waiting to accept connection\n");
    return accept(sockfd, (struct sockaddr*) &client_addr, &client_addr_size);
  }

private:
  int sockfd;
};

class ClientConnection
{
public:
  ClientConnection(int sockfd) {
    this->sockfd = sockfd;
  }
  ~ClientConnection() {}

  int receive(char* buf, size_t size, int flags) {
    cerr << "Waiting for request from client" << endl;
    return recv(sockfd, buf, size, flags);
  }

  void send_response(char* response_buf, int response_len, int flags) {
    send(sockfd, response_buf, response_len, flags);
    cerr << "Sent response back to client:" << endl << response_buf << endl;
  }

  void close_socket() {
    close(sockfd);
  }

private:
  int sockfd;
};

class RemoteConnection
{
public:
  RemoteConnection(HttpRequest* request) {
    cerr << "Setting up remote connection..." << endl;
    struct addrinfo hints, *servinfo, *p;
    int error;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;

    char request_port[6];
    snprintf(request_port, 6, "%hu", request->GetPort());

    cerr <<  "Host is " << request->GetHost() << ":" << request_port << endl;
    if ((error = getaddrinfo(request->GetHost().c_str(), request_port, &hints, &servinfo)) != 0) {
      cerr << "Error during getaddrinfo for server socket: " << gai_strerror(error) << endl;
      exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
      // try to open socket
      if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
        cerr << "Error opening server socket" << endl;
        continue;
      }
      cerr <<  "opened server socket" << endl;

      // use socket even if it is in use
      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
          cerr << "Error with setsockopt for server socket" << endl;
          exit(1);
      }

      // bind address and port to socket
      if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
          close(sockfd);
          cerr << "Error connecting with server socket" << endl;
          continue;
      }
      cerr << "connected to server socket" << endl;
      break;  // break out of loop if everything is successful
    }
  }
  ~RemoteConnection() {}

  void close_socket() {
    close(sockfd);
  }

  int update_request(HttpRequest* request, char* response_buf, string host, CachedPage* cached_page) {
    HttpResponse response;
    char end_str[] = "\r\n\r\n";
    request->AddHeader("If-Modified-Since", cached_page->get_last_modified());
    int req_len = request->GetTotalLength();
    cerr << "WITH MOD" << endl;
    int header_len = 0;
    int payload_received = 0;
    int response_len;
    int content_len;
    char request_buf[req_len];

    string URL = host + request->GetPath();
    request->FormatRequest(request_buf);
    cerr << request_buf << endl;
    send(sockfd, request_buf, req_len, 0);
    memset(response_buf, 0, BUF_SIZE);
    while (!strstr(response_buf, end_str)) {
      header_len += recv(sockfd, response_buf + header_len, BUF_SIZE - header_len, 0);
    }
    payload_received = strlen(response.ParseResponse(response_buf, header_len));
    cerr << "STATUS CODE IS: " << response.GetStatusCode() << endl;
    if (response.GetStatusCode() == "304") {
      cerr << "NOT MODIFIED, UPDATING EXPIRES" << endl;
      cached_page->set_expires(response.FindHeader("Expires"));
      return 0;
    } else {
      content_len = atoi(response.FindHeader("Content-Length").c_str());
      int close_connection = (response.FindHeader("Connection") == "close");
      header_len -= payload_received;

      cerr << "Content length is: " << content_len << ". Close connection is " << close_connection << endl;
      while(close_connection || (payload_received < content_len)) {
        response_len = recv(sockfd, response_buf + header_len + payload_received, BUF_SIZE - header_len - payload_received, 0);
        payload_received += response_len;
        cerr << "Received " << payload_received << " so far" << endl;
        if (close_connection && response_len == 0) {
          break;
        }
      }

      if (close_connection) {
        cerr << "Remote host wants to close connection!" << endl;
        close(sockfd);
        cerr << "Closed connection with remote host" << endl;
      }
      
      cerr << "Received response from server of size " << header_len + payload_received << endl;
      return header_len + payload_received;
    }
  }

  int send_request(HttpRequest* request, char* response_buf, string host) {
    HttpResponse response;
    char end_str[] = "\r\n\r\n";
    int req_len = request->GetTotalLength();
    int header_len = 0;
    int payload_received = 0;
    int response_len;
    int content_len;
    char request_buf[req_len];

    string URL = host + request->GetPath();
    cerr << "The requested URL is " << URL << endl;

    request->FormatRequest(request_buf);
    cerr << "Formatted request to be send to server" << endl << request_buf << endl;
    send(sockfd, request_buf, req_len, 0);
    cerr << "Sent request to server" << endl << request_buf << endl;
    memset(response_buf, 0, BUF_SIZE);
    while (!strstr(response_buf, end_str)) {
      header_len += recv(sockfd, response_buf + header_len, BUF_SIZE - header_len, 0);
    }
    cerr << "Received response from server" << endl << response_buf << endl;
    payload_received = strlen(response.ParseResponse(response_buf, header_len));
    content_len = atoi(response.FindHeader("Content-Length").c_str());
    int close_connection = (response.FindHeader("Connection") == "close");
    header_len -= payload_received;

    cerr << "Content length is: " << content_len << ". Close connection is " << close_connection << endl;
    while(close_connection || (payload_received < content_len)) {
      response_len = recv(sockfd, response_buf + header_len + payload_received, BUF_SIZE - header_len - payload_received, 0);
      payload_received += response_len;
      cerr << "Received " << payload_received << " so far" << endl;
      if (close_connection && response_len == 0) {
        break;
      }
    }

    if (close_connection) {
      cerr << "Remote host wants to close connection!" << endl;
      close(sockfd);
      cerr << "Closed connection with remote host" << endl;
    }
    
    cerr << "Received response from server of size " << header_len + payload_received << endl;
    return header_len + payload_received;
  }

private:
  int sockfd;
};

void serve_request(int client_socket)
{
  ClientConnection client_connection = ClientConnection(client_socket);
  map<string, RemoteConnection>::iterator iter;
  map<string, RemoteConnection> server_map;
  char request_buf[BUF_SIZE];
  char response_buf[BUF_SIZE];
  int req_len;  // request length in bytes
  HttpRequest request;
  HttpResponse response;
  int response_len;

  while (1) {
    memset(request_buf, 0, BUF_SIZE);
    memset(response_buf, 0, BUF_SIZE);
    if ((req_len = client_connection.receive(request_buf, BUF_SIZE, 0)) == 0) {
      fprintf(stderr, "Client closed socket\n");
      break;
    }
    fprintf(stderr, "Received request:\n%s\n\n", request_buf);

    try {
      request.ParseRequest(request_buf, req_len);
      fprintf(stderr, "Parsed request\n");
    } catch (ParseException e) {
      fprintf(stderr, "Exception attempting to parse: %s\n", e.what());
      return;
    }

    char request_port[6];
    snprintf(request_port, 6, "%hu", request.GetPort());
    string host = request.GetHost();
    host = host.append(":").append(request_port);
    string URL = host + request.GetPath();
    CachedPage* cached_page;

    if (!(cached_page = proxy_cache.find_page(URL))) {
      cerr << "PAGE NOT FOUND IN CACHE!" << endl;
      iter = server_map.find(host);
      if (iter == server_map.end()) {
        cerr << "Inserting " << host << " into server_map" << endl;
        iter = server_map.insert(map<string, RemoteConnection>::value_type(host, RemoteConnection(&request))).first;
      } else {
        cerr << "Connection found in server map" << endl;
      }

      cerr << "Host is " << host << " and path is " << request.GetPath() << endl;
      response_len = iter->second.send_request(&request, response_buf, host);
      cerr << "content length is " << response.FindHeader("Content-Length") << endl;
      const char* page_data = response.ParseResponse(response_buf, response_len);
      proxy_cache.add_page(page_data, URL, &response, atoi(response.FindHeader("Content-Length").c_str()));
      if (response.FindHeader("Connection") == "close") {
        server_map.erase(iter);
        connection_count--;
      }
    } else {
      response_len = cached_page->get_response(response_buf);
      if (cached_page->expired()) {
        cerr << "PAGE EXPIRED!!" << endl;
        iter = server_map.find(host);
        if (iter == server_map.end()) {
          cerr << "Inserting " << host << " into server_map" << endl;
          iter = server_map.insert(map<string, RemoteConnection>::value_type(host, RemoteConnection(&request))).first;
        } else {
          cerr << "Connection found in server map" << endl;
        }
        cerr << "Host is " << host << " and path is " << request.GetPath() << endl;
        response_len = iter->second.update_request(&request, response_buf, host, cached_page);
        if (response_len) {
          const char* page_data = response.ParseResponse(response_buf, response_len);
          cached_page->delete_data();
          proxy_cache.remove_page(URL);
          proxy_cache.add_page(page_data, URL, &response, atoi(response.FindHeader("Content-Length").c_str()));
        } else {
          response_len = cached_page->get_response(response_buf);
        }
      }
      cerr << "PAGE FOUND IN CACHE!" << endl;
      cerr << response_buf << endl << endl;
    }
    client_connection.send_response(response_buf, response_len, 0);
  }

  for (iter = server_map.begin(); iter != server_map.end(); ++iter) {
    iter->second.close_socket();
  }
  server_map.clear();

  fprintf(stderr, "Closing connection with client\n");
  client_connection.close_socket();
  return;
}

int main (int argc, char *argv[])
{
  proxy_cache = WebCache();
  Proxy proxy = Proxy();
  proxy.start_listening(PROXY_PORT);
  int client_socket;  // socket that clients connect to

  while (1) {
    client_socket = proxy.accept_connection();
    cerr << "Accepted connection" << endl;
    serve_request(client_socket);
  }
  
  // Should never reach this
  proxy.stop_listening();
  cerr << "Exiting" << endl;
  return 0;
}
