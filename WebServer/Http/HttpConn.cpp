#include "HttpConn.h"

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

int HttpConn::user_count_ = 0;  //初始化用户数为0
int HttpConn::epollfd_ = -1;    //初始化epollfd为-1

//关闭连接,减少客户数量,关闭连接
void HttpConn::CloseConn(bool real_close)
{
    if (sockfd_) {
        printf("the write_ret is false close %d\n", sockfd_);
        //删除监视的事件
        utils_.RemoveFd(epollfd_, sockfd_);
        sockfd_ = -1;
        user_count_--;
    }
}

void HttpConn::Init(int sockfd,  const sockaddr_in &address)
{
    ///home/shang/code/WebServer/github/WebServer/resources
    doc_root_ = "/home/shang/code/WebServer/github/MyTinyWebServer/WebServer/resources";

    // 初始化套接字和地址
    sockfd_ = sockfd;
    address_ = address;

    //向epoll对象添加监视事件,oneshoot模式保证单个线程负责
    utils_.AddFd(epollfd_, sockfd_, true);
    user_count_++;
    
    //private 版本的 init();专门用来来初始化private成员变量
    Init();
}

//非阻塞读客户端数据到server的读缓冲中
bool HttpConn::ReadOnce()
{
    //如果读取数据大于缓冲区大小,返回false
    if (read_idx_ >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    //[ET模式]配合[非阻塞connfd]读取数据
    while (true) {
        bytes_read = recv(sockfd_, read_buf_ + read_idx_, READ_BUFFER_SIZE - read_idx_, 0);
        if (bytes_read == -1) {
            // 非阻塞ET模式下，需要一次性将数据读完，下次不会通知所以循环读完
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        //客户端关闭了connfd
        else if (bytes_read == 0) {
            return false;
        }
        // 更新指针
        read_idx_ += bytes_read;
    }
    
    return true;
}

//写将响应报文内容写入connfd中
bool HttpConn::Write()
{
    // 如果数据发送完毕,那么写事件完成,下一次注册读事件
    if (bytes_to_send_ == 0) {
        utils_.ModFd(epollfd_, sockfd_, EPOLLIN);
        //重新准备下一次读取事件,所以所有成员变量初始化为最初值
        Init();
        return true;
    }

    int temp;
    //非阻塞connfd,一次循环写完
    while (true) {
        temp = writev(sockfd_, iv_, iv_count_);
        //没有成功发送数据
        if (temp < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                utils_.ModFd(epollfd_, sockfd_, EPOLLOUT);
                return true;
            }
            UnMap();
            return false;
        }

        //已发送数据增加,将发送数据减少
        bytes_have_send_ += temp;
        bytes_to_send_ -= temp;

        //因为可能多次发送,所以每次都要更新发送数据的起始位置和剩余发送数据大小
        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_to_send_ >= iv_[0].iov_len)
        {
            iv_[0].iov_len = 0;
            iv_[1].iov_base = file_address_ + (bytes_have_send_ - write_idx_);
            iv_[1].iov_len = bytes_to_send_;
        }
        else 
        {
            iv_[0].iov_base = write_buf_ + bytes_have_send_;
            iv_[0].iov_len = iv_[0].iov_len - bytes_have_send_;
        }

        //无数据发送了
        if (bytes_to_send_ <= 0) {
            UnMap();
            //修改为监视读事件
            utils_.ModFd(epollfd_, sockfd_, EPOLLIN);

            //如果长连接,重新初始化
            if (linger_) 
            {
                Init();
                return true;
            } 
            else 
            {
                return false;
            }
        }
    }
}


/*
从状态机工作逻辑
    分析读取的数据,将\r\n替换成\0\0,方便使用字符串操作获取一行
    enum LINE_STATUS
    {
        LINE_OK = 0,    //行解析成功
        LINE_BAD,       //行不正确
        LINE_OPEN       //未读取完整一行
    };
*/
HttpConn::LINE_STATUS HttpConn::ParseLine()
{
    char temp;
    for (; checked_idx_ < read_idx_; checked_idx_++)
    {   
        //temp为将要分析的字节
        temp = read_buf_[checked_idx_];
        //如果当前是\r字符，则有可能会读取到完整行
        if ('\r' == temp)
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if ((checked_idx_ + 1) == read_idx_)
                return LINE_OPEN;
            //下一个字符是\n，将\r\n改为\0\0
            else if (read_buf_[checked_idx_ + 1] == '\n')
            {
                read_buf_[checked_idx_++] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LINE_OK;
            }
            else
                return LINE_BAD;
        }
        else if (temp == '\n')
        {
            //前一个字符是\r，则接收完整(第一个字符不能是\n)
            if (checked_idx_ > 1 && read_buf_[checked_idx_ - 1] == '\r')
            {
                read_buf_[checked_idx_ - 1] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}

//解析请求行
HttpConn::HTTP_CODE HttpConn::ParseRequestLine(char* text)
{
    // GET /index.html HTTP/1.1

    // 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    // 请求行中最先含有空格和\t任一字符的位置并返回
    url_ = strpbrk(text, " \t");
    if (!url_) 
        return BAD_REQUEST;

    //将该位置改为\0，用于将前面 method 数据取出
    *url_++ = '\0';

    //取出数据，并通过与GET和POST比较，以确定请求方式
    char *method = text;
    if ( strcasecmp(method, "GET") == 0 )  // 忽略大小写比较
        method_ = GET;
    else if ( strcasecmp(method, "POST") == 0)
        method_ = POST;
    else 
        return BAD_REQUEST;
    
    //检索HTTP版本号位置
    version_ = strpbrk(url_, " \t");
    if (!version_)
        return BAD_REQUEST;
    //方便得到之前的url_
    *version_++ = '\0';
    if (strcasecmp(version_, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    if (strncasecmp(url_, "http://", 7) == 0) {
        url_ += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        url_ = strchr( url_, '/' );
    }
    if (!url_ || url_[0] != '/')
        return BAD_REQUEST;

    //状态转移
    check_state_ = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析请求头
HttpConn::HTTP_CODE HttpConn::ParseHeader(char* text)
{
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( content_length_ != 0 ) {
            check_state_ = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection:\tkeep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            linger_ = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        content_length_ = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        host_ = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
HttpConn::HTTP_CODE HttpConn::ParseContent(char* text)
{
    //判断buffer中是否读取了消息体
    if (read_idx_ >= (content_length_ + checked_idx_))
    {
        text[content_length_] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/*
响应函数：
    打开文件资源
    创建共享内存区域

当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
映射到内存地址m_file_address处，并告诉调用者获取文件成功
*/
HttpConn::HTTP_CODE HttpConn::DoRequest()
{
    // "/home/nowcoder/webserver/resources"
    strcpy(read_file_, doc_root_);
    int len = strlen(doc_root_);
    strncpy(read_file_ + len, url_, FILENAME_LEN - len - 1);

    printf("The HTPP request's read_file_ is %s\n", read_file_);

    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( read_file_, &file_stat_ ) < 0 ) {
        return NO_RESOURCE;
    }
    // 判断访问权限
    if ( ! ( file_stat_.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( file_stat_.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( read_file_, O_RDONLY );
    // 创建内存映射
    file_address_ = ( char* )mmap( 0, file_stat_.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

//释放映射区
void HttpConn::UnMap()
{
    if (file_address_) {
        munmap(file_address_, file_stat_.st_size);
        file_address_ = 0;
    }
}

//报错,因为HTTP_CODE类型是在HttpConn内的,需要带上类名
HttpConn::HTTP_CODE HttpConn::ProcessRead()
{
    // LINE_STATUS 从状态机的状态
    LINE_STATUS line_status = LINE_OK;
    // 报文解析结果默认为NO_REQUEST
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    
    while ((check_state_ == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = ParseLine()) == LINE_OK))
    {
        //得到一行字符串
        text = GetLine();

        //更新行开始处
        start_line_ = checked_idx_;

        //主状态机的三种状态转移逻辑
        switch (check_state_)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = ParseRequestLine(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            //一次处理一个头字段,返回NO_REQUEST
            //处理完成并且无请求体,返回GET_REQUEST
            case CHECK_STATE_HEADER:
            {
                ret = ParseHeader(text);
                if (ret == BAD_REQUEST) 
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST) 
                    return DoRequest();
                break;
            }     
            //Post有请求体
            case CHECK_STATE_CONTENT:
            {
                //解析消息体
                ret = ParseContent(text);
                //完整解析POST请求后，跳转到报文响应函数
                if (ret == GET_REQUEST)
                    return DoRequest();
                
                line_status = LINE_OPEN;
                break;
            }
        }
    }
}

bool HttpConn::AddResponse(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (write_idx_ >= WRITE_BUFFER_SIZE)
        return false; 
    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(write_buf_ + write_idx_, WRITE_BUFFER_SIZE - 1 - write_idx_, format, arg_list);

    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - write_idx_))
    {
        //清空可变参列表
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    write_idx_ += len;
    
    //清空可变参列表
    va_end(arg_list);

    return true;
        
}

//增加状态行
bool HttpConn::AddStatueLine(int status, const char *title)
{
    return AddResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//增加响应头
bool HttpConn::AddHeaders(int content_len)
{
    return AddContentLength(content_len) && AddLinger() && AddBlankLine();
}

//添加消息报头，具体的添加文本长度、连接状态和空行
bool HttpConn::AddContentLength(int content_len)
{
    return AddResponse("Content-Length:%d\r\n", content_len);
}

//添加文本类型，这里是html
bool HttpConn::AddContentType()
{
    return AddResponse("Content-Type:%s\r\n", "text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool HttpConn::AddLinger()
{
    return AddResponse("Connection:%s\r\n", (true == linger_) ? "keep-alive" : "close");
}

//添加空行
bool HttpConn::AddBlankLine()
{
    return AddResponse("%s", "\r\n");
}

//添加文本content
bool HttpConn::AddContent(const char *content)
{
    return AddResponse("%s", content);
}

//写响应报文
bool HttpConn::ProcessWrite(HTTP_CODE ret)
{
    switch (ret)
    {
        //内部错误，500
        case INTERNAL_ERROR:
        {
            AddStatueLine(200, error_500_title);
            AddHeaders(strlen(error_500_form));
            if (!AddContent(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST:
        {
            AddStatueLine(404, error_400_title);
            AddHeaders(strlen(error_400_form));
            if (!AddContent(error_400_form))
                return false;
            break;
        }
        //文件存在:200
        case FILE_REQUEST:
        {
            printf("PROCESSWRIET RETURN FILE_REQUEST\n");
            AddStatueLine(200, ok_200_title);
            //如果请求的资源存在
            if (file_stat_.st_size != 0)
            {
                AddHeaders(file_stat_.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向write_idx——
                iv_[0].iov_base = write_buf_;
                iv_[0].iov_len = write_idx_;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                iv_[1].iov_base = file_address_;
                iv_[1].iov_len = file_stat_.st_size;
                iv_count_ = 2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send_ = write_idx_ + file_stat_.st_size;
                return true;
            }
            else {
                //如果请求的资源大小为0，则返回空白html文件
                const char *ok_string = "<html><body></body></html>";
                AddHeaders(strlen(ok_string));
                if (!AddContent(ok_string))
                    return false;            
            }
        }
        default:
            return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    iv_[0].iov_base = write_buf_;
    iv_[0].iov_len = write_idx_;
    iv_count_ = 1;
    bytes_to_send_ = write_idx_;
    return true;
}

/*
处理HTTP请求
    包含分析读取的数据和写回响应信息
    分为ProcessRead ProcessWrite
*/
void HttpConn::Process()
{   
    printf("The HTTP request is \n%s", read_buf_); //测试读取到的数据

    HTTP_CODE read_ret = ProcessRead();

    //没有读完数据情况
    if (read_ret == NO_REQUEST) {
        //设置了EPOLLSHOOT,epollfd已经删除了该fd,所以需要重新设置事件
        utils_.ModFd(epollfd_, sockfd_, EPOLLIN);
        return;
    }

    //调用 ProcessWrite 完成报文响应，我们传入了读函数返回值作为判断
    bool write_ret = ProcessWrite(read_ret);
    printf("The write_buf_ response is \n %s\n", write_buf_);
    if (!write_ret) {
        CloseConn();
    }
    //该注册写事件了
    utils_.ModFd( epollfd_, sockfd_, EPOLLOUT);
}

void HttpConn::Init()
{       
    timer_flag_ = 0;
    event_finish_ = 0;

    //分析报文行所需要数据
    read_idx_ = 0;      //已读数据的下一位
    checked_idx_ = 0;   //当前已检查数据
    start_line_ = 0;    //当前行位置
    
    write_idx_ = 0;     //修改
    
    //主状态机及其分析对应变量
    check_state_ = CHECK_STATE_REQUESTLINE;
    method_ = GET;
    url_ = 0;
    host_ = 0;
    version_ = 0;
    content_length_ = 0;    
    linger_ = false;

    // 对读、写、文件名缓冲区初始化为'\0'
    memset(read_buf_, '\0', READ_BUFFER_SIZE);
    memset(write_buf_, '\0', WRITE_BUFFER_SIZE);
    memset(read_file_, '\0', FILENAME_LEN);
}