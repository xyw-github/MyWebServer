#ifndef LST_TIMER
#define LST_TIMER

#include"common.h"
class util_timer;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class util_timer
{
public:
    util_timer() : prev(nullptr),next(nullptr) {}

public:
    time_t expire;
    void (*cb_func)(client_data *);
    client_data *user_data;
    util_timer *prev;
    util_timer *next;
};

class sort_timer_list
{
public:
    sort_timer_list() : head(nullptr),tail(nullptr) {}
    ~sort_timer_list()
    {
        util_timer *tmp = head;
        while(tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    void add_timer(util_timer *timer)
    {
        if(!timer)
        {
            return ;
        }
        if(!head)
        {
            head = tail = timer;
            return;
        }
        if(timer->expire < head->expire)
        {
            head->prev = timer;
            timer->next = head;
            head = timer;
            return;
        }
        add_timer(timer,head);
    }

    void adjust_timer(util_timer *timer)
    {
        if(timer == nullptr)
        {
            return;
        }
        util_timer *tmp = timer->next;
        if((tmp == nullptr) || (timer->expire < tmp->expire))
        {
            return;
        }
        if(timer == head)
        {
            head = head->next;
            head->prev = nullptr;
            timer->next = nullptr;
            add_timer(timer,head);
        }
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer,timer->next);
        }
    }

    void del_timer(util_timer*timer)
    {
        if(timer == nullptr)
        {
            return;
        }
        if((timer == head) && (timer == tail))
        {
            delete timer;
            head = nullptr;
            tail = nullptr;
            return;
        }
        if(timer == head)
        {
            head = head->next;
            head->prev = nullptr;
            delete timer;
            return;
        }
        if(head == tail)
        {
            tail = tail->prev;
            tail->next = nullptr;
            delete timer;
            return;
        }
        timer->next->prev = timer->prev;
        timer->prev->next = timer->next;
        delete timer;
    }
    void tick()
    {
        if(head == nullptr)
        {
            return;
        }
        printf("timer tick\n");
        time_t cur = time(NULL);
        util_timer *tmp = head;
        while(tmp)
        {
            if(cur < tmp->expire)
            {
                break;
            }
            tmp->cb_func(tmp->user_data);
            head = tmp->next;
            if(head)
            {
                head->prev = nullptr;
            }
            delete tmp;
            tmp = head;
        }
    }
private:
    void add_timer(util_timer *timer,util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        while(tmp)
        {
            if(tmp->expire > timer->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        if(timer == nullptr)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = nullptr;
            tail = timer;
        }
    }

    util_timer *head;
    util_timer *tail;
};


#endif