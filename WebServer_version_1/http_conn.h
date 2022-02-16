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

#include "locker.h"
// #include "timer.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;        //设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048;   //设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;  //设置写缓冲区m_write_buf大小

    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    
public:
    http_conn() {}
    ~http_conn() {}

public:
    
    void init(int sockfd, const sockaddr_in& address);  //初始化新连接客户的数据
    void init();                                        //被上面调用,用于初始化字符串分析的数据
    
    void close_conn();                                  //关闭连接
    void process();                                     //处理客户端的请求（分为读取和写入）
    bool read();                                        //非阻塞读
    bool write();                                       //非阻塞写
    
public:
    /*
    主状态机每次解析具体的一行，根据返回的行状态来确定后面行为
        LINE_OPEN:行不完全,还需要继续往后读取
        LINE_BAD:行出错,说明客户端发送数据不规范
        LINE_OK:我们读到了完整的一行,接下来要解析该行的内容
    最开始先解析请求行(CHECK_STATE_REQUESTLINE)
        如果请求行解析成功,会在函数内部改变主状态机的状态为(CHECK_STATE_HEADER)
    */

    HTTP_CODE process_read();
    
    //下面这一组函数被process_read调用以分析HTTP请求
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();                                   //解析具体的一行
    HTTP_CODE parse_request_line(char *text);                   //解析请求行
    HTTP_CODE parse_headers(char *text);                        //解析请求头
    HTTP_CODE parse_content(char *text);                        //解析请求体
    HTTP_CODE do_request();                                     //做出处理(根据解析出的请求文件,根据文件是否存在、可读做出不同的回应,成功则调用mmap)    
    void unmap();                                               //释放内存映射区
    
    
    bool process_write(HTTP_CODE ret);

    //这一组函数被process_write调用以填充HTTP应答。
    bool add_response(const char* format, ...);                 // 往写缓冲中写入待发送的数据
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_len);
    bool add_content_type();
    bool add_content(const char *content);
    bool add_content_length(int conten_length);
    bool add_linger();
    bool add_blank_line();


    //这一组函数被process_write调用以填充HTTP应答。

public:
    static int m_epollfd;               //所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;            //统计用户的数量

private:
    int m_sockfd;                       //该HTTP连接的socket
    sockaddr_in m_address;              //该HTTP连接的地址

    char m_read_buf[READ_BUFFER_SIZE];  //读缓冲区
    int m_read_index;                   //读缓冲区已经读入的客户端数据
    int m_checked_index;                //当前解析的字符
    int m_start_line;                   //当前行在m_read_buf中的位置

    CHECK_STATE m_check_state;          //主状态机当前所处的状态        
    METHOD m_method;                    //请求方法

    char m_read_file[FILENAME_LEN];     //客户请求的目标文件路径
    char* m_url;                        //客户请求的目标文件的文件米
    char* m_version;                    //HTTP协议版本号,仅支持HTTP/1.1
    char* m_host;                       //主机名
    bool m_linger;                      //HTTP请求是否保持长连接
    int m_content_length;               //HTTP请求的消息总长度

    char m_write_buf[WRITE_BUFFER_SIZE];// 写缓冲区
    int m_write_index;                  // 写缓冲区中待发送的字节数
    char *m_file_address;               // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;            // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];               // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    int bytes_to_send;                  // 将要发送的数据的字节数
    int bytes_have_send;                // 已经发送的字节数

};

#endif