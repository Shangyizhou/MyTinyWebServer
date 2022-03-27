## MyTinyWebServer

Linux下C++轻量级Web服务器

## 简介

该项目是为学习Linux网络编程知识，在Linux环境下使用C++语言开发的轻量级多线程服务器，该服务器支持一定数量的客户端连接并及时响应，支持客户端访问服务器图片

本项目使用同步`I/O`模型实现`Reactor`模式，主线程使用`epoll`的`IO`复用分派事件。如果监听到一个新连接，则将其封装成一个任务插入到任务队列中。线程池中沉睡的工作线程会被唤醒，争抢锁并执行任务。工作线程处理任务逻辑，完成后注册写事件，这会触发一个`EPOLLOUT`，主线程再安排工作现场写响应报文，事后根据长连接还是短连接进行关闭连接或保持连接的操作。

![image-20220328014738048](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220328014738048.png)

## 核心功能及技术

- 状态机解析HTTP请求，目前支持 HTTP GET方法
- 创建线程池提高并发度，降低动态创建线程带来的开销，使用互斥锁，信号量保证线程的互斥和同步
- 定时器处理支持HTTP长连接，并处理非活动连接节省资源
- 使用epoll + 非阻塞IO + ET实现高并发处理，使用Reactor编程模型
- 统一信号源，将信号统一在主线程处理，减少SigHandler处理时间
- epoll使用EPOLLONESHOT保证一个socket连接在任意时刻都只被一个线程处理

## 并发模式

使用半同步/半反应堆模式

![image-20220328014823954](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220328014823954.png)

- 异步线程只有一个，由主线程来充当。它负责监听所有 socket上的事件。
- 如果监听socket上有可读事件发生，即有新的连接请求到来，主线程就接受之以得到新的连接socket，然后往epoll内核事件表中注册该socket上的读写事件。
- 如果连接socket上有读写事件发生， 即有新的客户请求到来或有数据要发送至客户端，主线程就将该连接 socket插入请求队列中。
- 所有工作线程都睡眠在请求队列上，当有任务到来时，它们将通过竞争（比如申请互斥锁）获得任务的接管权。这种竞争机制使得只有空闲的工作线程才有机会来处理新任务，这是很合理的。

## Makefile

```c++
CC = g++
CFLAGS = -Wall -g

server: main.o WebServer.o Utils.o HttpConn.o Timer.o
	$(CC) $(CFLAGS) *.o -lpthread -o server

main.o: main.cpp	
	$(CC) $(CFLAGS) -c main.cpp

WebServer.o: ./WebServer/WebServer.cpp
	$(CC) $(CFLAGS) -c ./WebServer/WebServer.cpp

Utils.o: ./Utils/Utils.cpp
	$(CC) $(CFLAGS) -c ./Utils/Utils.cpp

HttpConn.o: ./Http/HttpConn.cpp
	$(CC) $(CFLAGS) -c ./Http/HttpConn.cpp

Timer.o: ./Timer/Timer.cpp
	$(CC) $(CFLAGS) -c ./Timer/Timer.cpp

clean:
	rm -f *.o  

```

## 运行

```c++
make
```

```c++
./server 3000
```

## 运行实例

> 打开浏览器，输入120.77.3.164:3000

![image-20220328013757805](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220328013757805.png)

> 打开浏览器，输入120.77.3.164:3000/index.html

![image-20220328013805852](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220328013805852.png)

> 服务端创建线程池并启动定时器

![image-20220328013810076](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220328013810076.png)

> HTTP报文解析和响应报文及一些提示

![image-20220328013814339](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220328013814339.png)

## 项目讲解

- [线程池](./模块记录/线程池.md)
- [主线程事件循环](./模块记录/EventListen.md)
- [HTTP与状态机](./模块记录/HTTP连接.md)
- [定时器](./模块记录/定时器.md)

## 项目改善

- 使用C++ 11智能指针进一步封装，自动管理内存
- 增加对POST请求的支持
- 对定时器的数据结构进行改善，双向链表插入比较缓慢
- 增加日志功能，记录服务器运行情况

## 参考

- 《TCP/IP网络编程》——尹圣雨
- 《Linux高性能服务器编程》——游双
- [qinguoyi/TinyWebServer: Linux下C++轻量级Web服务器 (github.com)](https://github.com/qinguoyi/TinyWebServer)

