## HTTP报文解析

```c++
root@iZwz9eojvzsrz78f673t51Z:/home/shang/code# telnet 127.0.0.1 30000
Trying 127.0.0.1...
Connected to 127.0.0.1.
Escape character is '^]'.
GET /index.tenl HTTP/1.1
HOST: www.lincoding.com

I get a correct result
Connection closed by foreign host.
```

```c++
root@iZwz9eojvzsrz78f673t51Z:/home/shang/code/C++/test3# ./server 127.0.0.1 30000
The request method is GET
The request URL is: /index.tenl
the request host is: www.lincoding.com
```

**main**

```c++
char buffer[ BUFFER_SIZE ]; /*读缓冲区*/
memset( buffer, '\0', BUFFER_SIZE );
int data_read = 0;
int read_index = 0; /*当前已经读取了多少字节的客户数据*/
int checked_index = 0; /*当前已经分析完了多少字节的客户数据*/
int start_line = 0; /*行在buffer中的起始位置*/
```

**parse_content（主状态机，用于解析HTTP报文）**

`main`函数在建立好连接之后，首先调用函数`parse_content`

此函数用于分析`HTTP`报文，接收`buffer`，`checked_index`，`checkstate`，`read_index`，`startline`作为参数，最终返回`HTTP_CODE`，对于`HTTP`报文的处理结果

```c++
/*
服务器处理HTTP请求的结果：
    NO_REQUEST表示请求不完整，需要继续读取客户数据；
    GET_REQUEST表示获得了一个完整的客户请求；BAD_REQUEST表示客户请求有语法错误；
    FORBIDDEN_REQUEST表示客户对资源没有足够的访问权限；
    INTERNAL_ERROR表示服务器内部错误；
    CLOSED_CONNECTION表示客户端已经关闭连接了
*/
enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, FORBIDDEN_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
```

先使用从状态机，解析出一行的内容，然后再根据当前的主状态机状态，选择不同的`case`，执行不同的操作。执行完毕后，在各自的函数内改变主状态机的状态，下一次循环中进入不同的`case`。

```c++
//主状态机状态
enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
//从状态机状态
enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
```

当然，这一切都是建立在我们解析出了一个完整行的前提下

` while( ( linestatus = parse_line( buffer, checked_index, read_index ) ) == LINE_OK )`

```c++
switch ( checkstate ) /*checkstate记录主状态机当前的状态*/
{
    case CHECK_STATE_REQUESTLINE: /*第一个状态，分析请求行*/
        {
            retcode = parse_requestline( szTemp, checkstate );
            if ( retcode == BAD_REQUEST )
            {
                return BAD_REQUEST;
            }
            break;
        }
    case CHECK_STATE_HEADER: /*第二个状态，分析请求头*/
        {
            retcode = parse_headers( szTemp );
            if ( retcode == BAD_REQUEST )
            {
                return BAD_REQUEST;
            }
            else if ( retcode == GET_REQUEST )
            {
                return GET_REQUEST;
            }
            break;
        }
    default:
        {
            return INTERNAL_ERROR;
        }
}
```

**parse_line（从状态机，用于解析出一行的内容）**

我们是用缓冲区`buffer`接收`client`的数据，首先我们分析该数据能不能凑出一行，如果读到了完整的一行，我们需要将其末尾的`\r\n`赋值为`\0\0`，这样子字符串函数才能根据末尾字符`\0`直接截取得到字符串。

而我们正是使用从状态机来将`buffer`的字符变成`\0`的，在这里有三种情况

首先先循环遍历我们的数据

- 如果当前字符为`\r`
  - 我们需要判断后一位是不是`\n`，如果我们发现`\r`是当前`buffer`的最后一位，也就是`buffer[read_index - 1]`，则说明我们读取的行不完全，还需要继续读取后面的数据来判断
  - 如果下一个字符为`\n`，则我们需要将`\r\n`都置为`\0`
  - 否则，我们得到的`HTTP`报文存在语法问题
- 如果当前字符为`\n`
  - 如果前一个字符是`\r`，则两个置零。同时有一个限制，就是开头不能为`\r\n`，`if( ( checked_index > 1 ) && buffer[ checked_index - 1 ] == '\r' )`
  - 如果不满足`if (...)`，则返回`LINE_BAD`
- 如果之前的内容都分析完，结果还没有得到`\r`，则说明当前客户数据读不完一个完整行，还需要继续读取，返回`LINE_OPEN`

