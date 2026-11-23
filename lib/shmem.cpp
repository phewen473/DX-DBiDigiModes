#include <QSharedMemory>
#include <QLatin1String>
#include <QThread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Multiple instances: KK1D, 17 Jul 2013
QSharedMemory shmem;

struct jt9com;

// C wrappers for a QSharedMemory class instance
extern "C"
{
  bool shmem_create (int nsize) {return shmem.create(nsize);}
  void shmem_setkey (char * const mykey) {shmem.setKey(QLatin1String{mykey});}
  bool shmem_attach () {return shmem.attach();}
  int shmem_size () {return static_cast<int> (shmem.size());}
  struct jt9com * shmem_address () {return reinterpret_cast<struct jt9com *>(shmem.data());}
  bool shmem_lock () {return shmem.lock();}
  bool shmem_unlock () {return shmem.unlock();}
  bool shmem_detach () {return shmem.detach();}

  // DX-DBi: wait up to ms milliseconds for the GUI to signal new work
  // (named event "Local\\WSJTX_jt9wake_<key>", set by MainWindow::to_jt9).
  // jt9 used to sleep 10 ms between polls of the shared memory; with the
  // event it wakes at once and polls only every ms as a fallback.
  void shmem_wait (int ms)
  {
#ifdef _WIN32
    static HANDLE event {nullptr};
    if (!event)
      {
        auto const name = QString {"Local\\WSJTX_jt9wake_%1"}.arg (shmem.key ());
        event = CreateEventW (nullptr, FALSE, FALSE, reinterpret_cast<wchar_t const *> (name.utf16 ()));
      }
    if (event)
      {
        WaitForSingleObject (event, static_cast<DWORD> (ms));
        return;
      }
#endif
    QThread::msleep (ms > 10 ? 10 : ms);
  }
}
