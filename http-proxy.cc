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
#include <boost/thread.hpp>

using namespace std;

#define MAX_THREADS 10
#define MAX_CONNECTIONS 100
#define PROXY_PORT "14805"
#define BACKLOG 10
#define BUF_SIZE 100000

boost::mutex thread_count_lock;
boost::condition_variable thread_count_cond_var;
boost::mutex connection_count_lock;
int thread_count = 0;
int connection_count = 0;

class CachedPage
{
public:
  CachedPage(const char* page_data, string url, HttpResponse* response, int content_len) {
    page_lock = new boost::mutex();
    this->page_data = new char[content_len];
    memcpy(this->page_data, page_data, content_len);
    length = content_len;
    page_url = url;
    this->response = *response;
  }

  ~CachedPage() {
    boost::mutex::scoped_lock lock(*page_lock);
  };

  void delete_data() {
    {
      boost::mutex::scoped_lock lock(*page_lock);
      delete [] page_data;
    }
  }

  int expired() {
    string expires;
    {
      boost::mutex::scoped_lock lock(*page_lock);
      expires = response.FindHeader("Expires");
    }
    struct tm tm;
    time_t t;
    time_t now;
    strptime(expires.c_str(), "%a, %d %b %Y %H:%M:%S %Z", &tm);
    t = mktime(&tm);
    now = time(0);
    now = mktime(gmtime(&now));
    if (t < (now - 3600)) {
      return 1;
    } else {
      return 0;
    }
  }

  void set_expires(string expires) {
    boost::mutex::scoped_lock lock(*page_lock);
    response.ModifyHeader("Expires", expires);
  }

  string get_last_modified() {
    boost::mutex::scoped_lock lock(*page_lock);
    return response.FindHeader("Last-Modified");
  }

  int get_response(char* response_buf) {
    boost::mutex::scoped_lock lock(*page_lock);
    response.FormatResponse(response_buf);
    memcpy(response_buf + response.GetTotalLength(), page_data, length);
    return response.GetTotalLength() + length;
  }
private:
  boost::mutex *page_lock;
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
    boost::mutex::scoped_lock lock(cache_lock);
    response->RemoveHeader("Connection");
    cache.insert(map<string, CachedPage>::value_type(url, CachedPage(page_data, url, response, content_len)));
  }

  void remove_page(string URL) {
    boost::mutex::scoped_lock lock(cache_lock);
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
  boost::mutex cache_lock;
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
      exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
      // try to open socket
      if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
        continue;
      }

      // use socket even if it is in use
      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
          exit(1);
      }

      // bind address and port to socket
      if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
          close(sockfd);
          continue;
      }

      break;  // break out of loop if everything is successful
    }

    if (p == NULL)  {
      exit(1);
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, BACKLOG) == -1) {
        close(sockfd);
        exit(1);
    }
  }

  void stop_listening() {
    close(sockfd);
  }

  int accept_connection() {
    struct sockaddr_storage client_addr;
    socklen_t client_addr_size = sizeof(client_addr);
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
    return recv(sockfd, buf, size, flags);
  }

  void send_response(char* response_buf, int response_len, int flags) {
    send(sockfd, response_buf, response_len, flags);
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
    struct addrinfo hints, *servinfo, *p;
    int error;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;

    char request_port[6];
    snprintf(request_port, 6, "%hu", request->GetPort());

    if ((error = getaddrinfo(request->GetHost().c_str(), request_port, &hints, &servinfo)) != 0) {
      exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
      // try to open socket
      if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
        continue;
      }

      // use socket even if it is in use
      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
          exit(1);
      }

      // bind address and port to socket
      if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
          close(sockfd);
          continue;
      }
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
    int header_len = 0;
    int payload_received = 0;
    int response_len;
    int content_len;
    char request_buf[req_len];

    string URL = host + request->GetPath();
    request->FormatRequest(request_buf);
    send(sockfd, request_buf, req_len, 0);
    memset(response_buf, 0, BUF_SIZE);
    while (!strstr(response_buf, end_str)) {
      header_len += recv(sockfd, response_buf + header_len, BUF_SIZE - header_len, 0);
    }
    payload_received = strlen(response.ParseResponse(response_buf, header_len));
    if (response.GetStatusCode() == "304") {
      cached_page->set_expires(response.FindHeader("Expires"));
      return 0;
    } else {
      content_len = atoi(response.FindHeader("Content-Length").c_str());
      int close_connection = (response.FindHeader("Connection") == "close");
      header_len -= payload_received;

      while(close_connection || (payload_received < content_len)) {
        response_len = recv(sockfd, response_buf + header_len + payload_received, BUF_SIZE - header_len - payload_received, 0);
        payload_received += response_len;
        if (close_connection && response_len == 0) {
          break;
        }
      }

      if (close_connection) {
        close(sockfd);
      }
      
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

    request->FormatRequest(request_buf);
    send(sockfd, request_buf, req_len, 0);
    memset(response_buf, 0, BUF_SIZE);
    while (!strstr(response_buf, end_str)) {
      header_len += recv(sockfd, response_buf + header_len, BUF_SIZE - header_len, 0);
    }
    payload_received = strlen(response.ParseResponse(response_buf, header_len));
    content_len = atoi(response.FindHeader("Content-Length").c_str());
    int close_connection = (response.FindHeader("Connection") == "close");
    header_len -= payload_received;

    while(close_connection || (payload_received < content_len)) {
      response_len = recv(sockfd, response_buf + header_len + payload_received, BUF_SIZE - header_len - payload_received, 0);
      payload_received += response_len;
      if (close_connection && response_len == 0) {
        break;
      }
    }

    if (close_connection) {
      close(sockfd);
    }
    
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
    req_len = 0;
    int curr_len = 1;
    memset(request_buf, 0, BUF_SIZE);
    memset(response_buf, 0, BUF_SIZE);
    if ((req_len = client_connection.receive(request_buf, BUF_SIZE, 0)) == 0) {
      break;
    }
    while (!strstr(request_buf, "\r\n\r\n") && curr_len) {
      curr_len = client_connection.receive(request_buf - req_len, BUF_SIZE - req_len, 0);
      req_len += curr_len;
    }

    if (req_len == 0) {
      break;
    }


    try {
      request.ParseRequest(request_buf, req_len);
    } catch (ParseException e) {
      return;
    }

    char request_port[6];
    snprintf(request_port, 6, "%hu", request.GetPort());
    string host = request.GetHost();
    host = host.append(":").append(request_port);
    string URL = host + request.GetPath();
    CachedPage* cached_page;

    if (!(cached_page = proxy_cache.find_page(URL))) {
      iter = server_map.find(host);
      if (iter == server_map.end()) {
        {
          boost::mutex::scoped_lock lock(connection_count_lock);
          if (connection_count >= 100) {
            continue;
          } else {
            connection_count++;
          }
        }
        iter = server_map.insert(map<string, RemoteConnection>::value_type(host, RemoteConnection(&request))).first;
      }

      response_len = iter->second.send_request(&request, response_buf, host);
      const char* page_data = response.ParseResponse(response_buf, response_len);
      proxy_cache.add_page(page_data, URL, &response, atoi(response.FindHeader("Content-Length").c_str()));
      if (response.FindHeader("Connection") == "close") {
        server_map.erase(iter);
        {
          boost::mutex::scoped_lock lock(connection_count_lock);
          connection_count--;
        }
      }
    } else {
      response_len = cached_page->get_response(response_buf);
      if (cached_page->expired()) {
        iter = server_map.find(host);
        if (iter == server_map.end()) {
          iter = server_map.insert(map<string, RemoteConnection>::value_type(host, RemoteConnection(&request))).first;
          {
            boost::mutex::scoped_lock lock(connection_count_lock);
            connection_count++;
          } 
        } else {
        }
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
    }
    client_connection.send_response(response_buf, response_len, 0);
  }

  for (iter = server_map.begin(); iter != server_map.end(); ++iter) {
    iter->second.close_socket();
  }
  {
    boost::mutex::scoped_lock lock(connection_count_lock);
    connection_count -= server_map.size();
  }
  server_map.clear();

  client_connection.close_socket();
  {
    boost::mutex::scoped_lock lock(thread_count_lock);
    thread_count--;
    thread_count_cond_var.notify_all();
  }
  return;
}

int main (int argc, char *argv[])
{
  Proxy proxy = Proxy();
  proxy.start_listening(PROXY_PORT);
  int client_socket;  // socket that clients connect to

  while (1) {
    {
      boost::mutex::scoped_lock lock(thread_count_lock);
      while (thread_count >= 10)
        thread_count_cond_var.wait(lock);
      client_socket = proxy.accept_connection();
      thread_count++;
    }
    boost::thread(serve_request, client_socket);
  }
  
  // Should never reach this
  proxy.stop_listening();
  return 0;
}
