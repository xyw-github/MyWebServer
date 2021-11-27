#include"http_conn.h"
#include"common.h"
#include"threadpool.h"
#include"locker.h"
//#include"lst_timer.h"

#define MAX_FD 65536            //最大文件描述符
#define MAX_EVENT_NUMBER 10000  //最大事件数
#define TIMESLOT 5              //最小超时单位

// 这三个函数在http_conn.ccp中
extern void addfd(int epollfd,int fd,bool one_shot);
extern void removefd(int epollfd,int fd);
extern int setnonblocking(int fd);

// 设计定时器相关参数
static int pipefd[2];
//static sort_timer_list timer_list;
static int epollfd;

// 信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，需要保存errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1],(char *)&msg,1,0);
    errno = save_errno;
}

// 设置信号函数
void addsig(int sig,void(handler)(int),bool restart = true)
{
    struct sigaction sa;
    memset((void *)&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL) != -1);
}

// //定时处理函数，重新定义时不断触发
// void timer_handler()
// {
//     timer_list.tick();
//     alarm(TIMESLOT);
// }

// //定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
// void cb_func(client_data *user_data)
// {
//     epoll_ctl(epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
//     assert(user_data);
//     close(user_data->sockfd);
//     http_conn::m_user_count--;
//     printf("close fd %d\n",user_data->sockfd);
// }

void show_error(int connfd,const char *info)
{
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main(int argc,char **argv)
{
    if(argc != 2)
    {
        printf("Usage: %s port_number\n",basename(argv[0]));
        exit(1);
    }
    //const char *ip = argv[1];
    int port = atoi(argv[1]);
    //signal(SIGINT,)

    /*忽略SIGPIPE信号*/
    addsig(SIGPIPE,SIG_IGN);
    /*创建线程池*/
    threadpool<http_conn> *pool = new threadpool<http_conn>;
    /*预先为每个可能的客户连接分配http_conn对象*/
    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int listenfd = socket(AF_INET,SOCK_STREAM,0);
    assert(listenfd >= 0);

    int ret = bind(listenfd,(struct sockaddr *)&address,sizeof(address));
    assert(ret >= 0);
    
    ret = listen(listenfd,5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd = epollfd;

    ret = socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0],false);

    addsig(SIGALRM,sig_handler,false);
    addsig(SIGTERM,sig_handler,false);
    addsig(SIGINT,sig_handler,false);
    bool stop_server = false;

    //client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while(!stop_server)
    {
        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if(number < 0 && errno != EINTR)
        {
            printf("epoll failure");
            break;
        }
        for(int i = 0;i < number;i++)
        {
            int sockfd = events[i].data.fd;
            
            //处理新的客户连接
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                if(connfd < 0)
                {
                    printf("errno is: %d\n",errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd,"Internal server busy");
                    continue;
                }
                /*初始化客户连接*/
                users[connfd].init(connfd,client_address);

                // //初始化client_data数据
                // //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                // users_timer[connfd].address = client_address;
                // users_timer[connfd].sockfd = connfd;
                // util_timer *timer = new util_timer;
                // timer->cb_func = cb_func;
                // time_t cur = time(NULL);
                // timer->expire = cur + 3 * TIMESLOT;
                // users_timer[connfd].timer = timer;
                // timer_list.add_timer(timer);
            }
            /*如果有异常，直接关闭客户端连接*/
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // //服务端关闭连接，移除对应的定时器
                // util_timer *timer = users_timer[sockfd].timer;
                // timer->cb_func(&users_timer[sockfd]);

                // if(timer)
                // {
                //     timer_list.del_timer(timer);
                // }
            }

            //处理信号
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                memset(signals,'\0',sizeof(signals));
                ret = recv(pipefd[0],signals,sizeof(signals) - 1,0);
                if(ret == -1)
                {
                    continue;
                }
                else if(ret == 0)
                {
                    continue;
                }
                else
                {
                    for(int i = 0; i < ret ;i ++)
                    {
                        switch(signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGINT:
                            {
                                stop_server = true;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //处理客户连接上收到的数据
            else if(events[i].events & EPOLLIN)
            {
                // util_timer *timer = users_timer[sockfd].timer;
                /*根据读数据的结果，决定是讲任务添加到线程池，还是关闭连接*/
                if(users[sockfd].read())
                {
                    //监测到有读事件，则将事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输,则将定时器往后延迟三个单位
                    //并对新定时器在链表上的位置进行调整
                    // if(timer)
                    // {
                    //     time_t cur = time(NULL);
                    //     timer->expire = cur + 3 * TIMESLOT;
                    //     printf("adjust timer once\n");
                    //     timer_list.adjust_timer(timer);
                    // }
                }
                else
                {
                    // timer->cb_func(&users_timer[sockfd]);
                    // if(timer)
                    // {
                    //     timer_list.del_timer(timer);
                    // }
                }
            }
            // 返回给客户端数据
            else if(events[i].events & EPOLLOUT)
            {
                // util_timer *timer = users_timer[sockfd].timer;
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    // if (timer)
                    // {
                    //     time_t cur = time(NULL);
                    //     timer->expire = cur + 3 * TIMESLOT;
                        
                    //     timer_list.adjust_timer(timer);
                    // }
                }
                else
                {
                    // timer->cb_func(&users_timer[sockfd]);
                    // if (timer)
                    // {
                    //     timer_list.del_timer(timer);
                    // }
                }
            }
        }

        // if(timeout)
        // {
        //     timer_handler();
        //     timeout = false;
        // }

    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete []users;
    //delete []users_timer;
    delete pool;
    return 0;
}