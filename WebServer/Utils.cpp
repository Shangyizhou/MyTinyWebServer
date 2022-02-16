#include "Utils.h"

int* Utils::pipefd_ = nullptr;

/*
SetNonBlocking()
    设置非阻塞文件描述符
    获取fd属性old_option，使其增加非阻塞属性
    调用fcntl重新设置fd属性
    返回旧fd属性
*/
int Utils::SetNonBlocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/*
AddFd(int epollfd, int fd, bool one_shot)
    将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT确保一个线程服务一个请求
*/
void Utils::AddFd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    SetNonBlocking(fd);
}

/*
RemoveFd()
    从epoll中移除监听的文件描述符
*/ 
void Utils::RemoveFd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

/*
modfd()
    修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
*/ 
void Utils::ModFd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}


/*SigHandler(int sig)
    设置信号处理函数,统一信号源
    只向本地管道发送信号值,信号处理在事件循环中
*/
void Utils::SigHandler(int sig) {
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(Utils::pipefd_[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

/*
AddSig(int sig, void(handler)(int))
    向内核注册信号及其信号处理函数
*/
void Utils::AddSig(int sig, void(handler)(int)) {
    struct sigaction sa;
    bzero(&sa, sizeof(sig));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

