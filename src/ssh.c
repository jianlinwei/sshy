#include "ssh.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "connect.h"
#include "log.h"

static int ssh_initialized = 0;

static int connectToServer(const char *hostname, int port);

void handleTunnelClient(int clientSocket,const char *targetHost,
                        const int targetPort, struct sockaddr *clientAddress, int clientAddressLen);

struct sshSession *createSshSession();

int ssh_connect(struct sshSession *sshSession, int sockfd, const char *host, int port) {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel = NULL;
    int rc;
    char *userauthlist;
    
    if (!ssh_initialized) {
        ssh_initialized = 1;
        libssh2_init(0);
    }
    
    session = libssh2_session_init();
    rc = libssh2_session_startup(session, sockfd);
    
    userauthlist = libssh2_userauth_list(session, sshSession->username, strlen(sshSession->username));
    
    // TODO: obey userauthlist
    
    sshy_log( "authenticating %s/%s\n", sshSession->username, sshSession->password);
    
    if (libssh2_userauth_password(session, sshSession->username, sshSession->password)) {
        sshy_log( "authentication error\n");
        return -1;
    }
    
    sshy_log( "opening tcpchannel to %s:%d\n", host, port);
    
    channel = libssh2_channel_direct_tcpip(session, host, port);
    
    sshSession->session = session;
    sshSession->channel = channel;
    sshSession->blocking = 1;
    sshSession->peekDataRead = 0;
    sshSession->peekData = '\0';
   
    
    if (channel != NULL) {
        sshy_log( "i've got the channel!!!!\n");
        return 0;
    } else {
        sshy_log( "couldn't get the channel!!!!\n");
        return -1;
    }
}

void ssh_free(struct sshSession *sshSession) {
    libssh2_channel_free(sshSession->channel);
    libssh2_session_disconnect(sshSession->session, "Client disconnecting normally");
    libssh2_session_free(sshSession->session);
}

ssize_t ssh_write(struct sshSession *sshSession, const char *buf, size_t buflen) {
    ssize_t ret;
    
    //libssh2_channel_set_blocking(sshSession->channel, 1);
   
    ret = libssh2_channel_write(sshSession->channel, buf, buflen);
    
    //libssh2_channel_set_blocking(sshSession->channel, 1);
    
    if (ret == LIBSSH2_ERROR_EAGAIN) {
        ret = -EAGAIN;
    }
    
    return ret;
}

ssize_t ssh_read(struct sshSession *sshSession, char *buf, size_t buflen) {
    ssize_t ret;
    
    if (sshSession->peekDataRead) {
        if (buflen > 0) {
            sshSession->peekDataRead = 0;
            buf[0] = sshSession->peekData;
            return 1;
        } else {
            return 0;
        }
    }
        
    
    //libssh2_channel_set_blocking(sshSession->channel, 1);
   
    ret = libssh2_channel_read(sshSession->channel, buf, buflen);
    
    if (ret == LIBSSH2_ERROR_EAGAIN) {
        ret = -1;
        errno = EAGAIN;
    }
        
    if (ret > 0) {
        //sshy_log( "ssh_read got: %s\n", buf);
    } else {
        sshy_log( "ssh_read error: %d\n", ret);
        
        if (libssh2_channel_eof(sshSession->channel)) {
            sshy_log( "ssh_read eof\n");
        } else {
            sshy_log( "ssh_read no eof\n");
        }   
    }
    
    //libssh2_channel_set_blocking(sshSession->channel, 1);
    
    return ret;
}

int ssh_read_peek(struct sshSession *sshSession, char *buf, size_t buflen) {
  
    int ret;
    
    if (!sshSession->peekDataRead) {
        ret = libssh2_channel_read(sshSession->channel, &sshSession->peekData, 1);
        if (ret > 0) {
            sshSession->peekDataRead = 1;
        }
    }
    
    if (sshSession->peekDataRead) {
        if (buflen > 0) {
            buf[0] = sshSession->peekData;
            return 1;
        }
    } 
    return 0;
    
}

int ssh_read_poll(struct sshSession *sshSession, int blocking) {
    int ret;
    
    if (sshSession->peekDataRead) {
        return 1;
    }
    
    libssh2_channel_set_blocking(sshSession->channel, 0);
    
    ret = libssh2_channel_read(sshSession->channel, &sshSession->peekData, 1);
    
    libssh2_channel_set_blocking(sshSession->channel, blocking);
    
    sshy_log( "read poll ret: %d ____________-------------_______________-\n", ret);
    
    if (ret == 1) {
        sshSession->peekDataRead = 1;
    }
        
    
    return ret == 1 || ret == 0;
}

void ssh_set_block(struct sshSession *sshSession, int blocking) {
    if (sshSession->channel) {
        sshy_log( "setting channel to %s\n", blocking ? "blocking" : "non blocking");
        libssh2_channel_set_blocking(sshSession->channel, blocking);
    }
}

int createListenSocket(int *port) {
    struct sockaddr_in serv_addr;
    int sockfd;
    socklen_t addrlen;
    
    sockfd = real_socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        sshy_log( "cannot bind to ssh forward port, socket: %u\n", sockfd);
        return -1;
    }
    
    listen(sockfd,5);

    addrlen = sizeof(serv_addr);
    
    getsockname(sockfd, (struct sockaddr *) &serv_addr, &addrlen);
    *port = serv_addr.sin_port;
    
    return sockfd;
}

int tunnelPort(const char *targetHost, const int targetPort) {
    int listenSocket;
    int listenPort;
    struct sockaddr_in sin;
    socklen_t sinlen = sizeof(sin);
    pid_t p;
    
    
    listenSocket = createListenSocket(&listenPort);
        
    sshy_log("creating tunnel to %s:%d @ port %d\n", targetHost, targetPort, listenPort);

    
    if ((p = fork()) == 0) { // no zombies please
        int clientSocket;
        
        sshy_log("about to accept. %d\n", getpid());
        
        if((clientSocket = accept(listenSocket, (struct sockaddr *)&sin, &sinlen)) > 0) {
            real_close(listenSocket);
            sshy_log( "client socket: %d\n", clientSocket);
            
            handleTunnelClient(clientSocket, targetHost, targetPort, (struct sockaddr *) &sin, sinlen);
            real_close(clientSocket);
        }
    
        _exit(0);
    }
    
    real_close(listenSocket);
    return listenPort;
}

static const char *inet46_ntoa(struct sockaddr *sin, char *buf, int bufsize) {
   return inet_ntop(sin->sa_family, sin, buf, bufsize);
}

static unsigned short inet46_ntohs(struct sockaddr *sin) {
    if (sin->sa_family == AF_INET) {
        return ntohs(((struct sockaddr_in *)sin)->sin_port);
    } else {
        return 0;
    }
}

#define STOP { stopped = 1; break; }

void handleTunnelClient(int clientSocket,const char *targetHost,
                        const int targetPort, struct sockaddr *clientAddress, int clientAddressLen) {
    struct sshSession * sshSession = createSshSession();
    fd_set fds;
    struct timeval tv;
    int stopped = 0;
    char buf[16384];
    int rc;
    ssize_t len, wr;
    char shost[200];
    unsigned short sport;
    int i;
    
    if (ssh_connect(sshSession, sshSession->fd, targetHost, targetPort)) {
        sshy_log( "ssh_connect error\n");
        return;
    }
    
    libssh2_session_set_blocking(sshSession->session, 0);

    inet46_ntoa(clientAddress, shost, sizeof(shost));
    sport = inet46_ntohs(clientAddress);
    
    sshy_log( "client socket: %d\n", clientSocket);
    
    while (! stopped) {
        FD_ZERO(&fds);
        FD_SET(clientSocket, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        rc = select(clientSocket + 1, &fds, NULL, NULL, &tv);
        
        if (-1 == rc) {
                
                break;
        }
        
        if (rc && FD_ISSET(clientSocket, &fds)) {
            int len = recv(clientSocket, buf, sizeof(buf), 0);
            if (len < 0) {
                break;
            } else if (0 == len) {
                sshy_log( "The client at %s:%d disconnected!\n", shost,
                    sport);
                break;
            }
            wr = 0;
            
            ssh_write(sshSession, buf, len);
              
        }
        while (1) {
            len = ssh_read(sshSession, buf, sizeof(buf));

            if (-1 == len && errno == EAGAIN) {
                break;
            } else if (len < 0) {
                sshy_log( "libssh2_channel_read: %d", (int)len);
                STOP;
            } else if (len == 0) {
                STOP;
            }
            wr = 0;
            while (wr < len) {
                i = send(clientSocket, buf + wr, len - wr, 0);
                if (i <= 0) {
                    STOP;
                }
                wr += i;
            }
        }
    
    }
    
    real_close(clientSocket);
    
}



static int connectToServer(const char *hostname, int port) {
    int sockfd;
    struct addrinfo *addrinfo;
    char portstring[10];
    
    snprintf(portstring, sizeof(portstring), "%d", port);
    
    if (getaddrinfo(hostname, portstring, NULL, &addrinfo)) {
        sshy_log("ERROR, no such host as %s\n", hostname);
        return -1;
    }

    sockfd = real_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        freeaddrinfo(addrinfo);
        return -1;
    }
    
    /* connect: create a connection with the server */
    if (real_connect(sockfd, addrinfo->ai_addr, addrinfo->ai_addrlen) < 0) {
        close(sockfd);
        sockfd = -1;
    }
    
    freeaddrinfo(addrinfo);
    
    return sockfd;
}

struct sshSession *createSshSession() {
    struct sshSession *sshSession = calloc(1, sizeof(struct sshSession));
    
    
    strncpy(sshSession->username, getenv("SSHY_USER"), sizeof(sshSession->username));
    strncpy(sshSession->password, getenv("SSHY_PASS"), sizeof(sshSession->password));
    
    sshSession->fd = connectToServer(getenv("SSHY_HOST"), 22);
    
    if (sshSession->fd < 0) {
        return NULL;
    } else {    
        return sshSession;
    }
}
