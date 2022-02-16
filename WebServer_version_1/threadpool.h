#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    //插入任务函数
    bool append(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    //线程被创建的时候被指明执行该函数,我们会将this指针传递进去
    static void* worker(void* arg);
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 
    
    // 请求队列
    std::list< T* > m_workqueue;  

    // 保护请求队列的互斥锁
    locker m_queuelocker;   

    // 是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程          
    bool m_stop;                    
};

/*
构造函数
    设定创建线程数量
    设定最大请求数量
    初始化线程池指向NULL
    初始化m_stop为true
*/
template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL) {

    //线程数或请求数小于等于0，抛出异常
    if((thread_number <= 0) || (max_requests <= 0) ) {
        throw std::exception();
    }

    //申请线程池数组
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) {
        throw std::exception();
    }

    //创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "create the %dth thread\n", i);
        //创建线程并传递this指针
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
        //设置线程为脱离态
        if( pthread_detach( m_threads[i] ) ) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

/*
析构函数
    释放线程池资源
    重置m_stop标志位
*/
template< typename T >
threadpool< T >::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

/*
添加任务
    请求队列增加了元素，信号量应该执行V操作，增加sem值
*/
template< typename T >
bool threadpool< T >::append( T* request )
{
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //经过append操作，请求队列有了元素，执行V操作，增加信号量
    //这会唤醒在m_queuestat.wait()处被阻塞的工作线程
    m_queuestat.post();
    return true;
}

template< typename T >
void* threadpool< T >::worker( void* arg )
{
    //因为静态函数无法访问成员函数原因，我们直接传递this指针
    //注意，这里我们要利用原来的线程池对象，那里有我们的线程池数组，锁，信号量，这些都需要唯一的
    //新的线程池指针，指向原有的线程池
    threadpool* pool = ( threadpool* )arg;
    //线程池运行函数
    pool->run();
    return pool;
}

/*
线程池运行函数

*/
template< typename T >
void threadpool< T >::run() {

    while (!m_stop) {
        /*
        sem_wait函数将以原子操作方式将信号量减一,信号量为0时,sem_wait阻塞
        sem_post函数以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
        信号量最开始被初始化为0，工作线程在此处阻塞
        经过append操作，执行V操作，sem值大于0
        工作线程被唤醒竞争出一个线程，成功的工作现场进入，又执行P操作，导致sem值为0，其他线程被继续阻塞
        */
        m_queuestat.wait();
        //访问公共区域上锁
        m_queuelocker.lock();
        if ( m_workqueue.empty() ) {
            m_queuelocker.unlock();
            continue;
        }
        //取出队列前面的任务
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if ( !request ) {
            continue;
        }
        
        // printf("工作线程执行事件\n");
        //工作线程执行任务
        request->process();
    }

}

#endif
