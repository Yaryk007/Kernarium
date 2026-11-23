// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/event.h>
#include <thread>
#include <time.h>
#include <unistd.h>
// The backend's private header: the entry points and the ident of the idle
// queue's trigger.
extern "C" {
#include "irq.h"
}
extern "C" {
#include "irq.h"
int os_setup_epoll(); int os_add_epoll_fd(int,int,void*);
int os_del_epoll_fd(int); int os_waiting_for_events_epoll(); void os_close_epoll_fd();
void *os_epoll_get_data_pointer(int); int os_epoll_triggered(int,int);
int darwin_irq_set_notification(int,int); int os_irq_wake_pending();
void panic(const char*,...) { std::abort(); }
}
// The notification thread names itself first; every pass of its loop reads the
// clock, so the reads count its wake-ups.
static thread_local bool notify_thread=false;
static std::atomic<long> thread_loops{0};
extern "C" {
void os_set_thread_name(const char*) { notify_thread=true; }
long long os_nsecs(void) {
 if (notify_thread) thread_loops++;
 timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000LL+t.tv_nsec;
}
}
// What the dispatch calls, and the idle secondary the notification thread wakes.
// `shot' names a CPU that tries to dispatch once, from inside the next handler,
// and `shot_result' is what that attempt returned. A handler of the stress
// collects what has arrived, as the real one does until a kevent() returns
// nothing, and then lingers before it returns, as the real one does before the
// token is released.
// With `poll_events' the handler also polls as the real one does, until a poll
// returns nothing: `collected' counts the events it took, and a descriptor in
// `inject_fd' gets a byte after the first poll, so that the next pass has one.
static std::atomic<int> dispatches{0}, wakes{0}, shot{-1}, shot_result{-1};
static std::atomic<bool> stress{false}, poll_events{false};
static std::atomic<int> collected{0}, inject_fd{-1};
static std::atomic<long> arrived{0}, taken{0};
static std::atomic<int> linger{0};
extern "C" void sigio_handler(int,void*,void*,void*) {
 dispatches++;
 int cpu = shot.exchange(-1);
 // A CPU that dispatches while another does must not, whatever its time.
 if (cpu>=0) shot_result = darwin_irq_dispatch(cpu,1ULL<<62,nullptr);
 if (poll_events) {
  int count;
  while ((count = os_waiting_for_events_epoll()) > 0) collected += count;
  assert(count==0);
  int fd = inject_fd.exchange(-1);
  if (fd>=0) assert(write(fd,"i",1)==1);
 }
 if (stress) {
  for (long seen; taken < (seen = arrived.load()); ) taken = seen;
  for (volatile int i=0;i<linger;i=i+1) {}
 }
}
extern "C" int darwin_wake_idle_secondary(void) { return ++wakes==0 ? -1 : 1; }
static void sleep_ms(int ms) { timespec t{0,ms*1000000L}; nanosleep(&t,nullptr); }
static bool wait_for_wake(int count) {
 for (int i=0;i<5000 && wakes<count;i++) sleep_ms(1);
 return wakes>=count;
}
// CPU0's idle queue, as its carrier makes it: the trigger the notification
// thread posts to end the idle wait, registered before the first entry.
static int idle_queue=-1;
static int idle_begin() { return darwin_irq_idle_begin(idle_queue); }
// Whether the trigger is pending in the queue. A queue is readable while it
// holds an event, and polling it takes none.
static int trigger_pending() {
 pollfd queue{idle_queue,POLLIN,0};
 assert(poll(&queue,1,0)>=0);
 return (queue.revents&POLLIN)!=0;
}
// Wait for the trigger to be pending, then consume it, as a wait returns it. A
// later trigger, for an edge the thread read after this one, may be pending
// again as soon as this returns: the queue holds one, like a signal did.
static bool wait_for_trigger() {
 for (int i=0;i<5000 && !trigger_pending();i++) sleep_ms(1);
 if (!trigger_pending()) return false;
 struct kevent event; timespec zero{0,0};
 assert(kevent(idle_queue,nullptr,0,&event,1,&zero)==1);
 assert(event.filter==EVFILT_USER && event.ident==DARWIN_IRQ_IDLE_WAKE_IDENT);
 return true;
}
// Whether SIGUSR2 is blocked for this thread, as CPU0's first idle entry leaves it.
static int sigusr2_blocked() {
 sigset_t mask; sigemptyset(&mask); assert(pthread_sigmask(SIG_SETMASK,nullptr,&mask)==0);
 return sigismember(&mask,SIGUSR2);
}
// Dispatch as a running CPU0 does at its tick until the notification thread has
// nothing unserved, which it shows by taking no wake in a quiet spell.
static void settle() {
 for (int round=0;round<40;round++) {
  for (int tick=0;tick<20;tick++) {
   assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
   sleep_ms(5);
  }
  int before=wakes;
  sleep_ms(80);
  if (wakes==before) return;
 }
 assert(!"the notification thread settles");
}
int main() {
 idle_queue=kqueue(); assert(idle_queue>=0);
 struct kevent wake; EV_SET(&wake,DARWIN_IRQ_IDLE_WAKE_IDENT,EVFILT_USER,EV_ADD|EV_CLEAR,0,0,nullptr);
 assert(kevent(idle_queue,&wake,1,nullptr,0,nullptr)==0);
 int fds[2]; assert(pipe(fds)==0); int token=1, next=2;
 assert(os_add_epoll_fd(1,fds[0],&token)==0);
 assert(write(fds[1],"a",1)==1);
 assert(os_setup_epoll()==0);
 // A descriptor that notifies and has input is a wake source. A system
 // suspend clears the notification of the others, and setting it restores it.
 assert(os_irq_wake_pending()==1);
 assert(darwin_irq_set_notification(fds[0],0)==0 && os_irq_wake_pending()==0);
 assert(darwin_irq_set_notification(fds[0],1)==0 && os_irq_wake_pending()==1);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_epoll_get_data_pointer(0)==&token && os_epoll_triggered(0,1));
 // Reentry before the outer handler drains must not deliver the same level.
 assert(os_waiting_for_events_epoll()==0);
 // Adding a registered descriptor modifies it and rearms its edge.
 assert(os_add_epoll_fd(1,fds[0],&next)==0);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_epoll_get_data_pointer(0)==&next);
 assert(os_waiting_for_events_epoll()==0);
 char byte; assert(read(fds[0],&byte,1)==1);
 // Drain and refill between polls must still produce a fresh edge.
 assert(write(fds[1],"b",1)==1);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_waiting_for_events_epoll()==0);
 assert(read(fds[0],&byte,1)==1);
 close(fds[1]); assert(os_waiting_for_events_epoll()==1);
 assert(os_waiting_for_events_epoll()==0);
 assert(os_del_epoll_fd(fds[0])==0); close(fds[0]);
 // The dispatch token: CPU0 always dispatches; another CPU only once no
 // dispatch has begun for 20 ms; none while one runs; the stamp never goes back.
 constexpr unsigned long long ms=1000000ULL, t0=1000*ms;
 assert(darwin_irq_dispatch(0,t0,nullptr)==1 && dispatches==1);
 assert(darwin_irq_dispatch(1,t0+19*ms,nullptr)==0 && dispatches==1);
 assert(darwin_irq_dispatch(1,t0+20*ms,nullptr)==1 && dispatches==2);
 assert(darwin_irq_dispatch(1,t0+39*ms,nullptr)==0 && dispatches==2);
 assert(darwin_irq_dispatch(0,t0+5*ms,nullptr)==1 && dispatches==3);
 assert(darwin_irq_dispatch(1,t0+39*ms,nullptr)==0 && dispatches==3);
 // A CPU's own dispatch under way collects for it: a nested attempt is refused and
 // leaves no note, so the holder does not dispatch again.
 shot=0; assert(darwin_irq_dispatch(0,t0+50*ms,nullptr)==1 && shot_result==0 && dispatches==4);
 // CPU0 finding another CPU's dispatch under way leaves a note, and that CPU
 // dispatches once more after it releases the token; CPU0 did not wait.
 shot=0; shot_result=-1; assert(darwin_irq_dispatch(1,t0+80*ms,nullptr)==1 && shot_result==0 && dispatches==6);
 assert(darwin_irq_dispatch(0,t0+81*ms,nullptr)==1 && dispatches==7);
 // What a dispatch returns: DARWIN_IRQ_HANDLED when a poll of its handler
 // collected an event, in any pass of it, and 1 otherwise. The edge is taken
 // once; the descriptor stays readable and the next dispatch finds nothing.
 {
  int hp[2]; assert(pipe(hp)==0 && os_add_epoll_fd(1,hp[0],&token)==0);
  char byte; poll_events=true; collected=0;
  assert(darwin_irq_dispatch(0,t0+90*ms,nullptr)==1 && collected==0);
  assert(write(hp[1],"p",1)==1);
  assert(darwin_irq_dispatch(0,t0+91*ms,nullptr)==DARWIN_IRQ_HANDLED && collected==1);
  assert(darwin_irq_dispatch(0,t0+92*ms,nullptr)==1 && collected==1);
  assert(darwin_irq_dispatch(0,t0+93*ms,nullptr)==1 && collected==1);
  // The first pass collects an event and a retry, which another CPU's attempt
  // during the pass asks for, finds nothing: the result keeps the first's.
  assert(read(hp[0],&byte,1)==1 && write(hp[1],"q",1)==1);
  shot=0; shot_result=-1; collected=0; int before=dispatches;
  assert(darwin_irq_dispatch(1,t0+130*ms,nullptr)==DARWIN_IRQ_HANDLED && shot_result==0 && dispatches-before==2 && collected==1);
  assert(darwin_irq_dispatch(0,t0+131*ms,nullptr)==1 && collected==1);
  // The first pass finds nothing and the event comes during it: the retry's
  // poll collects it and the result is that pass's.
  assert(read(hp[0],&byte,1)==1);
  shot=0; shot_result=-1; collected=0; before=dispatches; inject_fd=hp[1];
  assert(darwin_irq_dispatch(1,t0+170*ms,nullptr)==DARWIN_IRQ_HANDLED && shot_result==0 && dispatches-before==2 && collected==1);
  assert(darwin_irq_dispatch(0,t0+171*ms,nullptr)==1 && collected==1);
  poll_events=false;
  assert(os_del_epoll_fd(hp[0])==0); close(hp[0]); close(hp[1]);
 }
 // An edge nobody dispatches after is handed to an idle secondary after 20 ms,
 // again after each interval, and no more once the dispatches go on, as a
 // running CPU0's do at its tick.
 settle();
 int edge[2]; assert(pipe(edge)==0 && os_add_epoll_fd(1,edge[0],&token)==0);
 int base=wakes;
 assert(write(edge[1],"c",1)==1);
 assert(wait_for_wake(base+1) && wait_for_wake(base+2));
 for (int tick=0;tick<20;tick++) {
  assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
  sleep_ms(5);
 }
 int settled=wakes;
 for (int tick=0;tick<20;tick++) {
  assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
  sleep_ms(5);
 }
 assert(wakes==settled);
 // CPU0's idle wait, in both orders of an edge and the wait. The wait ends on
 // a trigger of CPU0's queue; no signal does, and SIGUSR2 stays blocked on the
 // carrier from its first idle entry on.
 char drained[64];
 // Pending before the wait: the edge is there when CPU0 enters it, which then
 // must not sleep. The 20 ms takeover wake shows the thread has seen the edge.
 int idle[2]; assert(pipe(idle)==0 && os_add_epoll_fd(1,idle[0],&next)==0);
 settle();
 int before=wakes; assert(write(idle[1],"d",1)==1 && wait_for_wake(before+1));
 assert(trigger_pending()==0 && !sigusr2_blocked());
 assert(idle_begin()==1 && trigger_pending()==0 && sigusr2_blocked());
 darwin_irq_idle_end(); assert(idle_begin()==0);
 // Valid before the edge: the thread triggers, once, and nothing is left for the
 // next entry.
 assert(write(idle[1],"e",1)==1 && wait_for_trigger());
 sleep_ms(50); assert(!trigger_pending());
 darwin_irq_idle_end(); assert(idle_begin()==0 && !trigger_pending());
 // An edge after CPU0 left the wait is left for its next entry and triggers
 // nothing.
 darwin_irq_idle_end();
 assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
 assert(write(idle[1],"f",1)==1);
 sleep_ms(50); assert(!trigger_pending());
 assert(idle_begin()==1);
 darwin_irq_idle_end();
 assert(read(idle[0],drained,sizeof drained)==3);
 // While CPU0 is not idle the thread does not wake for every edge: it parks
 // after the first and the rest coalesce until its deadline, which a CPU0 that
 // dispatches at its tick meets. A thread that woke for each edge would loop
 // twice per edge, 200 times here.
 darwin_irq_idle_end(); settle();
 {
  long loops=thread_loops;
  for (int edge_count=0;edge_count<100;edge_count++) {
   assert(write(idle[1],"h",1)==1);
   if (edge_count%4==0) assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
   sleep_ms(1);
  }
  assert(thread_loops-loops<100);
  assert(read(idle[0],drained,sizeof drained)>=1);
 }
 // CPU0 enters its idle wait while the thread is parked on an edge: the entry
 // ends the park, so the next edge triggers at once and not at the takeover
 // deadline, which would have woken an idle secondary first. Host scheduling
 // may delay the thread, so a tenth of the rounds may be late.
 {
  const int rounds=100; int late=0;
  for (int r=0;r<rounds;r++) {
   darwin_irq_idle_end();
   while (trigger_pending()) wait_for_trigger();
   assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
   assert(write(idle[1],"i",1)==1);
   sleep_ms(2);
   if (idle_begin()!=1) assert(wait_for_trigger());
   while (trigger_pending()) wait_for_trigger();
   int before_edge=wakes;
   assert(write(idle[1],"j",1)==1 && wait_for_trigger());
   if (wakes!=before_edge) late++;
   darwin_irq_idle_end();
   assert(read(idle[0],drained,sizeof drained)>=2);
  }
  assert(late<=rounds/10);
 }
 // Whatever the order, the wake is not lost: each round starts an edge and a
 // wait at about the same time, and one of the two must see the other.
 {
  std::atomic<int> round{0};
  const int rounds=1000;
  std::thread producer([&] {
   for (int r=1;r<=rounds;r++) {
    while (round.load()<r) std::this_thread::yield();
    (void)!write(idle[1],"g",1);
   }
  });
  for (int r=1;r<=rounds;r++) {
   darwin_irq_idle_end();
   assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
   round=r;
   // From before the edge to well after the thread can have seen it.
   for (unsigned long long until=(unsigned long long)os_nsecs()+(unsigned long long)(r*7919%150)*1000; (unsigned long long)os_nsecs()<until;) {}
   // CPU0 enters the wait now, or after the edge, or leaves it before the thread looks.
   bool woken=false;
   for (int spin=0;spin<5000 && !woken;spin++) {
    woken = idle_begin()==1 || trigger_pending();
    if (!woken) { if (r%3==0) darwin_irq_idle_end(); sleep_ms(1); }
   }
   assert(woken);
   while (trigger_pending()) wait_for_trigger();
   darwin_irq_idle_end();
   assert(read(idle[0],drained,sizeof drained)>=1);
  }
  producer.join();
  darwin_irq_idle_end();
 }
 // Stopping the thread while it is parked, and starting it again.
 darwin_irq_idle_end(); settle();
 assert(write(idle[1],"k",1)==1);
 sleep_ms(5);
 os_close_epoll_fd();
 // CPU0 may still idle once after the thread stopped; the queue is gone.
 (void)idle_begin(); darwin_irq_idle_end();
 assert(os_setup_epoll()==0);
 assert(os_add_epoll_fd(1,idle[0],&next)==0);
 assert(read(idle[0],drained,sizeof drained)>=1);
 assert(write(idle[1],"l",1)==1);
 sleep_ms(30);
 assert(os_del_epoll_fd(idle[0])==0);
 // The token handshake: a CPU0 that finds another CPU's dispatch under way, and
 // an edge that arrives after that holder collected, must not strand the edge.
 // Every round an edge arrives at about the time the holder is lingering with
 // the token, and CPU0 tries then; the edge is collected by the end of it.
 {
  stress=true; linger=1500;
  std::atomic<int> go{0}, done{0};
  const long rounds=3000;
  std::thread holder([&] {
   for (long r=1;r<=rounds;r++) {
    while (go.load()<r) std::this_thread::yield();
    darwin_irq_dispatch(1,(unsigned long long)os_nsecs()+(unsigned long long)r*100*ms,nullptr);
    done++;
   }
  });
  for (long r=1;r<=rounds;r++) {
   go=(int)r;
   for (volatile int i=0;i<(r*29)%3000;i=i+1) {}
   arrived++;
   darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr);
   while (done.load()<r) std::this_thread::yield();
   assert(taken==arrived);
  }
  holder.join(); stress=false;
 }
 os_close_epoll_fd(); puts("PASS IRQ reentry, MOD rearm, wake source, drain/refill, EOF, early registration, dispatch token and its result, takeover wake, idle wait wake, parked thread, token handshake, shutdown");
}
