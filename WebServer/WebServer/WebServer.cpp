#include "WebServer.h"

/*
构造函数
    传入基本参数,端口号,线程数,最大请求数
    为储存客户端信息数组分配内存
    为定时器数组分配内存
*/
WebServer::WebServer(int port, int thread_nums, int max_queue_nums)
    : port_(port), thread_nums_(thread_nums), max_queue_nums_(max_queue_nums)
{
    //储存客户端连接情况
    users_ = new HttpConn[MAX_FD_NUMBER];

    //定时器相关结构
    users_timer_ = new ClientData[MAX_FD_NUMBER];
    
    //为定时器分配内存
    timer_manager_ = new TimerManager;
}

/*
析构函数
    关闭epollfd listenfd 管道
    释放分配的Http users资源
    释放分配的ClientData users_timer资源
    释放线程池资源
*/
WebServer::~WebServer() 
{
    close(epollfd_);
    close(listenfd_);
    close(pipefd_[0]);
    close(pipefd_[1]);
    delete[] users_;
    delete[] users_timer_;
    delete timer_manager_;
    delete thread_pool_;
}

/*
CreateThreadPool() 
    创建线程池
*/
void WebServer::CreateThreadPool() 
{
    thread_pool_ = new ThreadPool<HttpConn>(thread_nums_, max_queue_nums_);
}

/*
ListenEvents()
    向内核注册信号和对应的信号处理函数
    创建监听套接字
    设置端口复用
    创建epoll对象,并监视listenfd的EPOLLIN事件(设置非阻塞)
    创建定时器相关数组
*/
void WebServer::ListenEvents() 
{
    //注册信号
    utils_.AddSig(SIGPIPE, SIG_IGN);
    utils_.AddSig(SIGALRM, utils_.SigHandler);
    utils_.AddSig(SIGTERM, utils_.SigHandler);

    listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd_ != -1);

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);

    int reuse_flag = 1;
    //允许重用本地地址和端口
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &reuse_flag, sizeof(reuse_flag));
    
    // 给套接字绑定地址
    int ret = bind(listenfd_, (struct sockaddr *)&address, sizeof(address));
    assert(ret != -1);
    
    // 监听套接字
    ret = listen(listenfd_, 5);
    assert(ret != -1);

    // 这里是为了监听套接字以此来监听客户连接情况
    //epoll创建内核事件表
    epollfd_ = epoll_create(5);
    assert(epollfd_ != -1);

    /*
    监视listenfd_上的事件,因为只有主线程负责监听事件,所以不担心别的线程竞争,不需要使用EPOLLONESHOT
    listenfd_设置成非阻塞更好,虽然因为IO复用的原因不需要非阻塞,但是如果并发量上来了,处理事件可能效率不够,详细可以查看事件循环的
    */
    utils_.AddFd(epollfd_, listenfd_, false);

    //创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, WebServer::pipefd_);
    utils_.AddFd(epollfd_, pipefd_[0], false);
    
    //犯了一个错误,我直接将数组进行了赋值,这赋值的是地址而不是数组
    //所以把Utils::pipefd设置为指针比较好，毕竟数组名就是地址
    Utils::pipefd_ = pipefd_;
    Utils::epollfd_ = epollfd_;    
    //HttpConn代表客户端信息,并且会有一个初始化函数,客户端也需要监视是否有数据,所以也要上树
    HttpConn::epollfd_ = epollfd_;  

    //每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);
}

//处理事件循环中的新连接事件
bool WebServer::DealClientData()
{
    struct sockaddr_in client_addrss;
    socklen_t client_addrss_length = sizeof(client_addrss);
    
    //非阻塞listenfd,连续处理完所有客户端连接事情
    while (1) {
        int connfd = accept(listenfd_, (struct sockaddr*)&client_addrss, &client_addrss_length);
        //退处循环,一种是一开始就接受不到连接,另一种是处理完了所有连接
        if (connfd < 0) {
            printf("accept failure and the errno is %d\n", errno);
            return false;
        }
        else if (HttpConn::user_count_ >= MAX_FD_NUMBER) {
            perror("Internal server busy\n");
            return false;
        }
        else {
            //初始化客户端信息
            users_[connfd].Init(connfd, client_addrss);
            SetTimer(connfd, client_addrss);
        }
    }
}

/*
SetTimer()
    定时器内有ClientData结构,需要传入connfd client_address来初始化

*/
void WebServer::SetTimer(int connfd, struct sockaddr_in client_address)
{
    users_timer_[connfd].address = client_address;
    users_timer_[connfd].sockfd = connfd;
    
    TimerNode* timer = new TimerNode;
    timer->user_data_ = &users_timer_[connfd];
    timer->cb_func = CbFunc;                    //设置定时器回调函数
    time_t cur = time(NULL);                    //设置定时事件
    timer->expire = cur + 3 * TIMESLOT; 
    users_timer_[connfd].timer = timer;         //该连接更新其定时器成员
    timer_manager_->AddTimerNode(timer);        //将该定时器插入定时器管理结构
}

bool WebServer::DealWithSignal()
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(pipefd_[0], signals, sizeof(signals), 0);
    if (ret == -1) {
        return false;
    }
    else if (ret == 0) {
        return false;
    }
    else {
        for (int i = 0; i < ret; i++) {
            switch (signals[i])
            {
                case SIGALRM:
                {
                    timeout_ = true;
                    break;
                }
                case SIGTERM:
                {
                    stop_server_ = true;
                    break;
                }
            }
        }
    }

    return true;
}
/*
AddTimer()
    当有读写事件产生时,该客户端活跃
    增加定时时间
*/
void WebServer::AddTimer(TimerNode* timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT; //增加定时器事件
    timer_manager_->AdjustTimer(timer); //调整定时器链表
}

void WebServer::DealWithRead(int sockfd)
{
    //Reactor:主线程只负责监视,工作线程读写并处理数据
    TimerNode* timer = users_timer_[sockfd].timer;
    if (timer != NULL) {
        AddTimer(timer);
    }
    //若监测到读事件，将该事件放入请求队列
    thread_pool_->Append(&users_[sockfd], 0);
    while (true)
    {
        //完成事件判断
        if (1 == users_[sockfd].event_finish_) {
            //长连接和短连接判断
            if (1 == users_[sockfd].timer_flag_ ) {
                printf("读取数据失败,处理定时器和fd\n");
                DeleteTimer(users_timer_[sockfd].timer, sockfd);
                timer_manager_->DelTimer(users_timer_[sockfd].timer);
                users_[sockfd].timer_flag_ = 0;
            }
            users_[sockfd].event_finish_ = 0; //事件重新标记成未完成
            break;
        }
    }

}

void WebServer::DealWithWrite(int sockfd)
{
    TimerNode* timer = users_timer_[sockfd].timer;
    if (timer != NULL) {
        AddTimer(timer);
    }
    thread_pool_->Append(&users_[sockfd], 1);

    //需要先直到是否完成事件,没有完成事件就循环等待
    //完成事件后再判断是长连接还是短连接,短连接移除定时器关闭连接删除epoll注册对象
    
    while (true)
    {
        //完成事件判断
        if (1 == users_[sockfd].event_finish_) {
            //长连接和短连接判断
            if (1 == users_[sockfd].timer_flag_) {
                printf("短链接,关闭定时器\n");
                DeleteTimer(users_timer_[sockfd].timer, sockfd);
                users_[sockfd].timer_flag_ = 0;
            }
            users_[sockfd].event_finish_ = 0; //事件重新标记成未完成
            break;
        }
    }
}

/*
DeleteTimer
    调用回调函数,从epoll对象删除注册事件
    在定时器管理容器中删除该定时器
*/
void WebServer::DeleteTimer(TimerNode* timer, int sockfd)
{
    printf("删除定时器, 关闭文件描述符%d\n", sockfd);
    //调用回调函数,从epoll对象删除注册事件
    timer->cb_func(&users_timer_[sockfd]);
    //在定时器管理容器中删除该定时器
    if (timer) {
        timer_manager_->DelTimer(timer);
    }
}

void WebServer::TimerHandle()
{
    printf("timer tick!\n");
    timer_manager_->Tick();
    alarm(TIMESLOT);
}

/*
LoopEvents()
    事件循环
*/
void WebServer::LoopEvents()
{
    timeout_ = false;
    stop_server_ = false;

    while (!stop_server_) {
        int number = epoll_wait(epollfd_, events_, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) { //在非中断的方式下返回值小于0
            printf("epoll failure\n");
        } 
        for (int i = 0; i < number; i++) 
        {
            int sockfd = events_[i].data.fd;
            //如果监听到新的客户连接
            if (sockfd == listenfd_) {
                bool flag = DealClientData();
                if (false == flag) //false说明处理完了连接
                    continue;
            }
            else if (events_[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) //客户端
            {
                TimerNode* timer = users_timer_[sockfd].timer;
                DeleteTimer(timer, sockfd);
                printf("监听到异常事件,客户端关闭了连接, errno = %d\n", errno);
            }
            //如果是信号事件
            else if ((sockfd == pipefd_[0]) && events_[i].events & EPOLLIN) {
                bool flag = DealWithSignal();
                if (false == flag){
                    //错误信息
                    printf("DealWithSignal()信号错误\n");
                }               
            }
            //如果是客户端的读事件
            else if (events_[i].events & EPOLLIN) {
                DealWithRead(sockfd);
            }
            //如果是写事件
            else if (events_[i].events & EPOLLOUT) {
                DealWithWrite(sockfd);
            }
        } 
        //如果定时事件已到
        if (timeout_) 
        {
            TimerHandle();
            timeout_ = false;
        }
    }
}

