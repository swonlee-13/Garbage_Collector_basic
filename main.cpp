#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;

// 시간을 기록해놓기 위해서 주소, 사이즈, 생성시간을 들고 저장.
struct MemoryBlock
{
  void *address;
  size_t size;
  chrono::system_clock::time_point allocationTime;
  bool isFreed = false; // 해제 상태 추적
};

class GarbageCollector
{
private:
  // 싱글톤 객체
  static GarbageCollector *instance;
  // 모니터링 스레드와, 메인 스레드간의 교착 상태를 막기위함.
  static recursive_mutex mutex;
  // 모니터링 도는 주기
  const int period;
  // 쓰레드선언
  thread th;
  // 메모리블록을 벡터로 만들어서 new연산할 때마다 그 주소를 이렇게 포장해서
  // 저장.
  vector<MemoryBlock> memoryBlocks;
  // 싱글톤을 위한 변수
  bool running = true;

  // 생성자가 발동되면서, period 는 900ms으로 잡고 시작하고,
  // 가비지 콜렉터 모니터링 스레드를 돌립니다.
  GarbageCollector() : period(900)
  {
    th = thread(&GarbageCollector::timer, this, period);
  }
  // 이게 모니터링 쓰레드.
  void timer(int period)
  {
    while (running)
    {
      this_thread::sleep_for(chrono::milliseconds(period));
      collectGarbage();
    }
  }

  // 메모리블록을 벡터에 추가하는 함수.
  void addMemoryBlock(void *address, size_t size, bool isReallocation,
                      void *prevAddr = nullptr)
  {
    if (!address)
      return;
    lock_guard<recursive_mutex> lock(mutex);
    MemoryBlock block{address, size, chrono::system_clock::now()};
    memoryBlocks.push_back(block);
  }

  // 시간을 확인하면서 1초 이상이면, 할당해제.
  void collectGarbage()
  {
    lock_guard<recursive_mutex> lock(mutex);
    auto now = chrono::system_clock::now();
    vector<MemoryBlock> newBlocks;
    bool collected = false;

    for (const auto &block : memoryBlocks)
    {
      if (block.isFreed)
        continue; // 이미 해제된 블록은 스킵
      auto duration = chrono::duration_cast<chrono::seconds>(now - block.allocationTime).count();

      if (duration >= 5)
      {
        collected = true;
        free(block.address);
      }
      else
      {
        newBlocks.push_back(block);
      }
    }

    memoryBlocks = std::move(newBlocks);
    printAddressList();
  }

public:
  // 싱글톤은 이렇게 콜 해서 사용합니다.
  static GarbageCollector &GetInstance()
  {
    lock_guard<recursive_mutex> lock(mutex);
    if (instance == nullptr)
    {
      instance = new GarbageCollector();
    }
    return *instance;
  }

  // 현재 할당여부를 확인. 의도하지 않은 동작 방어
  bool checkExistingAllocation(void *address)
  {
    lock_guard<recursive_mutex> lock(mutex);
    for (const auto &block : memoryBlocks)
    {
      if (block.address == address)
        return true;
    }
    return false;
  }

  // 오브젝트를 생성할 때 뮤텍스를 걸어주고 해야해서 한번 감싸준 함수.
  void createObject(void *address, size_t size)
  {
    lock_guard<recursive_mutex> lock(mutex);
    addMemoryBlock(address, size, false, nullptr);
  }

  // 직접 가비지 콜렉터 호출할 떄 씁니다.
  // 해당 함수는 기능상 private 에 놨기 때문에 이렇게 퍼블릭 래핑 함수가 필요.
  //  출력문은 지우고 싶으면 지우세요.
  void manualGarbage()
  {
    cout << "manual GC" << endl;
    GarbageCollector::GetInstance().collectGarbage();
  }

  // 출력함수입니다. 원래 함수 안에서 막 출력했는데, 불편해서 기능을
  // 분리했습니다.
  void printAddressList()
  {
    cout << "garage collected." << endl;
    cout << "[";
    for (const auto &block : memoryBlocks)
    {
      cout << block.address << " ";
    }
    cout << "]" << endl;
    cout << "Active Object : " << memoryBlocks.size() << endl;
  }

  // 소명자입니다. 소멸할 때 쓰레드 회수는 해야하고
  // 메모리블록을 순회하면서 다 지우고 소멸하게 했습니다.
  ~GarbageCollector()
  {
    running = false;
    if (th.joinable())
    {
      th.join();
    }

    for (const auto &block : memoryBlocks)
    {
      if (block.address != nullptr)
      {
        try
        {
          free(block.address);
        }
        catch (...)
        {
          cout << "Failed to free memory at " << block.address << endl;
        }
      }
    }
    memoryBlocks.clear();
  }

  // 다른 생성자는 막아줬습니다. 싱글톤 기본 패턴. 프린트에도 막혀있던 기능.
  GarbageCollector(const GarbageCollector &) = delete;
  GarbageCollector &operator=(const GarbageCollector &) = delete;
  GarbageCollector(GarbageCollector &&) = delete;
  GarbageCollector &operator=(GarbageCollector &&) = delete;
};

// 전역으로 선언해줍니다.
GarbageCollector *GarbageCollector::instance = nullptr;
recursive_mutex GarbageCollector::mutex;

struct member
{
  int x;
  int y;
};

template <typename T>
class smartptr
{
private:
  T *ptr;

public:
  smartptr(T *p = nullptr) : ptr(p) {}

  // 이거도 가비지콜렉터에게 기능을 위임하므로 내부구동은 빼야합니다. 안그러면
  // 더블Free
  ~smartptr() {}

  // 가비지콜렉터가 지워줄꺼라 재할당시 delete 코드는 뺐습니다.
  smartptr &operator=(T *p)
  {
    ptr = p;
    return *this;
  }
  T &operator*() const { return *ptr; }
  T *operator->() const { return ptr; }

  smartptr(const smartptr &) = delete;
  smartptr &operator=(const smartptr &) = delete;

  smartptr(smartptr &&other) noexcept
  {
    ptr = other.ptr;
    other.ptr = nullptr;
  }

  smartptr &operator=(smartptr &&other) noexcept
  {
    if (this != &other)
    {
      delete ptr;
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }
};

// new 연산자를 오버로딩합니다.
// 여기서 중요한 점은 가비지 콜렉터를 초기화할 때도 new를 사용하는데,
//  이걸 원래처럼 할당만 하고 넘어가게 해야합니다.
//  그렇지 않으면 그냥 무한루프에 걸려요. isRecursive 를 주석걸고 해보시면
//  압니다. 그래서 처음에 1회 발동하는 new 는 그냥 할당만, 그 다음부터는 GC에
//  넣는 추가 기능이 있는 new 로 동작하게 만들었습니다.
void *operator new(size_t size)
{
  static thread_local bool isRecursive = false;
  if (isRecursive)
    return malloc(size);

  isRecursive = true;
  void *ptr = malloc(size);
  if (!ptr)
    throw bad_alloc();

  auto &gc = GarbageCollector::GetInstance();
  // 호출 위치의 타입 정보 전달
  gc.createObject(ptr, size);

  isRecursive = false;
  return ptr;
}

int main()
{
  cout << "action start." << endl;

  int *ptr;
  ptr = new int;
  ptr = new int;
  ptr = new int;
  ptr = new int;
  ptr = new int;
  ptr = new int;
  ptr = new int;

  member *qtr;
  qtr = new member;
  qtr = new member;
  qtr = new member;
  qtr = new member;
  qtr = new member;
  qtr = new member;
  qtr = new member;

  // For SmartPjointers
  smartptr<member> rtr;
  rtr = new member;
  rtr = new member;
  rtr = new member;
  rtr = new member;
  rtr = new member;
  rtr = new member;

  // 메뉴얼갈비지
  GarbageCollector::GetInstance().manualGarbage();

  while (1)
  {
    ptr = new int;
    qtr = new member;
    rtr = new member;
    this_thread::sleep_for(chrono::milliseconds(100));
  }

  return 0;
}
