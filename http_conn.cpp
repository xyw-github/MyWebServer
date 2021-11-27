#include "http_conn.h"


/*定义HTTP响应的一些状态信息*/
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";


//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/vscode/youshuang/root";

http_conn::http_conn()
{

}

http_conn::~http_conn()
{

}

/*静态变量初始化*/
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

int setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event ev;
    ev.events = EPOLLIN | EPOLLET  | EPOLLRDHUP;
    ev.data.fd = fd;
    if(one_shot)
    {
        ev.events |=  EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&ev);
    setnonblocking(fd);
}

void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLIN | EPOLLET  | EPOLLRDHUP | EPOLLONESHOT;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}


void http_conn::init(int sockfd,const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    int reuse = 1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CKECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    m_cgi = 0;

    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

void http_conn::close_conn(bool read_close)
{
    if(read_close && m_sockfd != -1)
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

/*从状态机，用于解析出一行内容*/
http_conn::LINE_STATES http_conn::parse_line()
{
    char temp;
    /*checked_index指向当前正在分析的字节，read_index指向buffer中客户数据的尾部的下一个字节。
    buffer中第0~checked_index字节都已经分析完毕，第checked_index~read_index-1字节由下面循环挨个分析*/
    for(; m_checked_idx < m_read_idx ;++m_checked_idx)
    {
        //获取当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        /*如果当前是 \r ,则说明可能读到一个完整的行*/
        if(temp == '\r')
        {
            /*如果\r是目前buffer中最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行，
            返回LINE_OPEN表示还要继续读取客户端数据进一步分析*/
            if((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if(m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            /*否则的话说明http请求存在语法问题*/
            return LINE_BAD;
        }
        else if(temp == '\n')
        {
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx-1] == '\r')
            {
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            /*否则的话说明http请求存在语法问题*/
            return LINE_BAD;
        }
    }
    /*所有的都分析完，还未遇到'\r',说明还需要从客户端读数据分析*/
    return LINE_OPEN;
}

/*循环读取数据，直到无数据可读或对方关闭连接*/
bool http_conn::read()
{
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
        if(bytes_read == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

/*分析请求行*/
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //返回text中第一个包含" \t"中字符的字符
    m_url = strpbrk(text," \t");
    //如果没有空白字符或\t字符，则HTTP请求必定有问题
    
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if( strcasecmp(method,"GET") == 0 ) // get请求
    {
        m_method = GET;
    }
    else if( strcasecmp(method,"POST") == 0 ) // post请求
    {
        m_method = POST;
        m_cgi = 1;
        printf("method = %s\n",method);
    }
    else
    {
        return BAD_REQUEST;
    }

    m_url += strspn(m_url," \t");
    m_version = strpbrk(m_url," \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version," \t");
    /*仅支持HTTP/1.1*/
    if(strcasecmp(m_version,"HTTP/1.1") != 0 )
    {
        return BAD_REQUEST;
    }
    /*检查url是否合法*/
    if(strncasecmp(m_url,"http://",7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url,'/');
    }
    if(!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }

    //当url为/时，显示默认index.html界面
    if (strlen(m_url) == 1)
        strcat(m_url, "index.html");
    /*HTTP请求行处理完毕，状态转移到头部字段的分析*/
    m_check_state = CKECK_STATE_HEADER;
    return NO_REQUEST;
}
/*分析请求头*/
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
     /*遇到一个空行，说明头部字段解析完毕*/
    if(text[0] == '\0')
    {
        /*如果HTTP请求有请求体，则需要继续读取m_content_length个字节的消息体，并将状态机转移*/
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            printf("252\n");
            return NO_REQUEST;
        }
        // printf("253 %s\n",text);
        /*否则就说明我们已经读到了一个完整的HTTP请求*/
        return GET_REQUEST;
    }
    /*处理Connection:头部字段*/
    else if( strncasecmp(text,"Connection:",11) == 0)
    {
        text += 11;
        text += strspn(text," \t");
        if( strcasecmp(text,"keep-alive") == 0 )
        {
            m_linger = true;
        }
        printf("processing： %s\n",text);
    }
    /*处理Content-length头部字段*/
    else if(strncasecmp(text,"Content-length:",15) == 0)
    {
        text += 15;
        text += strspn(text," \t");
        m_content_length = atol(text);
        printf("processing： %s\n",text);
    }
    //处理Host头部字段
    else if( strncasecmp(text,"Host:",5) == 0)
    {
        text += 5;
        text += strspn(text," \t");
        m_host = text;
        printf("processing： %s\n",text);
    }
    //其他的字段不处理
    else
    {
        printf("oop! unknow header：%s\n",text);
    }

    return NO_REQUEST;
}
/*我们并没有解析HTTP请求的消息体，只是判断它是否被完整读入*/
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    printf("297\n");
    if(m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        printf("%s\n",text);
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/*主状态机*/
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATES linestatus = LINE_OK; //记录当前行的读取状态
    HTTP_CODE retcode = NO_REQUEST;   //记录HTTP请求的处理结果

    char *text = 0;

    /*主状态机，用于从buffer中取出所有完整的行*/
    while(((linestatus = parse_line()) == LINE_OK) || ((m_check_state == CHECK_STATE_CONTENT) && (linestatus == LINE_OK)))
    {
        text = get_line();  //获取到当前所要分析行的起始位置
        m_start_line = m_checked_idx; //记录下一行的起始位置

        printf("got 1 http line: %s\n",text);
        printf("%d   %d\n",m_check_state,linestatus);

        /*checkstate是状态机当前的状态*/
        switch(m_check_state)
        {
            case CKECK_STATE_REQUESTLINE:
            {
                retcode = parse_request_line(text);
                if(retcode == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CKECK_STATE_HEADER:
            {
                retcode = parse_headers(text);
                if(retcode == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(retcode == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                // printf("351\n");
                retcode = parse_content(text);
                
                if(retcode == GET_REQUEST)
                {
                    return do_request();
                }
                linestatus = LINE_OPEN;
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

/*当得到一个完整，正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，
对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);

    // printf("----------------\n");
    // printf("m_url:%s\n %s\n",m_url,m_real_file);
    const char *p = strrchr(m_url,'/');
    
    // 处理cgi请求
    if( m_cgi == 1 && ( *(p+1) == '2' || *(p+1) == '3' ) )
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);
        printf("-----------------------\n");
        printf("\n%s\n",m_real_file);

    }

    if(*(p+1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real,"/register.html");
        strncpy(m_real_file + len,m_url_real,strlen(m_url_real));

        free(m_url_real);
    }

    else
        strncpy(m_real_file+len,m_url,FILENAME_LEN - len -1);

    if(stat(m_real_file,&m_file_stat) < 0)
    {
        return NO_REQUEST;
    }
    if( ! (m_file_stat.st_mode & S_IROTH) )
    {
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }
    
    int fd = open(m_real_file,O_RDONLY);
    m_file_address = (char *)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}

/*对内存*/
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address = 0;
    }
}

/*写HTTP响应*/
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;

    if(bytes_to_send == 0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        temp = writev(m_sockfd,m_iv,m_iv_count);
        if(temp <= -1)
        {
            /*如果Tcp写缓冲没有空间，则等待下一轮EPOLLOUT事件，*/
            if(errno == EAGAIN)
            {
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;

        if(bytes_to_send <= bytes_have_send)
        {
            /*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否关闭连接*/
            unmap();
            if(m_linger)
            {
                init();
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }
        }
        else
        {
            modfd(m_epollfd,m_sockfd,EPOLLIN);
            return false;
        }
    }
}


bool http_conn::add_response(const char *format, ...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len = vsnprintf(m_write_buf + m_write_idx,WRITE_BUFFER_SIZE - 1 - m_write_idx,format,arg_list);

    if(len >= (WRITE_BUFFER_SIZE -1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status,const char *title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}
bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length(int content_length)
{
    return add_response("Content-Length: %d\r\n",content_length);
}
bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n",(m_linger == true) ? "keep-alive":"close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

bool http_conn::add_content(const char*content)
{
    return add_response("%s",content);
}

/*根据读的结果来准备要发送的内容*/
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
    case INTERNAL_ERROR:
    {
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(! add_content(error_500_form))
            {
                return false;
            }
            break;
        }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    //bytes_to_send = m_write_idx;
    return true;

}

/*由线程池中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST)
    {
        // 使用此操作直接触发一次EPOLLIN，进而读取数据
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);
    if(! write_ret)
    {
        close_conn();
    }
    /*直接调用epoll_ctl()重新设置一下event就可以了,
     event跟原来的设置一模一样都行(但必须包含EPOLLOUT)，关键是重新设置，就会马上触发一次EPOLLOUT事件。*/
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}