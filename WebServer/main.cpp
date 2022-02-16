#include "WebServer.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        perror("Usage: ./server port\n");
    }

    int port = atoi(argv[1]);
    int thread_nums = 8;
    int max_queue_nums = 10000;

    WebServer webserver(port, thread_nums, max_queue_nums);
    
    webserver.CreateThreadPool();

    webserver.ListenEvents();
    
    webserver.LoopEvents();
    
    return 0;
}