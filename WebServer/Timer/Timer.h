#ifndef TIMER_H
#define TIMER_H

#include <time.h>
#include <sys/types.h>       
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "../Utils/Utils.h"
#include "../Http/HttpConn.h"
class TimerNode;

struct ClientData
{
    sockaddr_in address;
    int sockfd;
    TimerNode *timer;
};

//定时器结构
class TimerNode
{
public:
    TimerNode() : prev(NULL), next(NULL) {}

public:
    time_t expire;//定时器时间(绝对时间)

    void (*cb_func)(struct ClientData*);   //回调函数
    ClientData *user_data_;         //定时器维护的用户
    TimerNode* prev;                //下一节点
    TimerNode* next;                //上一节点
};

/*
使用的是双向链表来管理定时器
*/
class TimerManager
{
public:
    TimerManager(); 
    ~TimerManager();

    void AddTimerNode(TimerNode* timer);    //插入定时器
    void AdjustTimer(TimerNode* timer);     //调整定时器
    void DelTimer(TimerNode* timer);        //删除定时器
    void Tick();                            //定时器检查函数

private:
    void AddTimerNode(TimerNode* timer, TimerNode* head);

private:
    TimerNode* head;//头指针
    TimerNode* tail;//尾指针
};

void CbFunc(struct ClientData *user_data);

#endif
