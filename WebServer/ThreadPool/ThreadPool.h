#ifndef ThreadPool_H
#define ThreadPool_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <semaphore.h>

#include "Locker.h"
#include "../Http/HttpConn.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class ThreadPool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    ThreadPool(int thread_number, int max_requests);
    ~ThreadPool();
    //插入任务函数
    bool Append(T* request, int event_flag);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    //线程被创建的时候被指明执行该函数,我们会将this指针传递进去,因为静态函数无法访问成员变量
    static void* ThreadWorkFunc(void* arg);
    void ThreadRun();

private:
    
    int thread_number_;            // 线程的数量    
    int max_requests_;             // 请求队列中最多允许的、等待处理的请求的数量 
    pthread_t * threads_;          // 描述线程池的数组，大小为thread_number_    
    std::list< T* > workqueue_;    // 请求队列
    Locker queuelocker_;           // 保护请求队列的互斥锁
    Sem queuestat_;                // 是否有任务需要处理
    bool stop_;                    // 是否结束线程   
    int event_flag_;               // 0:读事件  1:写事件               
};

/*
ThreadPool(int thread_number, int max_requests)
构造函数
    设定创建线程数量
    设定最大请求数量
    初始化线程池指向NULL
    初始化stop_为true
*/
template< typename T >
ThreadPool< T >::ThreadPool(int thread_number, int max_requests) : 
        thread_number_(thread_number), max_requests_(max_requests), 
        stop_(false), threads_(NULL) {

    //线程数或请求数小于等于0，抛出异常
    if((thread_number_ <= 0) || (max_requests_ <= 0) ) {
        throw std::exception();
    }

    //申请线程池数组
    threads_ = new pthread_t[thread_number_];
    if(!threads_) {
        throw std::exception();
    }

    //创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "create the %dth thread\n", i);
        //创建线程并传递this指针
        if(pthread_create(threads_ + i, NULL, ThreadWorkFunc, this ) != 0) {
            delete [] threads_;
            throw std::exception();
        }
        //设置线程为脱离态
        if( pthread_detach( threads_[i] ) ) {
            delete [] threads_;
            throw std::exception();
        }
    }
}

/*
~ThreadPool()
析构函数
    释放线程池资源
    重置stop_标志位
*/
template< typename T >
ThreadPool< T >::~ThreadPool() {
    delete [] threads_;
    stop_ = true;
}

/*
Append( T* request )
添加任务
    请求队列增加了元素，信号量应该执行V操作，增加sem值
*/
template< typename T >
bool ThreadPool< T >::Append( T* request, int event_flag)
{
    event_flag_ = event_flag;
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    queuelocker_.Lock();
    if ( workqueue_.size() > max_requests_ ) {
        queuelocker_.UnLock();
        return false;
    }
    workqueue_.push_back(request);
    queuelocker_.UnLock();
    //经过Append操作，请求队列有了元素，执行V操作，增加信号量
    //这会唤醒在queuestat_.wait()处被阻塞的工作线程
    queuestat_.Post();
    return true;
}

/*
ThreadWorkFunc( void* arg )
    创建线程时指定的线程运行函数
*/
template< typename T >
void* ThreadPool< T >::ThreadWorkFunc( void* arg )
{
    //因为静态函数无法访问成员函数原因，我们直接传递this指针
    //注意，这里我们要利用原来的线程池对象，那里有我们的线程池数组，锁，信号量，这些都需要唯一的
    //新的线程池指针，指向原有的线程池
    ThreadPool* pool = ( ThreadPool* )arg;
    //线程池运行函数
    pool->ThreadRun();
    return pool;
}

/*
ThreadRun()
线程池运行函数
    生产者消费者模型,当公共队列有了元素即sem > 0,会解锁唤醒线程,线程唤醒后继续上锁
    线程获取锁取出队列头处请求,并执行处理函数     
*/
template< typename T >
void ThreadPool< T >::ThreadRun() {

    while (!stop_) {
        /*
        sem_wait函数将以原子操作方式将信号量减一,信号量为0时,sem_wait阻塞
        sem_post函数以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
        信号量最开始被初始化为0，工作线程在此处阻塞
        经过Append操作，执行V操作，sem值大于0
        工作线程被唤醒竞争出一个线程，成功的工作现场进入，又执行P操作，导致sem值为0，其他线程被继续阻塞
        */
        queuestat_.Wait();
        //访问公共区域上锁
        queuelocker_.Lock();
        if ( workqueue_.empty() ) {
            queuelocker_.UnLock();
            continue;
        }
        //取出队列前面的任务
        T* request = workqueue_.front();
        workqueue_.pop_front();
        queuelocker_.UnLock();
        if ( !request ) {
            continue;
        }
        
        if (0 == event_flag_) {
            if (request->ReadOnce()) {
                //线程对HTTP请求进行处理
                request->Process();
                request->event_finish_ = 1;
            }
            else {
                //读取失败
                request->timer_flag_ = 1;
                request->event_finish_ = 1;
            }
        }
        else if (1 == event_flag_) {            
            if (request->Write()) { //长连接
                request->event_finish_ = 1;
            } else {//短连接
                request->timer_flag_ = 1;
                request->event_finish_ = 1;
            }     
                                            
        }
    }

}

#endif
