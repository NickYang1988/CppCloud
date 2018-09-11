#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/epoll.h>
#include "comm/sock.h"
#include "switchhand.h"


std::thread* SwitchHand::s_thread = NULL;

SwitchHand::SwitchHand(void)
{
    m_pipe[0] = INVALID_FD;
    m_pipe[1] = INVALID_FD;
    bexit = false;
}

SwitchHand::~SwitchHand(void)
{
    IFCLOSEFD(m_pipe[0]);
    IFCLOSEFD(m_pipe[1]);
    IFDELETE(s_thread);
}

void SwitchHand::init( int epFd )
{
    int ret = pipe(m_pipe);
    if (0 == ret && INVALID_FD != epFd)
    {
        m_epCtrl.setEPfd(epFd);
        m_epCtrl.setActFd(m_pipe[0]); // read pipe

        if (s_thread == NULL)
        {
            s_thread = new std::thread(TimeWaitThreadFunc, this);
        }
    }
    else
    {
        LOGERROR("SWITCHINIT| msg=init swichhand fail| epFd=%d| ret=%d", epFd, ret);
    }
}

void SwitchHand::join( void )
{
    if (s_thread)
    {
        s_thread->join();
    }
}

int SwitchHand::setActive( char fg )
{
    int ret;
    ret = write(m_pipe[1], &fg, 1);
    ERRLOG_IF1(ret<0, "SWITCHSETACT| msg=write fail %d| err=%s", ret, strerror(errno));
    ret = m_epCtrl.setEvt(EPOLLIN, this);
    ERRLOG_IF1(ret, "SWITCHSETACT| msg=m_epCtrl.setEvt fail %d| err=%s", ret, strerror(errno));
    return ret;
}

// thread-safe
int SwitchHand::appendQTask( ITaskRun2* tsk, int delay_ms )
{
    return tskwaitq.append_delay(tsk, delay_ms);
}

// 此方法运行于io-epoll线程
int SwitchHand::run( int flag, long p2 )
{
    if (EPOLLIN & flag) // 有数据可读
    {
        char buff[32] = {0};
        unsigned beg = 0;
        int ret = Sock::recv(m_pipe[0], buff, beg, 31);
        char prech=' ';
        for (unsigned i = 0; i < beg; ++i)
        {
            if (prech == buff[i]) continue;
            prech = buff[i];
            switch (prech)
            {
                case 'q': // 执行队列任务(注意qrun里面不要阻塞)
                {
                    ITaskRun2* item = NULL;
                    while ( tskioq.pop(item, 0) )
                    {
                        item->qrun(0, 0);
                    }
                }
                break;
                default:
                    LOGERROR("SWITCHRUN| msg=unexcept pipe msg| buff=%s", buff);
                break;
            }
        }
    }
    else
    {
        if (HEFG_PEXIT == flag)
        {
            notifyExit();
        }
        else
        {
            LOGERROR("SWITCHRUN| msg=maybe pipe fail| err=%s| param=%d-%d", strerror(errno), flag, p2);
            m_epCtrl.setEvt(0, 0);
            IFCLOSEFD(m_pipe[0]);
            IFCLOSEFD(m_pipe[1]);
            pipe(m_pipe);
        }
    }
}

int SwitchHand::qrun( int flag, long p2 )
{
    LOGERROR("SWITCHRUN| msg=undefine flow qrun");
    return -1;
}

int SwitchHand::onEvent( int evtype, va_list ap )
{
    return 0;
}

void SwitchHand::notifyExit( void )
{
    bexit = true;
    tskwaitq.wakeup();
    tskioq.wakeup();
}

void SwitchHand::TimeWaitThreadFunc( SwitchHand* This )
{
    ITaskRun2* item = NULL;
    
    while ( !This->bexit ) 
    {
        if (This->tskwaitq.pop_delay(item))
        {
            bool bret = This->tskioq.append(item);
            if (bret)
            {
                LOGERROR("SwitchHand| msg=append task fail| qsize=%d", This->tskioq.size());
                This->tskwaitq.append_delay(item, 5000);
            }
            else
            {
                This->setActive('q');
            }

            item = NULL;
        }
    }
}