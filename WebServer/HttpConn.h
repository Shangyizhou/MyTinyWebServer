#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#include <string.h>
#include <stdarg.h>

#include "Utils.h"



class HttpConn 
{
public:
    static const int FILENAME_LEN = 200;        //设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048;   //设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;  //设置写缓冲区m_write_buf大小
    //报文的请求方法，本项目只用到GET和POST
    enum METHOD 
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    HttpConn() {}
    ~HttpConn() {}
    
public:
    void Init(int sockfd, const sockaddr_in &address);
    //读取浏览器端发来的数据
    bool ReadOnce();
    bool Write();

    //处理HTTP请求
    void Process();
    
    HTTP_CODE ProcessRead();
    bool ProcessWrite(HTTP_CODE ret);
    
    char* GetLine() { return read_buf_ + start_line_;}
    LINE_STATUS ParseLine();
    HTTP_CODE ParseRequestLine(char* text);
    HTTP_CODE ParseHeader(char* text);
    HTTP_CODE ParseContent(char* text);
    HTTP_CODE DoRequest();

    void UnMap();
    void CloseConn(bool real_close = true);

    bool AddResponse(const char* format, ...);
    bool AddStatueLine(int status, const char *title);
    bool AddHeaders(int content_len);
    bool AddLinger();
    bool AddContentType();
    bool AddBlankLine();
    bool AddContent(const char *content);
    bool AddContentLength(int content_len);



public:
    /*
    start_line_是已经解析的字符
    GetLine用于将指针向后偏移，指向未处理的字符
    从状态机读取一行，分析是请求报文的哪一部分
    */


public:
    int sockfd_;//客户端套接字
    struct sockaddr_in address_;//客户端地址

public:
    static int epollfd_;    //我们还需要监视connfd的读事件,所以也需要上树
    static int user_count_; //客户端总数

public:
    Utils utils_;            //工具类

private:
    //专门用来来初始化private成员变量
    void Init();
    char read_buf_[READ_BUFFER_SIZE];   //读缓冲区
    int read_idx_;                      //缓冲区中read_buf_中数据的最后一个字节的下一个位置
    int checked_idx_;                   //read_buf_读取的位置m_checke
    int start_line_;                    //read_buf_中已经解析的字符个数


    //解析请求报文中对应的变量
    char read_file_[FILENAME_LEN];
    char *doc_root_;        //请求文件的根目录
    char *url_;             //请求URL
    char *version_;         //http版本
    char *host_;            //对方IP
    int content_length_;    //请求体字节数
    bool linger_;           //是否长连接

    //存储发出的响应报文数据    
    char write_buf_[WRITE_BUFFER_SIZE];  
    int write_idx_;           //buffer长度
    CHECK_STATE check_state_; //主状态机状态
    METHOD method_;           //请求方法

    struct stat file_stat_;   //文件属性
    char *file_address_;      //内存映射区 

    struct iovec iv_[2];      //io向量机制iovec
    int iv_count_;            //发送部分数
    int bytes_to_send_;       //剩余发送字节数
    int bytes_have_send_;     //已发送字节数
};

#endif