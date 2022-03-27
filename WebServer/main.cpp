#include "./WebServer/WebServer.h"

int main(int argc, char* argv[])
{
    //暂时未设置命令行解析
    if (argc < 2) {
        perror("Usage: ./server port\n");
    }

    int port = atoi(argv[1]);
    int thread_nums = 8;
    int max_queue_nums = 10000;

    WebServer webserver(port, thread_nums, max_queue_nums);
    
    //创建线程池
    webserver.CreateThreadPool();

    //监听事件
    webserver.ListenEvents();
    
    //事件循环
    webserver.LoopEvents();
    
    return 0;
}