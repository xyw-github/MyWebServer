#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include"common.h"
#include"locker.h"


class http_conn
{
public:
    /*文件名的最大长度*/
    static const int FILENAME_LEN = 200;
    /*读缓冲区大小*/
    static const int READ_BUFFER_SIZE = 2048;
    /*写缓冲区大小*/
    static const int WRITE_BUFFER_SIZE = 1024;
    /*HTTP请求方法*/
    enum METHOD {GET = 0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT,PATCH};
    /*解析客户请求时，主状态机状态*/
    /*主状态机的两种可能状态，分别表示：当前正在分析请求行，当前正在分析请求头部，当前正在分析请求内容*/
    enum CHECK_STATE { CKECK_STATE_REQUESTLINE = 0,CKECK_STATE_HEADER,CHECK_STATE_CONTENT};    
    /*服务器处理HTTP请求的可能结果*/
    /*服务器处理http请求的结果：NO_REQUEST表示请求不完整，需要继续读取客户数据；GET_REQUEST表示获取了一个完整的客户请求；
    BAD_REQUEST表示客户端请求有语法错误；FORBIDDEN_REQUEST表示客户对资源没有充足的访问权限；INTERNAL_ERROR表示服务器内部错误；
    CLOSED_CONNECTION表示客户端已经关闭连接。*/
    enum HTTP_CODE { NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,
    INTERNAL_ERROR,CLOSED_CONNECTION };
    /*从状态机的三种可能状态，即为行的读取状态，分别表示：读取到一个完整的行、行出错和行数据尚不完整*/
    enum LINE_STATES { LINE_OK = 0,LINE_BAD,LINE_OPEN };
    
public:
    http_conn();
    ~http_conn();

public:
    /*初始化新接受的连接*/
    void init(int sockfd,const sockaddr_in& addr);
    /*关闭连接*/
    void close_conn(bool read_close = true);
    /*处理客户请求*/
    void process();
    /*非阻塞读操作*/
    bool read();
    /*非阻塞写操作*/
    bool write();

private:
    /*初始化连接*/
    void init();
    /*解析HTTP请求*/
    HTTP_CODE process_read();
    /*填充HTTP应答*/
    bool process_write(HTTP_CODE ret);

    /*下面一组函数被process_read调用以分析HTTP请求*/
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char* get_line(){
        return m_read_buf + m_start_line;
    }
    LINE_STATES parse_line();

    /*下面这一组被process_write调用以填充HTTP应答*/
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status,const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
public:    
    /*所有的socket上的事件注册到同一个epoll内核事件表*/
    static int m_epollfd;
    /*统计用户数量*/
    static int m_user_count;
private:
    /*该http连接的socket和对方的socket地址*/
    int m_sockfd;
    sockaddr_in m_address;
    /*读缓冲区*/
    char m_read_buf[READ_BUFFER_SIZE];
    /*标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置*/
    int m_read_idx;
    /*当前正在分析的字符在读缓冲区的位置*/
    int m_checked_idx;
    /*当前正在解析的行的起始位置*/
    int m_start_line;
    /*写缓冲区*/
    char m_write_buf[WRITE_BUFFER_SIZE];
    /*写缓冲区中待发送的字节数*/
    int m_write_idx; 
    
    /*主状态机当前的状态*/
    CHECK_STATE m_check_state;
    /*请求方法*/
    METHOD m_method;

    /*客户请求的目标文件的路径*/
    char m_real_file[FILENAME_LEN];
    /*客户请求的目标文件文件名*/
    char *m_url;
    /*HTTP协议的版本号,支持HTTP/1.1*/
    char *m_version;
    /*主机名*/
    char *m_host;
    /*HTTP请求的消息体的长度*/
    int m_content_length;
    /*HTTP请求是否要求保持连接*/
    bool m_linger;

    /*客户请求的目标文件被mmap到内存中的起始位置*/
    char *m_file_address;
    /*目标文件的状态，通过它我们可以判断文件是否存在，是否为目录，是否可读，并获取文件大小信息等*/
    struct stat m_file_stat;
    /*我们采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量*/
    struct iovec m_iv[2];
    int m_iv_count;
    
    int m_cgi; //判断是否为POST请求
};

#endif