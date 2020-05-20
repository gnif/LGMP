#include <lgmp/host.h>
#include <lgmp/client.h>
#include <relacy/relacy.hpp>

class CLGMPQueueTest
{
public:
  bool initHost(void * mem, const size_t memSize)
  {
    LGMP_STATUS status;

    uint8_t data[32];
    memset(data, 0xaa, sizeof(data));
    if ((status = lgmpHostInit(mem, memSize, &m_host, sizeof(data), data))
        != LGMP_OK)
    {
      printf("lgmpHostInit: %s\n", lgmpStatusString(status));
      return false;
    }

    const struct LGMPQueueConfig conf =
    {
      .queueID     = 0,
      .numMessages = 10,
      .subTimeout  = 10000
    };

    if ((status = lgmpHostQueueNew(m_host, conf, &m_hqueue)) != LGMP_OK)
    {
      printf("lgmpHostQueueNew: %s\n", lgmpStatusString(status));
      return false;
    }

    if ((status = lgmpHostMemAlloc(m_host, 1024, &m_mem)) != LGMP_OK)
    {
      printf("lgmpHostMemAlloc: %s\n", lgmpStatusString(status));
      return false;
    }

    return true;
  }

  bool initClient(void * mem, const size_t memSize)
  {
    LGMP_STATUS status;

    uint32_t dataSize;
    uint8_t * data;

    if((status = lgmpClientInit(mem, memSize, &m_client))
        != LGMP_OK)
    {
      printf("lgmpClientInit: %s\n", lgmpStatusString(status));
      return false;
    }

    if((status = lgmpClientSessionInit(m_client, &dataSize, &data))
        != LGMP_OK)
    {
      printf("lgmpClientSessionInit: %s\n", lgmpStatusString(status));
      return false;
    }

    if ((status = lgmpClientSubscribe(m_client, 0, &m_cqueue)) != LGMP_OK)
    {
      printf("lgmpClientSubscribe: %s\n", lgmpStatusString(status));
      return false;
    }

    return true;
  }

  void fini()
  {
    lgmpClientUnsubscribe(&m_cqueue);
    lgmpClientFree       (&m_client);
    lgmpHostFree         (&m_host  );
  }

  bool hostProcess()
  {
    LGMP_STATUS status;
    if ((status = lgmpHostProcess(m_host)) != LGMP_OK)
    {
      printf("lgmpHostProcess: %s\n", lgmpStatusString(status));
      return false;
    }
    return true;
  }

  LGMP_STATUS enqueue()
  {
    return lgmpHostQueuePost(m_hqueue, 0, m_mem);
  }

  LGMP_STATUS peek()
  {
    LGMPMessage msg;
    return lgmpClientProcess(m_cqueue, &msg);
  }

  LGMP_STATUS dequeue()
  {
    return lgmpClientMessageDone(m_cqueue);
  }

private:
  PLGMPHost    m_host   = NULL;
  PLGMPHostQueue  m_hqueue = NULL;
  PLGMPMemory  m_mem    = NULL;

  PLGMPClient  m_client = NULL;
  PLGMPClientQueue  m_cqueue = NULL;
};

struct QueueTest : rl::test_suite<QueueTest, 60>
{
  CLGMPQueueTest q;
  const size_t memSize = 10 * 1024 * 1024;
  void * mem;
  int pending = 0;

  void before()
  {
    RL_ASSERT((mem = malloc(memSize)) != NULL);
    RL_ASSERT(q.initHost  (mem, memSize));
    RL_ASSERT(q.initClient(mem, memSize));
  }

  void after()
  {
    q.fini();
    free(mem);
  }

  void thread(unsigned thread_index)
  {
    if (thread_index < 30)
    {
      RL_ASSERT(q.hostProcess());
      LGMP_STATUS status = q.enqueue();

      if (pending == 10)
        RL_ASSERT(status == LGMP_ERR_QUEUE_FULL);
      else
      {
        RL_ASSERT(status == LGMP_OK);
        ++pending;
      }
    }
    else
    {
      LGMP_STATUS status = q.peek();
      if (pending == 0)
      {
        RL_ASSERT(status      == LGMP_ERR_QUEUE_EMPTY);
        RL_ASSERT(q.dequeue() == LGMP_ERR_QUEUE_EMPTY);
      }
      else
      {
        RL_ASSERT(status      == LGMP_OK);
        RL_ASSERT(q.dequeue() == LGMP_OK);
        --pending;
      }
    }
  }
};

int main()
{
  rl::simulate<QueueTest>();
}
