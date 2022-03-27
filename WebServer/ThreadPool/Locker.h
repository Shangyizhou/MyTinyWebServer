#ifndef Locker_H
#define Locker_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 线程同步机制封装类

// 互斥锁类,保证线程互斥访问临界区
class Locker {
public:
    Locker() {
        //创建互斥锁
        if(pthread_mutex_init(&mutex_, NULL) != 0) {
            throw std::exception();
        }
    }

    ~Locker() {
        //销毁互斥锁
        pthread_mutex_destroy(&mutex_);
    }

    bool Lock() {
        //上锁
        return pthread_mutex_lock(&mutex_) == 0;
    }

    bool UnLock() {
        //解锁
        return pthread_mutex_unlock(&mutex_) == 0;
    }

    //取锁
    pthread_mutex_t *get()
    {
        return &mutex_;
    }

private:
    pthread_mutex_t mutex_;
};


// 条件变量类
class Cond {
public:
    Cond(){
        if (pthread_cond_init(&cond_, NULL) != 0) {
            throw std::exception();
        }
    }
    ~Cond() {
        pthread_cond_destroy(&cond_);
    }

    bool Wait(pthread_mutex_t *mutex_) {
        int ret = 0;
        ret = pthread_cond_wait(&cond_, mutex_);
        return ret == 0;
    }
    bool TimeWait(pthread_mutex_t *mutex_, struct timespec t) {
        int ret = 0;
        ret = pthread_cond_timedwait(&cond_, mutex_, &t);
        return ret == 0;
    }
    bool Signal() {
        return pthread_cond_signal(&cond_) == 0;
    }
    bool BroadCast() {
        return pthread_cond_broadcast(&cond_) == 0;
    }

private:
    pthread_cond_t cond_;
};



/*
信号量类
    初始化信号量值为0,用于设置线程同步
        对于工作线程，只有请求队列有任务时才会执行任务
        而sem <= 0 的时候是会阻塞线程的，所以设置为0一开始是阻塞线程的
        当有任务的时候，执行V操作，增加信号量值
        线程取走任务后，执行P操作，减少信号量值
*/
class Sem {
public:
    Sem() {
        if( sem_init( &sem_, 0, 0 ) != 0 ) {
            throw std::exception();
        }
    }
    Sem(int num) {
        if( sem_init( &sem_, 0, num ) != 0 ) {
            throw std::exception();
        }
    }
    ~Sem() {
        sem_destroy( &sem_ );
    }
    // 等待信号量，sem值小于等于0，则线程阻塞
    bool Wait() {
        return sem_wait( &sem_ ) == 0;
    }
    // 增加信号量
    bool Post() {
        return sem_post( &sem_ ) == 0;
    }
private:
    sem_t sem_;
};

#endif