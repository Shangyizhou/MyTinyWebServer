## 监听事件和准备条件（EventListen）

监听事件，我们创建listenfd来监听是否有客户端发起连接，于是我们要调用网络编程的日常API。socket，bind，listen，以让服务端达到可接收连接状态，而且这里我们还需要设置其他的东西

**端口复用**

端口复用允许在一个应用程序可以把 n 个套接字绑在一个端口上而不出错。同时，这 n 个套接字发送信息都正常，没有问题。但是，这些套接字并不是所有都能读取信息，只有最后一个套接字会正常接收数据。

`SO_REUSEADDR`可以用在以下四种情况下。

1. 当有一个有相同本地地址和端口的socket1处于TIME_WAIT状态时，而你启动的程序的socket2要占用该地址和端口，你的程序就要用到该选项（TIME_WAIT，四次挥手快结束了）。
2. SO_REUSEADDR允许同一port上启动同一服务器的多个实例(多个进程)。但每个实例绑定的IP地址是不能相同的。在有多块网卡或用IP Alias技术的机器可以测试这种情况。
3. SO_REUSEADDR允许单个进程绑定相同的端口到多个socket上，但每个socket绑定的ip地址不同。这和2很相似，区别请看UNPv1。
4. SO_REUSEADDR允许完全相同的地址和端口的重复绑定。但这只用于UDP的多播，不用于TCP。

> https://zhuanlan.zhihu.com/p/145635380

```c++
//设置端口复用
int ret = setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &m_reuse_, sizeof(m_reuse_));
assert(ret != -1);
```

**创建epoll监视fd**

```C++
m_epollfd_ = epoll_create(5);
assert(m_epollfd_ != -1);

addfd(epollfd, listenfd_, true, listenfd_trig_mode_);
http_conn::epollfd_ = epollfd_;
```

**addfd**

```C++
/*
addfd(int epollfd, int fd, bool one_shot)
    向epoll对象注册fd和监视事件类型
    根据选项设置是否为 EPOLLONESHOT
    根据选项设置是否为 EPOLLET EPOLLLT(trig:触发模式, 默认是LT模式)
    EPOLLHUP：表示对应的文件描述符被挂断；
    EPOLLONESHOT：
        对于注册了EPOLLONESHOT事件的文件描述符，操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次。
        也就是说，新数据来是不会再触发事件的，既然不会触发，那么也就不会有新的线程被选择来处理这个连接        
        
        注册了EPOLLONESHOT事件的 socket一旦被某个线程处理完毕，该线程就应该立即重置这个socket上的EPOLLONESHOT事件，
        以确保这个socket下一次可读时，其EPOLLIN事件能被触发，进而让其他工作线程有机会继续处理这个socket。
*/
void Webserver::addfd(int epollfd, int fd, bool one_shot, int Trig_mode_)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == Trig_mode_)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;    //epoll默认LT模式

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}
```

**EPOLLSHOT**

[epoll中et+多线程模式中很重要的EPOLL_ONESHOT实验 - blcblc - 博客园 (cnblogs.com)](https://www.cnblogs.com/charlesblc/p/5538363.html)

[epoll中EPOLLSHOT的使用 - 李湘沅 - 博客园 (cnblogs.com)](https://www.cnblogs.com/lxy-xf/p/11307100.html)

**因为et模式需要循环读取，但是在读取过程中，如果有新的事件到达，很可能触发了其他线程来处理这个socket，那就乱了。**（工作线程循环读，ET模式下一次触发时候选择了另一个线程，第一个工作线程就傻眼了）

**注：EPOLL_ONESHOT的原理其实是，每次触发事件之后，就将事件注册从fd上清除了，也就不会再被追踪到；下次需要用epoll_ctl的EPOLL_CTL_MOD来手动加上才行。**

注册了`EPOLLONESHOT`事件的 socket一旦被某个线程处理完毕（譬如循环读完数据），该线程就应该立即重置这个socket上的`EPOLLONESHOT`事件，以确保这个socket下一次可读时，其EPOLLIN事件能被触发，进而让其他工作线程有机会继续处理这个socket。

**Trig_mode_**

我们会根据不同的模式设置不同的事件。ET LT

**创建管道(统一信号源)**

```C++
//创建管道套接字
ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd_);

//设置管道读端为ET非阻塞 统一事件源
addfd(epollfd_, pipefd_[0], false, 0);
```

**管道的读端和写端都设置为非阻塞**

写端：send将信息发给fd[1]，如果管道满了，那么会阻塞住，加长信号处理的事件

读端：没有信号值，一直阻塞到有信号，那么主线程的事件循环就阻塞在读取管道这行代码了

**管道的特性**

如果先读，陷入阻塞，等待写操作；如果先写，陷入阻塞，等待读操作。

而非阻塞读和非阻塞写，是无须等待另一个操作的，直接执行read()或者write()能读就读，能写就写，不能就返回-1,非阻塞读写主要是用于自己循环读取，去判断读写的长度

## 阻塞与非阻塞

[高性能网络通信库中为何要将侦听 socket 设置成非阻塞的？ (qq.com)](https://mp.weixin.qq.com/s/nacUx_qQr93_y-CsVxkejA)

有个问题，什么fd需要设置为阻塞的，什么fd需要设置为非阻塞的？这和IO模型又有关系吗？

**listenfd**

假设，我们不使用IO复用，而只是单个线程调用accept，那么如果listenfd是阻塞的，然后又没有客户端连接，此时就会在accept这里阻塞住，这是不好的。有的框架可能listenfd是阻塞的，但是它们还有其他处理这类情况的代码来避免滞留在accept这

如果使用IO复用，那么我们使用epoll来监听事件，我们注册了listenfd和读事件，只有返回了以后我们才可以根据connfd值取调用accept，所以accept不存在阻塞的说法。只要到达了调用了accept的代码，就证明epoll_wait返回，也就说明了读事件的发生即有连接。但是epoll_wait本身是阻塞的，只不过我们可以避免调用accept的阻塞时间。

**问题**是这样的设计存在严重的效率问题：这种设计在每一轮循环中只能一次接受一个连接（每次循环仅调用了一次 accept），如果连接数较多，这种处理速度可能跟不上，所以要在一个循环里面处理 accept，但是实际情形是我们没法确定下一轮调用 accept 时 backlog 队列中是否还有新连接呀，如果没有，由于 listenfd 是阻塞模式的， accept 会阻塞（意思是假设加入了一个循环，结果下一次没有连接了，那么就死循环了），**这与ET触发加循环和非阻塞读取数据的情况是差不多的**。

设置listenfd为非阻塞可以解决这个问题，我们此时还加入循环，在一个while里解决掉多个连接，此时不会出现死循环问题，因为我们使用的是非阻塞listenfd，如果不行会立即返回，我们根据返回的情况的EPOLLAGAIN EWOULDBLOCK

```C++
 1void* io_thread_func(void* param)
 2{
 3    //可以在这里做一些初始化工作
 4
 5    while (退出标志)
 6    {
 7        epoll_event epoll_events[1024];
 8        //listenfd和clientfd都挂载到epollfd由epoll_wait统一检测读写事件
 9        n = epoll_wait(epollfd, epoll_events, 1024, 1000);
10
11        if (listenfd上有事件)
12        {
13            while (true) 
14            {
15                //此时调用accept函数不会阻塞
16                int clientfd = accept(listenfd, ...);
17                if (clientfd == -1)
18                {
19                    //错误码是EWOULDBLOCK说明此时已经没有新连接了
20                    //可以退出内层的while循环了
21                    if (errno == EWOULDBLOCK)
22                        break;
23                    //被信号中断重新调用一次accept即可   
24                    else if (errno == EINTR)
25                        continue;
26                    else 
27                    {
28                        //其他情况认为出错
29                        //做一次错误处理逻辑
30                    }   
31                } else {
32                    //正常接受连接
33                    //对clientfd作进一步处理
34                }//end inner-if             
35            }//end inner-while-loop
36
37        }//end outer-if
38
39        //其他一些操作
40    }//end outer-while-loop 
41}
```

将 listenfd 设置成非阻塞模式还有一个好处时，我们可以自己定义一次 listenfd 读事件时最大接受多少连接数，这个逻辑也很容易实现，只需要将上述代码的内层 while 循环的判断条件从 true 改成特定的次数就可以：

```C++
1void* io_thread_func(void* param)
 2{
 3    //可以在这里做一些初始化工作
 4
 5    //每次处理的最大连接数目
 6    const int MAX_ACCEPTS_PER_CALL = 200;
 7    //当前数量
 8    int currentAccept;
 9
10    while (退出标志)
11    {
12        epoll_event epoll_events[1024];
13        //listenfd和clientfd都挂载到epollfd由epoll_wait统一检测读写事件
14        n = epoll_wait(epollfd, epoll_events, 1024, 1000);
15
16        if (listenfd上有事件)
17        {
18            currentAccept = 0;
19            while (currentAccept <= MAX_ACCEPTS_PER_CALL) 
20            {
21                //此时调用accept函数不会阻塞
22                int clientfd = accept(listenfd, ...);
23                if (clientfd == -1)
24                {
25                    //错误码是EWOULDBLOCK说明此时已经没有新连接了
26                    //可以退出内层的while循环了
27                    if (errno == EWOULDBLOCK)
28                        break;
29                    //被信号中断重新调用一次accept即可   
30                    else if (errno == EINTR)
31                        continue;
32                    else 
33                    {
34                        //其他情况认为出错
35                        //做一次错误处理逻辑
36                    }   
37                } else {
38                    //累加处理数量
39                    ++currentAccept;
40                    //正常接受连接
41                    //对clientfd作进一步处理
42                }//end inner-if  
43            }//end inner-while-loop
44
45        }//end outer-if
46
47        //其他一些操作
48    }//end outer-while-loop 
49}
```

**clientfd**

现在就剩下 clientfd 了，如果不将 clientfd 设置成非阻塞模式，那么一旦 epoll_wait 检测到读或者写事件返回后，接下来处理 clientfd 的读或写事件，如果对端因为 TCP 窗口太小，send 函数刚好不能将数据全部发送出去，将会造成阻塞，进而导致整个服务“卡住”。

## 定时器

先设置一个`alarm`，到了时间后发送信号，然后信号处理函数将信号值非阻塞写到管道（统一信号源），管道读端被epoll监视读事件，有专门处理信号的`switch`，发现是`alarm`事件则设置`timeout`，定时器事件没有`IO`事件优先，所以最后处理，所以只是将`timeout`的标记设置一下。主线程事件循环到最后，有一个判断发现`timeout`为`true`，此时会调用定时器回调函数，回调函数内部会调用维护定时器的双向链表的`tick`函数，用于检测定时器事件是否到达，如果到达则删除定时器。然后回调函数继续调用`alarm`

**定时器和用户数据结构的设计属于你中有我，我中有你。**

- 增添定时时间时
  - 我们发现`connfd`发来了数据，我们需要通过`connfd`找到该客户端的定时器，增加定时器时间，调整位置
- 删除定时器并调用回调函数时
  - 我们的定时检查时间到了，定时器调用`tick`，发现超时的定时器，会调用回调函数删除该定时器对应的`fd`

**所以定时器和用户结构你中有我，我中有你，其中有部分属性没有用上，可以理解为方便扩展**

对于定时器来说，`sort_lst`的`tick`函数如果发现了超时事件，是要删除定时器并且要调用回调函数，回调函数关闭连接，所以要在`epoll`删除`connfd`。所以定时器内部需要保存`connfd`的信息，直接保存`client_data`。定时器的操作是删除定时事件然后关闭用户连接，所以需要有用户的数据结构，可以根据对应的`connfd`找到对应的定时器删除它并且也可以根据`connfd`找到对应的用户数据结构，`close fd`，并且删除在`epoll`上的注册

所以需要有`epollfd`，同时一开始还需要创建一个用于统计计时器事件的数组

## 事件循环（EventLoop）

在一个循环内调用`epoll_wait`接收连接，然后分析得到的发生事件的fd

- 如果为新的客户端申请连接，调用`DealClientData()`
  - 接收连接
  - 初始化客户端信息
  - 设置该连接的定时器
- 如果为管道有信号到达，调用`DealWithSignal()`
  - 主要针对`SIGALARM`和`SIGTERM`信号，如果是`SIGALARM`，则设置`timeout_ = true`
- 如果为客户端的读事件，调用`DealWithRead()`
  - 增加该定时器时长
  - 将该时间放入线程池请求队列，线程竞争执行任务
  - 经过一个循环，待读事件完成操作完成后，进入循环内部判断是短连接还是长连接，短连接直接关闭。长连接将事件重新标记成未完成
- 如果为自己向客户端的写事件，调用`DealWithWrite()`
  - 与读事件类似
- 最后是定时器事件，通过`timer_flag`判断
  - 调用定时处理函数`TimerHandle()`，再在里面调用`tick()`函数检查到期的定时器，关闭连接
  - 继续调用alarm()`
- 如果是`if (events_[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))`
  - 则关闭对应的文件描述符
  - 关闭对应的定时器



## HTTP

他们也需要`epollfd`和`fd`，因为需要在`epoll`上注册写事件

## 工具类Utils

我们有一些常用的函数或类，比如`addsig`，`setnonblocking`，我们将这些函数放到工具类内部实现，供随时使用

```c++
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
```

