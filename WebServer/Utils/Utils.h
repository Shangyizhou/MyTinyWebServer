#ifndef UTILS_H
#define UTILS_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

class Utils
{
public:
    Utils() { }
    ~Utils() { }
    
    //设置非阻塞函数
    int SetNonBlocking(int fd);
    
    //信号处理函数,统一信号源
    static void SigHandler(int sig);

    //设置信号函数
    void AddSig(int sig, void(handler)(int));

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void AddFd(int epollfd, int fd, bool one_shoot);

    void RemoveFd( int epollfd, int fd);

    void ModFd(int epollfd, int fd, int ev);

public:
    static int *pipefd_;    //信号的管道
    static int epollfd_;    //epoll文件描述符
};

#endif