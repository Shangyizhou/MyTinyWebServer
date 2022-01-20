#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include "http_conn.h"
#include "threadpool.h"


static const int MAX_EVENT_NUMBER = 10000;  //epoll最多监视的事件数目
const int MAX_FD_NUMBER = 65535;            //最大文件描述符数量

//增添监视事件和文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
//移除监视事件和文件描述符
extern void removefd(int epollfd, int fd);

//注册信号处理函数
void addsig(int sig, void(handler)(int))
{
    struct sigaction sa;
    bzero(&sa, sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

int main(int argc, char* argv[])
{
    if (argc <= 1) {
        printf("Usage: ip and port\n");
        exit(1);
    }
    
    //端口从 字符串 -> int
    int port = atoi(argv[1]);
    
    /*
    对SIGPIPE信号进行处理
    默认情况下，往一个读端关闭的管道或socket连接中写数据将引发SIGPIPE信号。
    我们需要在代码中捕获并处理该信号，或者至少忽略它，因为程序接收到SIGPIPE信号的默认行为是结束进程，
    而我们绝对不希望因为错误的写操作而导致程序退出。引起SIGPIPE信号的写操作将设置errno为EPIPE。
    SIG_IGN忽略信号
    */
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池，初始化线程池
    //线程池向请求队列插入任务
    threadpool<http_conn> *pool = nullptr;
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        // LOG_ERROR("%s", "create threadpool failure");
        return 1;
    }

    //创建数组用于保存所有的客户端信息
    http_conn *users = new http_conn[MAX_FD_NUMBER];
    
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd > 0);

    //在socket地址绑定之前设置端口复用，让别的socket也可以绑定此端口
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);


    int ret = bind(listenfd, (struct sockaddr*)&server_address, sizeof(server_address));
    assert(ret != -1);
    
    //创建epoll对象
    epoll_event events[MAX_EVENT_NUMBER];
    //向内核的建议值
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    //添加到epoll对象中?
    addfd(epollfd, listenfd, false);
    //线程池也要用到epollfd
    http_conn::m_epollfd = epollfd;

    //errno is : 22（之前忘记listen返回的情况）
    ret = listen(listenfd, 10);
    assert(ret != -1);

    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_FD_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            //如果是监听文件描述符
            if (sockfd == listenfd) {
                printf("测试语句:sockfd == listenfd 监听到新连接\n");
                struct sockaddr_in client_address;
                socklen_t client_address_length = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_address_length);
                if (connfd < 0) {
                    printf("accept failure and the errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count > MAX_FD_NUMBER) {
                    printf("测试语句:超过最大连接数\n");
                    close(connfd);
                    continue;
                }

                //新客户的数据初始化
                users[connfd].init(connfd, client_address);
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //异常情况下关闭连接
                users[sockfd].close_conn();
                printf("测试语句:监听到异常事件,客户端关闭了连接, errno = %d\n", errno);
            
            } else if(events[i].events & EPOLLIN) {
                printf("测试语句:监听到读取事件,有读取事件发生\n");
                //嵌套循环读完数据,我们是ET + 非阻塞
                if (users[sockfd].read()) {
                    //线程池插入请求队列
                    printf("测试语句:读完数据,插入请求队列\n");
                    pool->append(&users[sockfd]);
                
                } else {
                    printf("测试语句:读取不到数据了,客户端关闭了连接\n");
                    users[sockfd].close_conn();
                }
            
            } else if(events[i].events & EPOLLOUT) { //监视到写事件
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }

            }

        }
    }

    
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;


    return 0;
}