#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "http_conn.h"
#include "threadpool.h"
#include "timer.h"

static const int TIMESLOT = 10;                     //定时时间
static int pipefd[2];                               //创建管道发送接收信号
static const int MAX_EVENT_NUMBER = 10000;          //epoll最多监视的事件数目
const int MAX_FD_NUMBER = 65535;                    //最大文件描述符数量

static bool stop_server = false;                    //服务器运行标记
static bool timeout = false;                        //定时结束标记
static client_data m_users_timer[MAX_FD_NUMBER];    //定时器相关的客户端数据
static sort_timer_lst timer_lst;                    //管理定时器的双向链表
int epollfd;

//增添监视事件和文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
//移除监视事件和文件描述符
extern void removefd(int epollfd, int fd);

//信号处理函数
void sig_handler( int sig )
{    
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

//注册信号处理函数
void addsig(int sig, void( handler )(int))
{
    struct sigaction sa;
    bzero(&sa, sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

//调用定时器事件的回调函数
void timer_handler()
{
    /*定时处理任务，实际上就是调用tick函数，在链表中增删定时时间*/
    timer_lst.tick();
    //继续调用闹钟
    alarm( TIMESLOT );
}

void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    printf( "close fd %d\n", user_data->sockfd );
}

bool listen_event(int epollfd, int fd)
{

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
    addsig( SIGPIPE, SIG_IGN);
    addsig( SIGALRM, sig_handler);
    addsig( SIGTERM, sig_handler);

    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);

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
        
    //创建监听文件描述符
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd > 0);
   
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    //在socket地址绑定之前设置端口复用，让别的socket也可以绑定此端口
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int ret = bind(listenfd, (struct sockaddr*)&server_address, sizeof(server_address));
    assert(ret != -1);

    //errno is : 22（之前忘记listen返回的情况）
    ret = listen(listenfd, 10);
    assert(ret != -1);
    
    //创建epoll对象
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    //添加到epoll对象中
    addfd(epollfd, listenfd, false);
    //线程池也要用到epollfd
    http_conn::m_epollfd = epollfd;

    //监视管道读端读事件(ET + 非阻塞)
    addfd(epollfd, pipefd[0], false);
    //手动设置一个闹钟
    alarm(TIMESLOT);
    bool timeout = false;

    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_FD_NUMBER, -1);
        //errno 4:异步非阻塞IO,不确认是否收到,发送缓冲区可能会满,就释放这个信号,让你再试一次
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
           
            //如果是监听文件描述符
            if (sockfd == listenfd) {
                printf("测试语句:listenfd 监听到新连接\n\n");
                struct sockaddr_in client_address;
                socklen_t client_address_length = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_address_length);
                if (connfd < 0) {
                    printf("accept failure and the errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD_NUMBER) {
                    printf("测试语句:超过最大连接数\n");
                    close(connfd);
                    continue;
                }
                //新客户的数据初始化,记录客户端的connfd和地址
                users[connfd].init(connfd, client_address);
                
                //定时器相关
                m_users_timer[connfd].address = client_address;
                m_users_timer[connfd].sockfd = connfd;
                
                //创建定时器
                util_timer* timer = new util_timer;
                timer->user_data = &m_users_timer[connfd];  //该定时器绑定该客户端
                timer->cb_func = cb_func;         //定时器回调函数，删除定时器
                time_t cur = time( NULL );          //获取当前时间
                timer->expire = cur + 3 * TIMESLOT; //设立定时时间
                m_users_timer[connfd].timer = timer;        //该客户端数据结构也包含此定时器
                timer_lst.add_timer( timer );       //将给定时器交给定时器双向链表管理

            
            } else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    // handle the error
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            /*用timeout变量标记有定时任务需要处理，但不立即处理定时任务。这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务*/
                            case SIGALRM:
                            {          
                                                   
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //异常情况下关闭连接
                users[sockfd].close_conn();
                printf("测试语句:监听到异常事件,客户端关闭了连接, errno = %d\n\n", errno);

            } else if(events[i].events & EPOLLIN) {
                printf("测试语句:监听到读取事件,有读取事件发生\n");
                util_timer* timer = m_users_timer[sockfd].timer;
                //嵌套循环读完数据,我们是ET + 非阻塞
                if (users[sockfd].read()) {
                    //正常读取了数据
                    //线程池插入请求队列
                    pool->append(&users[sockfd]);
                    /*如果某个客户连接上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间*/
                    if (timer)
                    {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                } else {
                    //没读到数据或异常数据
                    printf("测试语句:读取不到数据（并非是数据读完情况）\n");
                    //读取数据完成,调用定时器回调函数
                    cb_func( &m_users_timer[sockfd] );
                    if( timer )
                    {
                        //管理定时器的链表也移除该定时器
                        timer_lst.del_timer( timer );
                    }
                    users[sockfd].close_conn();
                }
            
            } else if(events[i].events & EPOLLOUT) {
                //监视到写事件
                if (!users[sockfd].write()) {
                        users[sockfd].close_conn();
                }
            } if (timeout) {
                timer_handler();
                timeout = false;
            }
            
            
        }
    }

    
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;


    return 0;
}