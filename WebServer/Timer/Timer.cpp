#include "Timer.h"

TimerManager::TimerManager() 
{
    head = NULL;
    tail = NULL;
}

TimerManager::~TimerManager()
{
    TimerNode* cur = head;
    while (cur) {
        cur = cur->next;
        delete head;
        head = cur;
    }
}

void TimerManager::AddTimerNode(TimerNode* timer)
{
    if (timer == NULL) {
        return;
    }
    if (head == NULL) {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    AddTimerNode(timer, head);
}

void TimerManager::AdjustTimer(TimerNode* timer)
{
    if (timer == NULL) {
        return;
    }
    //如果还是小于下一个定时器,则不用移动
    TimerNode* tmp = timer->next;
    if (tmp == NULL || (timer->expire < tmp->expire)) {
        return;
    }
    //如果是头节点,则隔离出该节点,再重新插入
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        AddTimerNode(timer, head);
    }
    else {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        AddTimerNode(timer, head);
    }
}

void TimerManager::DelTimer(TimerNode* timer)
{
    if (timer == NULL) {
        return;
    }
    //如果只有一个定时器
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //如果是第一个定时器
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
    }
    //如果是最后一个定时器
    if (timer == tail) {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    if (!timer || timer->prev || timer->next) {
        return;
    }
    //如果是中间节点
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;    
}

//检查是否有定时器超时,有则移除
void TimerManager::Tick()
{
    if (head == NULL) {
        return;
    }
    
    time_t cur = time(NULL);
    TimerNode* tmp = head;
    while (tmp) {
        //当前时间小于某定时器则退出
        if (cur < tmp->expire) {
            break;
        }
        tmp->cb_func(tmp->user_data_);//调用该定时器回调函数,关闭该客户连接
        head = tmp->next;
        if (head != NULL) {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void TimerManager::AddTimerNode(TimerNode* timer, TimerNode* head)
{
    TimerNode* prev = head;
    TimerNode* cur = prev->next;

    while (cur) {
        if (timer->expire < cur->expire) {
            prev->next = timer;
            timer->next = cur;
            timer->prev = prev;
            cur->prev = timer;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    
    //作为最后一个节点
    if (cur == NULL) {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void CbFunc(ClientData *user_data)
{
    epoll_ctl(Utils::epollfd_, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    HttpConn::user_count_--;
}
