#include "http_conn.h"

//网站根目录
const char *doc_root = "/home/shang/code/Tiny_Web_Server/github/MyTinyWebServer/MyWebServer/resources";

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 将文件描述符设置为非阻塞的,ET + 非阻塞
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );      //获取信息
    int new_option = old_option | O_NONBLOCK;   //并集
    fcntl( fd, F_SETFL, new_option );           //设置信息
    return old_option;                          //返回旧信息
}

//增添监视事件和文件描述符
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    //增添读取事件,使用ET模式,客户端断开事件
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    //保证一个线程处理一个socket,解决事件后记得重新设置,否则别的线程不能选中该socket
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符,对应之前设置的EPOLLONESHOT
//等到该线程处理完事件后,此socket不会被epoll_wait返回(不触发事件),所以需要重新设置
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}



//客户数量初始化
int http_conn::m_user_count = 0;

//epollfd初始化
//所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

//客户数据初始化
void http_conn::init(int sockfd, const sockaddr_in& address)
{
    //初始化
    m_sockfd = sockfd;
    m_address = address;

    //设定socket端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //该客户的连接也应该被监视,添加到epoll监视中
    addfd(http_conn::m_epollfd, m_sockfd, true);
    m_user_count++;

    init();
}

//初始化主状态机状态
void http_conn::init()
{
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;// 初始状态为检查请求行
    m_checked_index = 0;
    m_start_line = 0;
    m_read_index = 0;
    m_write_index = 0;
    m_content_length = 0;
    
    m_method = GET;     // 默认请求方式为GET
    m_url = nullptr;
    m_version = nullptr;
    m_host = 0;
    m_linger = true;   // 默认不保持链接  Connection : keep-alive保持连接

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_read_file, FILENAME_LEN);
}

//关闭用户连接
void http_conn::close_conn()
{
    //m_sockfd有效情况下
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if (m_read_index >= READ_BUFFER_SIZE) {
        printf("读入数据超出缓冲区\n");
        return false;
    }
    
    /*
    ET模式下使用非阻塞文件描述符读取文件
    listenfd不需要考虑阻塞非阻塞
    connfd需要考虑非阻塞使用

    因为是ET模式,只会触发一次,所以一次就要读完所有数据,我们需要加一个循环
    加嵌套循环后会发生另一个问题
    数据读完后,阻塞IO会一直等待对方写数据,停在这里,这个时候主线程阻塞了,如果有新的客户连接是无法处理的
    调成非阻塞IO就可以了,因为非阻塞 IO 如果没有数据可读时，会立即返回，并设置 errno。
    这里我们根据 EAGAIN 和 EWOULDBLOCK 来判断数据是否全部读取完毕了，如果读取完毕，就会正常退出循环了。
    */
    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //正常读完数据的情况,此时退出循环
                break;
            }
            return false;
        } else if (bytes_read == 0) { //对方关闭连接
            return false;
        } else {
            //已经读取数据的标记向后移动
            m_read_index += bytes_read;
        }
    }
    printf("%s", m_read_buf);
    return true;
}

//分散写入socket
bool http_conn::write()
{
    int temp = 0;
    
    if (bytes_to_send == 0) {
        //将要发送的字节为0,这一次响应结束
        //写事件已经结束,需要修改文件描述符connfd为监视读事件,oneshot让epoll_wait只响应事件一次
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        //有限状态机等状态重新初始化
        init();
        return true;
    }

    //要发送数据
    while (1)
    {
        //分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN) {
                printf("TCP写缓冲空间不够,再重新注册写事件一次\n");
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
    

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_index);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0) {
            //没有数据要发送
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            //如果是长连接
            if (m_linger) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

/*
工作线程处理http请求(之前显示调用线程的append函数插入请求,
然后睡眠的工作线程被条件变量唤醒去取队列的首任务,然后调用process函数执行请求)

process函数逻辑分为proces_read和process_write
process_read通过有限状态机分析HTTP请求报文
*/
void http_conn::process()
{
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    //请求不完整，需要继续读取客户数据
    if (read_ret == NO_REQUEST) {
        //需要客户端继续发送数据,所以需要重新设置connfd,否则因为oneshot的原因无法再响应事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    //生成响应报文
    //传入read_ret是因为我们要根据读HTTP的情况来进行不同的操作
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    //注册写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
    printf("the write_ret is %d\n");

}

//解析具体的一行
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;//储存当前读到的字符
    for ( ; m_checked_index < m_read_index; m_checked_index++)
    {
        temp = m_read_buf[m_checked_index];
        if (temp == '\r') {
            //说明当前行不完整,下一个需要判定的字符没有都进去
            if ((m_checked_index + 1) == m_read_index) {
                return LINE_OPEN;
            } 
            //正好是一行,我们将末尾的\r\n变为\0,处理过后m_checked_index为下一行的开头
            else if (m_read_buf[m_checked_index + 1] == '\n') {
                // printf("解析成功\n");
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            } 
        }
        else if(temp == '\n') {
            //说明第二行开头是\n
            //m_checked_index > 1排除开头就是\r\n的情况
            if ((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r')) {
                m_read_buf[ m_checked_index-1 ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}     

//解析请求行,需要获得请求方法,目标URL,HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");   //判断第二个参数中的字符哪个在text中最先出现
    if (!m_url) {
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';
    char *method = text; //GET\0
    if (strcasecmp(method, "GET") == 0) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';    // /index.html\0HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    /*
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {
        m_url += 7;
        //192.168.110.129:10000/index.html
        //在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
        //m_url = index.html
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    
    //主状态转移
    m_check_state = CHECK_STATE_HEADER;
    
    return NO_REQUEST;//还没有完全解析完报文
} 

//解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{   
    //遇到空行,说明头字段解析完成
    if(text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {//说明有请求体
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        //处理Connection头部字段 Connection: keep-alive
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        //处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}   

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    
    if ( m_read_index >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}        

//主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    HTTP_CODE ret = NO_REQUEST;       //处理的结果
    LINE_STATUS line_status = LINE_OK;
    char* text = 0;
    int i = 0;
    printf("以下是process_read()状态机解析结果\n");
    while ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)
            || ((line_status = parse_line()) == LINE_OK))
    {
        //return m_read_buf + m_start_line
        text = get_line();
        m_start_line = m_checked_index; //此时m_checked_index位置为下一行开始位置
        printf("%s\n", text);

        switch (m_check_state)
        {
            //请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                //语法错误直接结束
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            //请求头
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {    //获得完整的HTTP请求,就开始做出回应
                    return do_request();
                }
                break;
            }
            //请求体
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                //解析完语句只会,仍然会进入循环,所以借助
                //m_check_state==CHECK_STATE_CONTENT && line_status==LINE_OK
                //来循环,我们改成了LINE_OPEN,所以会退出循环
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }

        }
    }
    return NO_REQUEST;
}

//往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_index >= WRITE_BUFFER_SIZE) {
        return false;
    }

    //定义可变参数列表
    va_list arg_list;
    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区写,返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_index, WRITE_BUFFER_SIZE - m_write_index - 1, format, arg_list);
    
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_index)) {
        va_end(arg_list);
        return false;
    }
    //更新m_write_index文直
    m_write_index += len;
    //清空可变参数列表
    va_end(arg_list);

    return true;
}

//添加响应行
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加响应头
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);    //内容长度
    add_content_type();                 //类型
    add_linger();                       //长连接
    add_blank_line();                   //空行
}

bool http_conn::add_content_length(int content_length)
{
    return add_response( "Content-Length: %d\r\n", content_length );   
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果(do_request)，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        //表示服务器内部错误
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        }
        //表示客户请求语法错误
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }
        //表示服务器没有资源
        case NO_RESOURCE:
        {
            add_status_line(404, "error_404_title");
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }
        //表示客户对资源没有足够的访问权限
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }
        //文件请求,获取文件成功
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            //m_iv[0]写响应报文状态行,属性字段
            //m_iv[1]写响应体,也就是发送过去的文件的内容
            //使用m_iv储存两部分的开头地址和长度,然后调用writev一并发送过去
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_index;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            //发送两块
            m_iv_count = 2;
            //更新已发送字节大小
            bytes_to_send = m_write_index + m_file_stat.st_size;
            
            return true;
        }
        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_index;
    m_iv_count = 1;
    bytes_to_send = m_write_index;
    return true;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/shang/code/Tiny_Web_Server/MyWebServer/resources"
    strcpy(m_read_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_read_file + len, m_url, FILENAME_LEN -len - 1);
    printf("客户端所求文件路径为:%s\n", m_read_file);
    //获取m_read_file文件的相关的状态信息,-1失败,0成功
    if (stat(m_read_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    //判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        //无权限访问
        return FORBIDDEN_REQUEST;
    }

    //判断是否是根目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        //请求错误
        return BAD_REQUEST;
    }

    //以下是成功找到可访问文件的情况
    //以只读方式打开文件
    int fd = open(m_read_file, O_RDONLY);
    //创建内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!m_file_address) {
        //服务器内部错误
        return INTERNAL_ERROR;
    }
    close(fd);
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = nullptr;
    }
}


