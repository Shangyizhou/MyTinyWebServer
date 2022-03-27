## HTTP_CODE

表示HTTP请求的处理结果，在头文件中初始化了八种情形，在报文解析时只涉及到四种。

- NO_REQUEST
  - 请求不完整，需要继续读取请求报文数据
  - 跳转主线程继续监测读事件

- GET_REQUEST
  - 获得了完整的HTTP请求
  - 调用do_request完成请求资源映射

- NO_RESOURCE
  - 请求资源不存在
  - 跳转process_write完成响应报文

- BAD_REQUEST
  - HTTP请求报文有语法错误或请求资源为目录
  - 跳转process_write完成响应报文

- FORBIDDEN_REQUEST
  - 请求资源禁止访问，没有读取权限
  - 跳转process_write完成响应报文

- FILE_REQUEST
  - 请求资源可以正常访问
  - 跳转process_write完成响应报文

- INTERNAL_ERROR
  - 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发


### 代码分析

> 状态转移图

![image-20220322174214265](https://syz-picture.oss-cn-shenzhen.aliyuncs.com/image-20220322174214265.png)

## 重点函数分析

### process

工作现场领取任务，调用任务处理函数`Process()`

`Process()`

- `ProcessRead()`：使用状态机分析`read_buf`中的报文
- `ProcessWrite()`：根据分析报文的结果，写响应报文

```c++
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
```

### DoRequset

当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功

```c++
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
```

```c++
//释放映射区
void HttpConn::UnMap()
{
    if (file_address_) {
        munmap(file_address_, file_stat_.st_size);
        file_address_ = 0;
    }
}
```

### ProcessWrite

根据`ProcessRead()`返回的`HTTP_CODE`写不同的响应报文

```c++
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
```

若文件存在，则使用`writev`函数发送文件，此函数可以定义两个向量，向两个向量同时写。为什么要这么使用呢，我们发送的信息应该分为两个部分，一个为响应报文提示内容，一个为请求文件的内容。

```c++
struct iovec iv_[2];      //io向量机制iovec
int iv_count_;            //发送部分数
int bytes_to_send_;       //剩余发送字节数
int bytes_have_send_;     //已发送字节数
```

每个`iovec`变量都需指明它的`起始地址`和`长度`

### Write

现在，我们需要将发送缓冲区的内容发送到对应的`connfd`

- 我们使用的是非阻塞文件描述符，所以需要一次性写完（套一个循环）
- 这里需注意多次发送的问题，因为我们每次都要更新指针的位置，所以也专门设置了`将要发送的字节`和`已发送的字节`
  - 若发送一次没有发送完成，我们需要判断已发送字节数是否超过第一个部分的发送字节数
- 无数据发送了，我们需要重新注册读事件，并且根据该连接属性做出不同操作
  - 长连接，调用`Init()`重置相关变量，不关闭`connfd`，返回`true`
  - 短连接，关闭`connfd`，返回`false`

```c++
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
```







































