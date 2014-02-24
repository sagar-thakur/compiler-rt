// RUN: %clangxx_tsan %s -o %t
// RUN: TSAN_OPTIONS=detect_deadlocks=1 not %t 2>&1 | FileCheck %s
#include <pthread.h>
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

class PaddedLock {
 public:
  PaddedLock() { assert(0 == pthread_mutex_init(&mu_, 0)); }
  ~PaddedLock() {
    assert(0 == pthread_mutex_destroy(&mu_));
    (void)padding_;
  }
  void lock() { assert(0 == pthread_mutex_lock(&mu_)); }
  void unlock() { assert(0 == pthread_mutex_unlock(&mu_)); }

 private:
  pthread_mutex_t mu_;
  char padding_[64 - sizeof(pthread_mutex_t)];
};

class LockTest {
 public:
  LockTest(size_t n) : n_(n), locks_(new PaddedLock[n]) { }
  ~LockTest() { delete [] locks_; }
  void L(size_t i) {
    assert(i < n_);
    locks_[i].lock();
  }
  void U(size_t i) {
    assert(i < n_);
    locks_[i].unlock();
  }

  void *A(size_t i) {
    assert(i < n_);
    return &locks_[i];
  }

  // Simple lock order onversion.
  void Test1() {
    fprintf(stderr, "Starting Test1\n");
    // CHECK: Starting Test1
    fprintf(stderr, "Expecting lock inversion: %p %p\n", A(0), A(1));
    // CHECK: Expecting lock inversion: [[A1:0x[a-f0-9]*]] [[A2:0x[a-f0-9]*]]
    Lock_0_1();
    Lock_1_0();
    // CHECK: WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
    // CHECK: path: [[M1:M[0-9]+]] => [[M2:M[0-9]+]] => [[M1]]
    // CHECK: Mutex [[M1]] ([[A1]]) created at:
    // CHECK: Mutex [[M2]] ([[A2]]) created at:
    // CHECK-NOT: WARNING: ThreadSanitizer:
  }

  // Simple lock order inversion with 3 locks.
  void Test2() {
    fprintf(stderr, "Starting Test2\n");
    // CHECK: Starting Test2
    fprintf(stderr, "Expecting lock inversion: %p %p %p\n", A(0), A(1), A(2));
    // CHECK: Expecting lock inversion: [[A1:0x[a-f0-9]*]] [[A2:0x[a-f0-9]*]] [[A3:0x[a-f0-9]*]]
    Lock2(0, 1);
    Lock2(1, 2);
    Lock2(2, 0);
    // CHECK: WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
    // CHECK: path: [[M1:M[0-9]+]] => [[M2:M[0-9]+]] => [[M3:M[0-9]+]] => [[M1]]
    // CHECK: Mutex [[M1]] ([[A1]]) created at:
    // CHECK: Mutex [[M2]] ([[A2]]) created at:
    // CHECK: Mutex [[M3]] ([[A3]]) created at:
    // CHECK-NOT: WARNING: ThreadSanitizer:
  }

  // Lock order inversion with lots of new locks created (but not used)
  // between. Since the new locks are not used we should still detect the
  // deadlock.
  void Test3() {
    fprintf(stderr, "Starting Test3\n");
    // CHECK: Starting Test3
    Lock_0_1();
    L(2);
    CreateAndDestroyManyLocks();
    U(2);
    Lock_1_0();
    // CHECK: WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
    // CHECK-NOT: WARNING: ThreadSanitizer:
  }

  // lock l0=>l1; then create and use lots of locks; then lock l1=>l0.
  // The deadlock epoch should have changed and we should not report anything.
  void Test4() {
    fprintf(stderr, "Starting Test4\n");
    // CHECK: Starting Test4
    Lock_0_1();
    L(2);
    CreateLockUnlockAndDestroyManyLocks();
    U(2);
    Lock_1_0();
    // CHECK-NOT: WARNING: ThreadSanitizer:
  }

  void Test5() {
    fprintf(stderr, "Starting Test5\n");
    // CHECK: Starting Test5
    RunThreads(&LockTest::Lock_0_1, &LockTest::Lock_1_0);
    // CHECK: WARNING: ThreadSanitizer: lock-order-inversion
    // CHECK-NOT: WARNING: ThreadSanitizer:
  }

 private:
  void Lock2(size_t l1, size_t l2) { L(l1); L(l2); U(l2); U(l1); }
  void Lock_0_1() { Lock2(0, 1); }
  void Lock_1_0() { Lock2(1, 0); }

  void CreateAndDestroyManyLocks() {
    PaddedLock create_many_locks_but_never_acquire[kDeadlockGraphSize];
  }
  void CreateLockUnlockAndDestroyManyLocks() {
    PaddedLock many_locks[kDeadlockGraphSize];
    for (size_t i = 0; i < kDeadlockGraphSize; i++) {
      many_locks[i].lock();
      many_locks[i].unlock();
    }
  }

  // LockTest Member function callback.
  struct CB {
    void (LockTest::*f)();
    LockTest *lt;
  };

  // Thread function with CB.
  static void *Thread(void *param) {
    CB *cb = (CB*)param;
    (cb->lt->*cb->f)();
    return NULL;
  }

  void RunThreads(void (LockTest::*f1)(), void (LockTest::*f2)()) {
    const int kNumThreads = 2;
    pthread_t t[kNumThreads];
    CB cb[2] = {{f1, this}, {f2, this}};
    for (int i = 0; i < kNumThreads; i++)
      pthread_create(&t[i], 0, Thread, &cb[i]);
    for (int i = 0; i < kNumThreads; i++)
      pthread_join(t[i], 0);
  }

  static const size_t kDeadlockGraphSize = 4096;
  size_t n_;
  PaddedLock *locks_;
};

int main() {
  { LockTest t(5); t.Test1(); }
  { LockTest t(5); t.Test2(); }
  { LockTest t(5); t.Test3(); }
  { LockTest t(5); t.Test4(); }
  { LockTest t(5); t.Test5(); }
  fprintf(stderr, "DONE\n");
  // CHECK: DONE
}
