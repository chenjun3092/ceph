// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_MIRROR_LEADER_WATCHER_H
#define CEPH_RBD_MIRROR_LEADER_WATCHER_H

#include <list>
#include <memory>
#include <string>

#include "librbd/ManagedLock.h"
#include "librbd/managed_lock/Types.h"
#include "librbd/Watcher.h"
#include "MirrorStatusWatcher.h"
#include "tools/rbd_mirror/leader_watcher/Types.h"

namespace librbd { class ImageCtx; }

namespace rbd {
namespace mirror {

struct Threads;

template <typename ImageCtxT = librbd::ImageCtx>
class LeaderWatcher : protected librbd::Watcher {
public:
  struct Listener {
    virtual ~Listener() {
    }

    virtual void post_acquire_handler(Context *on_finish) = 0;
    virtual void pre_release_handler(Context *on_finish) = 0;
  };

  LeaderWatcher(Threads *threads, librados::IoCtx &io_ctx, Listener *listener);

  int init();
  void shut_down();

  void init(Context *on_finish);
  void shut_down(Context *on_finish);

  bool is_leader();
  void release_leader();

private:
  /**
   * @verbatim
   *
   *  <uninitialized> <------------------------------ UNREGISTER_WATCH
   *     | (init)      ^                                      ^
   *     v             *                                      |
   *  CREATE_OBJECT  * *  (error)                     SHUT_DOWN_LEADER_LOCK
   *     |             *                                      ^
   *     v             *                                      |
   *  REGISTER_WATCH * *                                      | (shut_down)
   *     |                                                    |
   *     |           (no leader heartbeat and acquire failed) |
   *     | BREAK_LOCK <-------------------------------------\ |
   *     |    |                 (no leader heartbeat)       | |
   *     |    |  /----------------------------------------\ | |
   *     |    |  |              (lock_released received)    | |
   *     |    |  |  /-------------------------------------\ | |
   *     |    |  |  |                   (lock_acquired or | | |
   *     |    |  |  |                 heartbeat received) | | |
   *     |    |  |  |       (ENOENT)        /-----------\ | | |
   *     |    |  |  |  * * * * * * * * * *  |           | | | |
   *     v    v  v  v  v  (error)        *  v           | | | |
   *  ACQUIRE_LEADER_LOCK  * * * * *> GET_LOCKER ---> <secondary>
   *     |                   *                           ^
   * ....|...................*.............         .....|.....................
   * .   v                   *            .         .    |       post_release .
   * .INIT_STATUS_WATCHER  * *            .         .NOTIFY_LOCK_RELEASED     .
   * .   |                 (error)        .         .....^.....................
   * .   v                                .              |
   * .NOTIFY_LISTENERS                    .          RELEASE_LEADER_LOCK
   * .   |                                .              ^
   * .   v                                .         .....|.....................
   * .NOTIFY_LOCK_ACQUIRED   post_acquire .         .SHUT_DOWN_STATUS_WATCHER .
   * ....|.................................         .    ^                    .
   *     v                                          .    |                    .
   *  <leader> -----------------------------------> .NOTIFY_LISTENERS         .
   *            (shut_down, release_leader,         .             pre_release .
   *             notify error)                      ...........................
   * @endverbatim
   */

  class LeaderLock : public librbd::ManagedLock<ImageCtxT> {
  public:
    typedef librbd::ManagedLock<ImageCtxT> Parent;

    LeaderLock(librados::IoCtx& ioctx, ContextWQ *work_queue,
               const std::string& oid, LeaderWatcher *watcher,
               bool blacklist_on_break_lock,
               uint32_t blacklist_expire_seconds)
      : Parent(ioctx, work_queue, oid, watcher, librbd::managed_lock::EXCLUSIVE,
               blacklist_on_break_lock, blacklist_expire_seconds),
        watcher(watcher) {
    }

    bool is_leader() const {
      Mutex::Locker loker(Parent::m_lock);
      return Parent::is_state_post_acquiring() || Parent::is_state_locked();
    }

  protected:
    void post_acquire_lock_handler(int r, Context *on_finish) {
      if (r == 0) {
        // lock is owned at this point
        Mutex::Locker locker(Parent::m_lock);
        Parent::set_state_post_acquiring();
      }
      watcher->handle_post_acquire_leader_lock(r, on_finish);
    }
    void pre_release_lock_handler(bool shutting_down,
                                  Context *on_finish) {
      watcher->handle_pre_release_leader_lock(on_finish);
    }
    void post_release_lock_handler(bool shutting_down, int r,
                                   Context *on_finish) {
      watcher->handle_post_release_leader_lock(r, on_finish);
    }
  private:
    LeaderWatcher *watcher;
  };

  struct HandlePayloadVisitor : public boost::static_visitor<void> {
    LeaderWatcher *leader_watcher;
    Context *on_notify_ack;

    HandlePayloadVisitor(LeaderWatcher *leader_watcher, Context *on_notify_ack)
      : leader_watcher(leader_watcher), on_notify_ack(on_notify_ack) {
    }

    template <typename Payload>
    inline void operator()(const Payload &payload) const {
      leader_watcher->handle_payload(payload, on_notify_ack);
    }
  };

  struct C_GetLocker : public Context {
    LeaderWatcher *leader_watcher;
    librbd::managed_lock::Locker locker;

    C_GetLocker(LeaderWatcher *leader_watcher)
      : leader_watcher(leader_watcher) {
    }

    void finish(int r) override {
      leader_watcher->handle_get_locker(r, locker);
    }
  };

  Threads *m_threads;
  Listener *m_listener;

  Mutex m_lock;
  uint64_t m_notifier_id;
  Context *m_on_finish = nullptr;
  Context *m_on_shut_down_finish = nullptr;
  int m_acquire_attempts = 0;
  int m_notify_error = 0;
  std::unique_ptr<LeaderLock> m_leader_lock;
  std::unique_ptr<MirrorStatusWatcher> m_status_watcher;
  librbd::managed_lock::Locker m_locker;
  Context *m_timer_task = nullptr;

  bool is_leader(Mutex &m_lock);

  void cancel_timer_task();
  void schedule_timer_task(const std::string &name,
                           int delay_factor, bool leader,
                           void (LeaderWatcher<ImageCtxT>::*callback)());

  void create_leader_object();
  void handle_create_leader_object(int r);

  void register_watch();
  void handle_register_watch(int r);

  void shut_down_leader_lock();
  void handle_shut_down_leader_lock(int r);

  void unregister_watch();
  void handle_unregister_watch(int r);

  void break_leader_lock();
  void handle_break_leader_lock(int r);

  void get_locker();
  void handle_get_locker(int r, librbd::managed_lock::Locker& locker);

  void acquire_leader_lock(bool reset_attempt_counter);
  void acquire_leader_lock();
  void handle_acquire_leader_lock(int r);

  void release_leader_lock();
  void handle_release_leader_lock(int r);

  void init_status_watcher();
  void handle_init_status_watcher(int r);

  void shut_down_status_watcher();
  void handle_shut_down_status_watcher(int r);

  void notify_listener();
  void handle_notify_listener(int r);

  void notify_lock_acquired();
  void handle_notify_lock_acquired(int r);

  void notify_lock_released();
  void handle_notify_lock_released(int r);

  void notify_heartbeat();
  void handle_notify_heartbeat(int r);

  void handle_post_acquire_leader_lock(int r, Context *on_finish);
  void handle_pre_release_leader_lock(Context *on_finish);
  void handle_post_release_leader_lock(int r, Context *on_finish);

  void handle_notify(uint64_t notify_id, uint64_t handle,
                     uint64_t notifier_id, bufferlist &bl) override;

  void handle_heartbeat(Context *on_ack);
  void handle_lock_acquired(Context *on_ack);
  void handle_lock_released(Context *on_ack);

  void handle_payload(const leader_watcher::HeartbeatPayload &payload,
                      Context *on_notify_ack);
  void handle_payload(const leader_watcher::LockAcquiredPayload &payload,
                      Context *on_notify_ack);
  void handle_payload(const leader_watcher::LockReleasedPayload &payload,
                      Context *on_notify_ack);
  void handle_payload(const leader_watcher::UnknownPayload &payload,
                      Context *on_notify_ack);
};

} // namespace mirror
} // namespace rbd

#endif // CEPH_RBD_MIRROR_LEADER_WATCHER_H
