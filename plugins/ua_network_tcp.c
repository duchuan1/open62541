/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

#include "ua_network_tcp.h"

#include <stdlib.h> // malloc, free
#include <stdio.h> // snprintf
#include <string.h> // memset
#include <errno.h>

#ifdef _WIN32
# include <malloc.h>
# include <winsock2.h>
# include <ws2tcpip.h>
# define CLOSESOCKET(S) closesocket(S)
# define ssize_t long
#else
# include <fcntl.h>
# include <sys/select.h>
# include <netinet/in.h>
# ifndef __CYGWIN__
#  include <netinet/tcp.h>
# endif
# include <sys/ioctl.h>
# include <netdb.h> //gethostbyname for the client
# include <unistd.h> // read, write, close
# include <arpa/inet.h>
# ifdef __QNX__
#  include <sys/socket.h>
# endif
# define CLOSESOCKET(S) close(S)
#endif

/* workaround a glibc bug where an integer conversion is required */
#if !defined(_WIN32)
# if defined(__GNU_LIBRARY__) && (__GNU_LIBRARY__ >= 6) && (__GLIBC__ >= 2) && (__GLIBC_MINOR__ >= 16)
#  define UA_fd_set(fd, fds) FD_SET(fd, fds)
#  define UA_fd_isset(fd, fds) FD_ISSET(fd, fds)
# else
#  define UA_fd_set(fd, fds) FD_SET((unsigned int)fd, fds)
#  define UA_fd_isset(fd, fds) FD_ISSET((unsigned int)fd, fds)
# endif
#else
# define UA_fd_set(fd, fds) FD_SET((unsigned int)fd, fds)
# define UA_fd_isset(fd, fds) FD_ISSET((unsigned int)fd, fds)
#endif

#ifdef UA_ENABLE_MULTITHREADING
# include <urcu/uatomic.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/****************************/
/* Generic Socket Functions */
/****************************/

static void
socket_close(UA_Connection *connection) {
    connection->state = UA_CONNECTION_CLOSED;
    shutdown(connection->sockfd,2);
    CLOSESOCKET(connection->sockfd);
}

static UA_StatusCode
socket_write(UA_Connection *connection, UA_ByteString *buf) {
    size_t nWritten = 0;
    do {
        ssize_t n = 0;
        do {
        /*
         * If the OS throws EMSGSIZE, force a smaller packet size:
         *  size_t bytes_to_send = buf->length - nWritten >  1024 ? 1024 : buf->length - nWritten;
         */
            size_t bytes_to_send = buf->length - nWritten;
#ifdef _WIN32
            n = send((SOCKET)connection->sockfd, (const char*)buf->data + nWritten, bytes_to_send, 0);
            const int last_error = WSAGetLastError();
            if(n < 0 && last_error != WSAEINTR && last_error != WSAEWOULDBLOCK) {
                connection->close(connection);
                socket_close(connection);
                UA_ByteString_deleteMembers(buf);
                return UA_STATUSCODE_BADCONNECTIONCLOSED;
            }
#else
            n = send(connection->sockfd, (const char*)buf->data + nWritten, bytes_to_send, MSG_NOSIGNAL);
            if(n == -1L && errno != EINTR && errno != EAGAIN) {
                connection->close(connection);
                socket_close(connection);
                UA_ByteString_deleteMembers(buf);
                return UA_STATUSCODE_BADCONNECTIONCLOSED;
            }
#endif
        } while(n == -1L);
        nWritten += (size_t)n;
    } while(nWritten < buf->length);
    UA_ByteString_deleteMembers(buf);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
socket_recv(UA_Connection *connection, UA_ByteString *response, UA_UInt32 timeout) {
    response->data = malloc(connection->localConf.recvBufferSize);
    if(!response->data) {
        response->length = 0;
        return UA_STATUSCODE_BADOUTOFMEMORY; /* not enough memory retry */
    }

    if(timeout > 0) {
        /* currently, only the client uses timeouts */
#ifndef _WIN32
        UA_UInt32 timeout_usec = timeout * 1000;
    #ifdef __APPLE__
        struct timeval tmptv = {(long int)(timeout_usec / 1000000), timeout_usec % 1000000};
    #else
        struct timeval tmptv = {(long int)(timeout_usec / 1000000), (long int)(timeout_usec % 1000000)};
    #endif
        int ret = setsockopt(connection->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmptv, sizeof(struct timeval));
#else
        DWORD timeout_dw = timeout;
        int ret = setsockopt(connection->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_dw, sizeof(DWORD));
#endif
        if(0 != ret) {
            UA_ByteString_deleteMembers(response);
            socket_close(connection);
            return UA_STATUSCODE_BADCONNECTIONCLOSED;
        }
    }

#ifdef __CYGWIN__
    /* WORKAROUND for https://cygwin.com/ml/cygwin/2013-07/msg00107.html */
    ssize_t ret;

    if (timeout > 0) {
        fd_set fdset;
        UA_UInt32 timeout_usec = timeout * 1000;
    #ifdef __APPLE__
        struct timeval tmptv = {(long int)(timeout_usec / 1000000), timeout_usec % 1000000};
    #else
        struct timeval tmptv = {(long int)(timeout_usec / 1000000), (long int)(timeout_usec % 1000000)};
    #endif
        UA_Int32 retval;

        FD_ZERO(&fdset);
        UA_fd_set(connection->sockfd, &fdset);
        retval = select(connection->sockfd+1, &fdset, NULL, NULL, &tmptv);
        if(retval && UA_fd_isset(connection->sockfd, &fdset)) {
            ret = recv(connection->sockfd, (char*)response->data, connection->localConf.recvBufferSize, 0);
        } else {
            ret = 0;
        }
    } else {
        ret = recv(connection->sockfd, (char*)response->data, connection->localConf.recvBufferSize, 0);
    }
#else
    ssize_t ret = recv(connection->sockfd, (char*)response->data, connection->localConf.recvBufferSize, 0);
#endif

    if(ret == 0) {
        /* server has closed the connection */
        UA_ByteString_deleteMembers(response);
        socket_close(connection);
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    } else if(ret < 0) {
        UA_ByteString_deleteMembers(response);
#ifdef _WIN32
        const int last_error = WSAGetLastError();
        #define TEST_RETRY (last_error == WSAEINTR || (timeout > 0) ? 0 : (last_error == WSAEWOULDBLOCK))
#else
        #define TEST_RETRY (errno == EINTR || (timeout > 0) ? 0 : (errno == EAGAIN || errno == EWOULDBLOCK))
#endif
        if (TEST_RETRY)
            return UA_STATUSCODE_GOOD; /* retry */
        else {
            socket_close(connection);
            return UA_STATUSCODE_BADCONNECTIONCLOSED;
        }
    }
    response->length = (size_t)ret;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode socket_set_nonblocking(UA_Int32 sockfd) {
#ifdef _WIN32
    u_long iMode = 1;
    if(ioctlsocket(sockfd, FIONBIO, &iMode) != NO_ERROR)
        return UA_STATUSCODE_BADINTERNALERROR;
#else
    int opts = fcntl(sockfd, F_GETFL);
    if(opts < 0 || fcntl(sockfd, F_SETFL, opts|O_NONBLOCK) < 0)
        return UA_STATUSCODE_BADINTERNALERROR;
#endif
    return UA_STATUSCODE_GOOD;
}

static void FreeConnectionCallback(UA_Server *server, void *ptr) {
    UA_Connection_deleteMembers((UA_Connection*)ptr);
    free(ptr);
 }

/***************************/
/* Server NetworkLayer TCP */
/***************************/

/**
 * For the multithreaded mode, assume a single thread that periodically "gets work" from the network
 * layer. In addition, several worker threads are asynchronously calling into the callbacks of the
 * UA_Connection that holds a single connection.
 *
 * Creating a connection: When "GetWork" encounters a new connection, it creates a UA_Connection
 * with the socket information. This is added to the mappings array that links sockets to
 * UA_Connection structs.
 *
 * Reading data: In "GetWork", we listen on the sockets in the mappings array. If data arrives (or
 * the connection closes), a WorkItem is created that carries the work and a pointer to the
 * connection.
 *
 * Closing a connection: Closing can happen in two ways. Either it is triggered by the server in an
 * asynchronous callback. Or the connection is close by the client and this is detected in
 * "GetWork". The server needs to do some internal cleanups (close attached securechannels, etc.).
 * So even when a closed connection is detected in "GetWork", we trigger the server to close the
 * connection (with a WorkItem) and continue from the callback.
 *
 * - Server calls close-callback: We close the socket, set the connection-state to closed and add
 *   the connection to a linked list from which it is deleted later. The connection cannot be freed
 *   right away since other threads might still be using it.
 *
 * - GetWork: We remove the connection from the mappings array. In the non-multithreaded case, the
 *   connection is freed. For multithreading, we return a workitem that is delayed, i.e. that is
 *   called only after all workitems created before are finished in all threads. This workitems
 *   contains a callback that goes through the linked list of connections to be freed.
 *
 */

#define MAXBACKLOG 100

typedef struct {
    UA_ConnectionConfig conf;
    UA_UInt16 port;
    UA_Logger logger; // Set during start

    /* open sockets and connections */
    UA_Int32 serversockfd;
    size_t mappingsSize;
    struct ConnectionMapping {
        UA_Connection *connection;
        UA_Int32 sockfd;
    } *mappings;
} ServerNetworkLayerTCP;

static UA_StatusCode
ServerNetworkLayerGetSendBuffer(UA_Connection *connection, size_t length, UA_ByteString *buf) {
    if(length > connection->remoteConf.recvBufferSize)
        return UA_STATUSCODE_BADCOMMUNICATIONERROR;
    return UA_ByteString_allocBuffer(buf, length);
}

static void
ServerNetworkLayerReleaseSendBuffer(UA_Connection *connection, UA_ByteString *buf) {
    UA_ByteString_deleteMembers(buf);
}

static void
ServerNetworkLayerReleaseRecvBuffer(UA_Connection *connection, UA_ByteString *buf) {
    UA_ByteString_deleteMembers(buf);
}

/* after every select, we need to reset the sockets we want to listen on */
static UA_Int32
setFDSet(ServerNetworkLayerTCP *layer, fd_set *fdset) {
    FD_ZERO(fdset);
    UA_fd_set(layer->serversockfd, fdset);
    UA_Int32 highestfd = layer->serversockfd;
    for(size_t i = 0; i < layer->mappingsSize; i++) {
        UA_fd_set(layer->mappings[i].sockfd, fdset);
        if(layer->mappings[i].sockfd > highestfd)
            highestfd = layer->mappings[i].sockfd;
    }
    return highestfd;
}

/* callback triggered from the server */
static void
ServerNetworkLayerTCP_closeConnection(UA_Connection *connection) {
#ifdef UA_ENABLE_MULTITHREADING
    if(uatomic_xchg(&connection->state, UA_CONNECTION_CLOSED) == UA_CONNECTION_CLOSED)
        return;
#else
    if(connection->state == UA_CONNECTION_CLOSED)
        return;
    connection->state = UA_CONNECTION_CLOSED;
#endif
#if UA_LOGLEVEL <= 300
   //cppcheck-suppress unreadVariable
    ServerNetworkLayerTCP *layer = connection->handle;
    UA_LOG_INFO(layer->logger, UA_LOGCATEGORY_NETWORK, "Connection %i | Force closing the connection",
                connection->sockfd);
#endif
    /* only "shutdown" here. this triggers the select, where the socket is
       "closed" in the mainloop */
    shutdown(connection->sockfd, 2);
}

/* call only from the single networking thread */
static UA_StatusCode
ServerNetworkLayerTCP_add(ServerNetworkLayerTCP *layer, UA_Int32 newsockfd) {
    UA_Connection *c = malloc(sizeof(UA_Connection));
    if(!c)
        return UA_STATUSCODE_BADINTERNALERROR;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    int res = getpeername(newsockfd, (struct sockaddr*)&addr, &addrlen);
    if(res == 0) {
        UA_LOG_INFO(layer->logger, UA_LOGCATEGORY_NETWORK, "Connection %i | New connection over TCP from %s:%d",
                    newsockfd, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    } else {
        UA_LOG_WARNING(layer->logger, UA_LOGCATEGORY_NETWORK, "Connection %i | New connection over TCP, getpeername failed with errno %i",
                       newsockfd, errno);
    }
    UA_Connection_init(c);
    c->sockfd = newsockfd;
    c->handle = layer;
    c->localConf = layer->conf;
    c->send = socket_write;
    c->close = ServerNetworkLayerTCP_closeConnection;
    c->getSendBuffer = ServerNetworkLayerGetSendBuffer;
    c->releaseSendBuffer = ServerNetworkLayerReleaseSendBuffer;
    c->releaseRecvBuffer = ServerNetworkLayerReleaseRecvBuffer;
    c->state = UA_CONNECTION_OPENING;
    struct ConnectionMapping *nm;
    nm = realloc(layer->mappings, sizeof(struct ConnectionMapping)*(layer->mappingsSize+1));
    if(!nm) {
        UA_LOG_ERROR(layer->logger, UA_LOGCATEGORY_NETWORK, "No memory for a new Connection");
        free(c);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    layer->mappings = nm;
    layer->mappings[layer->mappingsSize] = (struct ConnectionMapping){c, newsockfd};
    layer->mappingsSize++;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
ServerNetworkLayerTCP_start(UA_ServerNetworkLayer *nl, UA_Logger logger) {
    ServerNetworkLayerTCP *layer = nl->handle;
    layer->logger = logger;

    /* get the discovery url from the hostname */
    UA_String du = UA_STRING_NULL;
    char hostname[256];
    if(gethostname(hostname, 255) == 0) {
        char discoveryUrl[256];
#ifndef _MSC_VER
        du.length = (size_t)snprintf(discoveryUrl, 255, "opc.tcp://%s:%d", hostname, layer->port);
#else
        du.length = (size_t)_snprintf_s(discoveryUrl, 255, _TRUNCATE,
                                        "opc.tcp://%s:%d", hostname, layer->port);
#endif
        du.data = (UA_Byte*)discoveryUrl;
    }
    UA_String_copy(&du, &nl->discoveryUrl);

    /* open the server socket */
#ifdef _WIN32
    if((layer->serversockfd = socket(PF_INET, SOCK_STREAM,0)) == (UA_Int32)INVALID_SOCKET) {
        UA_LOG_WARNING(layer->logger, UA_LOGCATEGORY_NETWORK, "Error opening socket, code: %d",
                       WSAGetLastError());
        return UA_STATUSCODE_BADINTERNALERROR;
    }
#else
    if((layer->serversockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        UA_LOG_WARNING(layer->logger, UA_LOGCATEGORY_NETWORK, "Error opening socket");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
#endif
    const struct sockaddr_in serv_addr =
        {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
         .sin_port = htons(layer->port), .sin_zero = {0}};
    int optval = 1;
    if(setsockopt(layer->serversockfd, SOL_SOCKET,
                  SO_REUSEADDR, (const char *)&optval, sizeof(optval)) == -1) {
        UA_LOG_WARNING(layer->logger, UA_LOGCATEGORY_NETWORK,
                       "Error during setting of socket options");
        CLOSESOCKET(layer->serversockfd);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    if(bind(layer->serversockfd, (const struct sockaddr *)&serv_addr,
            sizeof(serv_addr)) < 0) {
        UA_LOG_WARNING(layer->logger, UA_LOGCATEGORY_NETWORK, "Error during socket binding");
        CLOSESOCKET(layer->serversockfd);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    socket_set_nonblocking(layer->serversockfd);
    listen(layer->serversockfd, MAXBACKLOG);
    UA_LOG_INFO(layer->logger, UA_LOGCATEGORY_NETWORK, "TCP network layer listening on %.*s",
                nl->discoveryUrl.length, nl->discoveryUrl.data);
    return UA_STATUSCODE_GOOD;
}

static size_t
ServerNetworkLayerTCP_getJobs(UA_ServerNetworkLayer *nl, UA_Job **jobs, UA_UInt16 timeout) {
    ServerNetworkLayerTCP *layer = nl->handle;
    fd_set fdset, errset;
    UA_Int32 highestfd = setFDSet(layer, &fdset);
    setFDSet(layer, &errset);
    struct timeval tmptv = {0, timeout * 1000};
    UA_Int32 resultsize = select(highestfd+1, &fdset, NULL, &errset, &tmptv);
    if(resultsize < 0) {
        *jobs = NULL;
        return 0;
    }

    /* accept new connections (can only be a single one) */
    if(UA_fd_isset(layer->serversockfd, &fdset)) {
        resultsize--;
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int newsockfd = accept(layer->serversockfd, (struct sockaddr *) &cli_addr, &cli_len);
        int i = 1;
        if(newsockfd >= 0) {
            /* Send messages directly and do wait to merge packets (disable Nagle's algorithm) */
            setsockopt(newsockfd, IPPROTO_TCP, TCP_NODELAY, (void *)&i, sizeof(i));

            socket_set_nonblocking(newsockfd);
            ServerNetworkLayerTCP_add(layer, newsockfd);
        }
    }

    /* alloc enough space for a cleanup-connection and free-connection job per resulted socket */
    if(resultsize == 0)
        return 0;
    UA_Job *js = malloc(sizeof(UA_Job) * (size_t)resultsize * 2);
    if(!js)
        return 0;

    /* read from established sockets */
    size_t j = 0;
    UA_ByteString buf = UA_BYTESTRING_NULL;
    for(size_t i = 0; i < layer->mappingsSize && j < (size_t)resultsize; i++) {
        if(!UA_fd_isset(layer->mappings[i].sockfd, &errset) &&
           !UA_fd_isset(layer->mappings[i].sockfd, &fdset))
          continue;

        UA_StatusCode retval = socket_recv(layer->mappings[i].connection, &buf, 0);
        if(retval == UA_STATUSCODE_GOOD) {
            js[j].job.binaryMessage.connection = layer->mappings[i].connection;
            js[j].job.binaryMessage.message = buf;
            js[j].type = UA_JOBTYPE_BINARYMESSAGE_NETWORKLAYER;
            j++;
        } else if (retval == UA_STATUSCODE_BADCONNECTIONCLOSED) {
            UA_Connection *c = layer->mappings[i].connection;
            UA_LOG_INFO(layer->logger, UA_LOGCATEGORY_NETWORK,
                        "Connection %i | Connection closed from remote", c->sockfd);
            /* the socket was closed from remote */
            js[j].type = UA_JOBTYPE_DETACHCONNECTION;
            js[j].job.closeConnection = layer->mappings[i].connection;
            layer->mappings[i] = layer->mappings[layer->mappingsSize-1];
            layer->mappingsSize--;
            j++;
            js[j].type = UA_JOBTYPE_METHODCALL_DELAYED;
            js[j].job.methodCall.method = FreeConnectionCallback;
            js[j].job.methodCall.data = c;
            j++;
        }
    }

    if(j == 0) {
        free(js);
        js = NULL;
    }

    *jobs = js;
    return j;
}

static size_t
ServerNetworkLayerTCP_stop(UA_ServerNetworkLayer *nl, UA_Job **jobs) {
    ServerNetworkLayerTCP *layer = nl->handle;
    UA_LOG_INFO(layer->logger, UA_LOGCATEGORY_NETWORK,
                "Shutting down the TCP network layer with %d open connection(s)", layer->mappingsSize);
    shutdown(layer->serversockfd,2);
    CLOSESOCKET(layer->serversockfd);
    UA_Job *items = malloc(sizeof(UA_Job) * layer->mappingsSize * 2);
    if(!items)
        return 0;
    for(size_t i = 0; i < layer->mappingsSize; i++) {
        socket_close(layer->mappings[i].connection);
        items[i*2].type = UA_JOBTYPE_DETACHCONNECTION;
        items[i*2].job.closeConnection = layer->mappings[i].connection;
        items[(i*2)+1].type = UA_JOBTYPE_METHODCALL_DELAYED;
        items[(i*2)+1].job.methodCall.method = FreeConnectionCallback;
        items[(i*2)+1].job.methodCall.data = layer->mappings[i].connection;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    *jobs = items;
    return layer->mappingsSize*2;
}

/* run only when the server is stopped */
static void ServerNetworkLayerTCP_deleteMembers(UA_ServerNetworkLayer *nl) {
    ServerNetworkLayerTCP *layer = nl->handle;
    free(layer->mappings);
    free(layer);
    UA_String_deleteMembers(&nl->discoveryUrl);
}

UA_ServerNetworkLayer
UA_ServerNetworkLayerTCP(UA_ConnectionConfig conf, UA_UInt16 port) {
#ifdef _WIN32
    WORD wVersionRequested;
    WSADATA wsaData;
    wVersionRequested = MAKEWORD(2, 2);
    WSAStartup(wVersionRequested, &wsaData);
#endif

    UA_ServerNetworkLayer nl;
    memset(&nl, 0, sizeof(UA_ServerNetworkLayer));
    ServerNetworkLayerTCP *layer = calloc(1,sizeof(ServerNetworkLayerTCP));
    if(!layer)
        return nl;
    
    layer->conf = conf;
    layer->port = port;

    nl.handle = layer;
    nl.start = ServerNetworkLayerTCP_start;
    nl.getJobs = ServerNetworkLayerTCP_getJobs;
    nl.stop = ServerNetworkLayerTCP_stop;
    nl.deleteMembers = ServerNetworkLayerTCP_deleteMembers;
    return nl;
}

/***************************/
/* Client NetworkLayer TCP */
/***************************/

static UA_StatusCode
ClientNetworkLayerGetBuffer(UA_Connection *connection, size_t length, UA_ByteString *buf) {
    if(length > connection->remoteConf.recvBufferSize)
        return UA_STATUSCODE_BADCOMMUNICATIONERROR;
    if(connection->state == UA_CONNECTION_CLOSED)
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    return UA_ByteString_allocBuffer(buf, connection->remoteConf.recvBufferSize);
}

static void
ClientNetworkLayerReleaseBuffer(UA_Connection *connection, UA_ByteString *buf) {
    UA_ByteString_deleteMembers(buf);
}

static void
ClientNetworkLayerClose(UA_Connection *connection) {
#ifdef UA_ENABLE_MULTITHREADING
    if(uatomic_xchg(&connection->state, UA_CONNECTION_CLOSED) == UA_CONNECTION_CLOSED)
        return;
#else
    if(connection->state == UA_CONNECTION_CLOSED)
        return;
    connection->state = UA_CONNECTION_CLOSED;
#endif
    socket_close(connection);
}

/* we have no networklayer. instead, attach the reusable buffer to the handle */
UA_Connection
UA_ClientConnectionTCP(UA_ConnectionConfig localConf, const char *endpointUrl, UA_Logger logger) {
    UA_Connection connection;
    UA_Connection_init(&connection);
    connection.localConf = localConf;

    //socket_set_nonblocking(connection.sockfd);
    connection.send = socket_write;
    connection.recv = socket_recv;
    connection.close = ClientNetworkLayerClose;
    connection.getSendBuffer = ClientNetworkLayerGetBuffer;
    connection.releaseSendBuffer = ClientNetworkLayerReleaseBuffer;
    connection.releaseRecvBuffer = ClientNetworkLayerReleaseBuffer;

    size_t urlLength = strlen(endpointUrl);
    if(urlLength < 11 || urlLength >= 512) {
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "Server url size invalid");
        return connection;
    }
    if(strncmp(endpointUrl, "opc.tcp://", 10) != 0) {
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "Server url does not begin with opc.tcp://");
        return connection;
    }

    UA_UInt16 portpos = 9;
    UA_UInt16 port;
    for(port = 0; portpos < urlLength-1; portpos++) {
        if(endpointUrl[portpos] == ':') {
            char *endPtr = NULL;
            unsigned long int tempulong = strtoul(&endpointUrl[portpos+1], &endPtr, 10);
            if (ERANGE != errno && tempulong < UINT16_MAX && endPtr != &endpointUrl[portpos+1])
                port = (UA_UInt16)tempulong;
            break;
        }
    }
    if(port == 0) {
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "Port invalid");
        return connection;
    }

    char hostname[512];
    for(int i=10; i < portpos; i++)
        hostname[i-10] = endpointUrl[i];
    hostname[portpos-10] = 0;
#ifdef _WIN32
    WORD wVersionRequested;
    WSADATA wsaData;
    wVersionRequested = MAKEWORD(2, 2);
    WSAStartup(wVersionRequested, &wsaData);
    if((connection.sockfd = socket(PF_INET, SOCK_STREAM,0)) == (UA_Int32)INVALID_SOCKET) {
#else
    if((connection.sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
#endif
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "Could not create socket");
        return connection;
    }
    struct hostent *server = gethostbyname(hostname);
    if(!server) {
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "DNS lookup of %s failed", hostname);
        return connection;
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    memcpy((char *)&server_addr.sin_addr.s_addr, (char *)server->h_addr_list[0], (size_t)server->h_length);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    connection.state = UA_CONNECTION_OPENING;
    if(connect(connection.sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        ClientNetworkLayerClose(&connection);
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "Connection failed");
        return connection;
    }

#ifdef SO_NOSIGPIPE
    int val = 1;
    if(setsockopt(connection.sockfd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(val)) < 0) {
        UA_LOG_WARNING((*logger), UA_LOGCATEGORY_NETWORK, "Couldn't set SO_NOSIGPIPE");
        return connection;
    }
#endif

    return connection;
}
