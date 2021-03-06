#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <cstdio>
#include <cstdio>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>         
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <strings.h>
#include <assert.h>
#include <errno.h>

#include "../ThreadPool/ThreadPool.h"
#include "../Timer/Timer.h"
#include "../Utils/Utils.h"

const int MAX_EVENT_NUMBER = 10000; //epoll最多注册的事件数量
const int MAX_FD_NUMBER = 65536;    //最多的文件描述符数量
const int TIMESLOT = 5;             //每隔5s发送alarm信号

class WebServer 
{
public:
    WebServer(int port, int thread_nums = 10, int max_queue_nums = MAX_EVENT_NUMBER);
    ~WebServer();

public:
    void CreateThreadPool();        //创建线程池
    void ListenEvents();            //开启事件监听
    void LoopEvents();              //开启事件循环

public:
    //事件循环针对不同事件的处理函数
    bool DealClientData();          //处理客户端连接事件
    bool DealWithSignal();          //处理信号事件
    void DealWithRead(int sockfd);  //处理读事件
    void DealWithWrite(int sockfd); //处理写事件

    //定时器设置函数
    void SetTimer(int connfd, struct sockaddr_in client_address);
    void DeleteTimer(TimerNode* timer, int sockfd);
    void TimerHandle();
    void AddTimer(TimerNode* timer);

public:
    int listenfd_;      //监听文件描述符
    int port_;          //端口
    int epollfd_;       //epoll句柄
    int pipefd_[2];     //发送信号的管道
    HttpConn* users_;   //各个客户端连接
    bool stop_server_;  //停止服务器的标志
    bool timeout_;      //计时时间标志
    
    //epoll_event相关
    epoll_event events_[MAX_EVENT_NUMBER];//储存发生的事件

public:    
    ThreadPool<HttpConn> *thread_pool_;

    int thread_nums_;   //线程池的线程数量
    int max_queue_nums_;//请求队列最多请求数

public:
    ClientData *users_timer_;       //定时器相关数据结构
    TimerManager *timer_manager_;   //维护定时器的双向链表

public:
    Utils utils_;               //工具类成员,有addfd, addsig等常用函数
    
};

#endif