#include <errno.h>
#include <boost/scoped_array.hpp>
#include <boost/bind.hpp>
#include <zookeeper/zookeeper.h>
#include <glog/logging.h>
#include "keeper_impl.h"
#include "util.h"
#include "functions.h"
#include "failure_result.h"
#include "callback_wrapper.h"

namespace ldd {
namespace cage {
using namespace std;

Keeper::Impl::Impl(net::EventLoop* event_loop, const KeeperListener& listener)
    : zh_(NULL),
      event_loop_(event_loop),
      event_(event_loop),
      listener_(listener)
{
}

Keeper::Impl::~Impl()
{
    Close();
}

bool Keeper::Impl::IsOpen() const {
    return (zh_ != NULL);
}

bool Keeper::Impl::IsUnrecoverable() const {
    return is_unrecoverable(zh_) != ZOK;
}

int Keeper::Impl::timeout() const {
    if (!IsOpen()) {
        return 0;
    }
    return zoo_recv_timeout(zh_);
}

bool Keeper::Impl::Open(const std::string& dest, int timeout) {
    CHECK(!IsOpen());
    CHECK_GE(timeout, 0);

    errno = 0;
    zh_ = zookeeper_init(dest.c_str(),
            listener_ ? WatchSession : NULL,
            timeout, NULL, this, 0);
    if (zh_) {
        UpdateEvent();
        return true;
    } else if (errno && errno != ENOMEM) {
        LOG(ERROR) << "Keeper::Open " << Status(errno).ToString();
    } else {
        LOG(FATAL) << "Keeper::Open Out of memory";
    }
    return false;
}

void Keeper::Impl::Close() {
    if (zh_) {
        int rc = zookeeper_close(zh_);
        LOG_IF(ERROR, rc) << "zookeeper_close: " << Status(rc).ToString();
        zh_ = NULL;
        listener_ = NULL;
        node_watcher_.clear();
        child_watcher_.clear();
        ClearEvent();
    }
}

Status Keeper::Impl::Interest(int* fd, int* interest,
        ldd::util::TimeDiff* timeout) {
    if (!IsOpen()) {
        return Status::kInvalidState;
    }
    struct timeval tv = {};
    Status s = zookeeper_interest(zh_, fd, interest, &tv);
    if (s.IsOk()) {
        *timeout = tv;
    }
    return s;
}

Status Keeper::Impl::Process(int events) {
    if (!IsOpen()) {
        return Status::kInvalidState;
    }
    return zookeeper_process(zh_, events);
}

void Keeper::Impl::AddAuth(const std::string& scheme, const std::string& cert,
        const AddAuthCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<AddAuthResult>(callback));
    s = zoo_add_auth(zh_, scheme.c_str(), cert.c_str(), cert.size(),
            AddAuthCompletion, cb.get());
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<AddAuthResult>(s));
}

void Keeper::Impl::Create(const std::string& path, const std::string& value,
        const std::vector<Acl>& acls, Mode::Type mode,
        const CreateCallback& callback) {
    Status s;
    struct ACL_vector aclv;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    AllocateACLVector(&aclv, (int32_t)acls.size());
    std::copy(acls.begin(), acls.end(), aclv.data);

    cb.reset(new CallbackWrapper::Impl<CreateResult>(callback));
    s = zoo_acreate(zh_, path.c_str(), value.data(), value.size(),
            &aclv, mode, CreateCompletion, cb.get());
    ::deallocate_ACL_vector(&aclv);
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<CreateResult>(s));
}

void Keeper::Impl::Delete(const std::string& path, int32_t version,
        const DeleteCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<DeleteResult>(callback));
    s = zoo_adelete(zh_, path.c_str(), version,
            DeleteCompletion, cb.get());
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<DeleteResult>(s));
}

void Keeper::Impl::Exists(const std::string& path,
        const NodeWatcher& watcher,
        const ExistsCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<ExistsResult>(callback));
    s = zoo_awexists(zh_, path.c_str(),
            watcher ? WatchNode : NULL, this,
            ExistsCompletion, cb.get());
    if (s.IsOk()) {
        NodeWatchersMap::iterator it = node_watcher_.find(path);
        if (it == node_watcher_.end()) {
            it = node_watcher_.insert(make_pair(path, NodeWatchers())).first;
            CHECK(it != node_watcher_.end());
        }
        it->second.push_back(watcher);
        cb.release();
        return;
    }
failed:
    callback(FailureResult<ExistsResult>(s));
}

void Keeper::Impl::Get(const std::string& path,
        const NodeWatcher& watcher,
        const GetCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<GetResult>(callback));
    s = zoo_awget(zh_, path.c_str(),
            watcher ? WatchNode : NULL, this,
            GetCompletion, cb.get());
    if (s.IsOk()) {
        NodeWatchersMap::iterator it = node_watcher_.find(path);
        if (it == node_watcher_.end()) {
            it = node_watcher_.insert(make_pair(path, NodeWatchers())).first;
            CHECK(it != node_watcher_.end());
        }
        it->second.push_back(watcher);
        cb.release();
        return;
    }
failed:
    callback(FailureResult<GetResult>(s));
}

void Keeper::Impl::Set(const std::string& path, const std::string& value,
        int32_t version, const SetCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<SetResult>(callback));
    s = zoo_aset(zh_, path.c_str(), value.data(), value.size(),
            version, SetCompletion, cb.get());
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<SetResult>(s));
}

void Keeper::Impl::GetAcl(const std::string& path,
        const GetAclCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<GetAclResult>(callback));
    s = zoo_aget_acl(zh_, path.c_str(), GetAclCompletion,
            cb.get());
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<GetAclResult>(s));
}

void Keeper::Impl::SetAcl(const std::string& path,
        const std::vector<Acl>& acls, int32_t version,
        const SetAclCallback& callback) {
    Status s;
    struct ACL_vector aclv;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    AllocateACLVector(&aclv, (int32_t)acls.size());
    std::copy(acls.begin(), acls.end(), aclv.data);

    cb.reset(new CallbackWrapper::Impl<SetAclResult>(callback));
    s = zoo_aset_acl(zh_, path.c_str(), version, &aclv,
            SetAclCompletion, cb.get());
    ::deallocate_ACL_vector(&aclv);
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<SetAclResult>(s));
}

void Keeper::Impl::GetChildren(const std::string& path,
        const ChildWatcher& watcher,
        const GetChildrenCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<GetChildrenResult>(callback));
    s = zoo_awget_children(zh_, path.c_str(),
            watcher ? WatchChild : NULL, this,
            GetChildrenCompletion, cb.get());
    if (s.IsOk()) {
        ChildWatchersMap::iterator it = child_watcher_.find(path);
        if (it == child_watcher_.end()) {
            it = child_watcher_.insert(make_pair(path, ChildWatchers())).first;
            CHECK(it != child_watcher_.end());
        }
        it->second.push_back(watcher);
        cb.release();
        return;
    }
failed:
    callback(FailureResult<GetChildrenResult>(s));
}

void Keeper::Impl::GetChildrenWithStat(const std::string& path,
        const ChildWatcher& watcher,
        const GetChildrenWithStatCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper> cb;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<GetChildrenWithStatResult>(callback));
    s = zoo_awget_children2(zh_, path.c_str(),
            watcher ? WatchChild : NULL, this,
            GetChildrenWithStatCompletion, cb.get());
    if (s.IsOk()) {
        ChildWatchersMap::iterator it = child_watcher_.find(path);
        if (it == child_watcher_.end()) {
            it = child_watcher_.insert(make_pair(path, ChildWatchers())).first;
            CHECK(it != child_watcher_.end());
        }
        it->second.push_back(watcher);
        cb.release();
        return;
    }
failed:
    callback(FailureResult<GetChildrenWithStatResult>(s));
}

void Keeper::Impl::Multi(const std::vector<Op*>& ops,
        const MultiCallback& callback) {
    Status s;
    std::auto_ptr<CallbackWrapper::Impl<MultiResult> > cb;
    boost::scoped_array<zoo_op_t> zops;
    int idx = 0;
    if (!IsOpen()) {
        s = Status::kInvalidState;
        goto failed;
    }
    cb.reset(new CallbackWrapper::Impl<MultiResult>(ops.size(), callback));
    zops.reset(new zoo_op_t[ops.size()]);
    for (std::vector<Op*>::const_iterator it = ops.begin();
            it != ops.end(); ++it, ++idx) {
        Op::Result* result = (*it)->MakeResult(&zops[idx]);
        cb->results().push_back(result);
    }
    CHECK_EQ(cb->results().size(), cb->count());

    s = zoo_amulti(zh_, ops.size(), zops.get(),
            cb->zresults(), MultiCompletion, cb.get());
    if (s.IsOk()) {
        cb.release();
        return;
    }
failed:
    callback(FailureResult<MultiResult>(s));
}

void Keeper::Impl::UpdateEvent() {
    int fd = -1;
    int interest = 0;
    int zkevents = 0;
    ldd::util::TimeDiff timeout;
    Status s = Interest(&fd, &interest, &timeout);
    if (s.IsOk()) {
        //LOG(INFO) << "UpdateEvent:  fd = " << fd 
        //    << " interest = " << interest
        //    << " timeout = " << timeout.ToMilliseconds();
    } else {
        CHECK(!s.IsBadArguments());
        LOG(INFO) << "Keeper interests: " << s.ToString()
            << " fd = " << fd
            << " interest = " << interest
            << " timeout(ms) = " << timeout.ToMilliseconds();
        if (timeout.IsZero()) {
            timeout = ldd::util::TimeDiff::Milliseconds(100);
        }
    }
    if (interest & ZOOKEEPER_READ) {
        zkevents |= ldd::net::FdEvent::kReadable;
    }
    if (interest & ZOOKEEPER_WRITE) {
        zkevents |= ldd::net::FdEvent::kWritable;
    }
    //add event to eventbase and one async call
    event_.AsyncWait(fd, zkevents, 
            boost::bind(&Impl::HandleEvent, this, _1), timeout);
}

void Keeper::Impl::HandleEvent(int events) {
    int interest = 0;
    if (events & ldd::net::FdEvent::kReadable) {
        interest |= ZOOKEEPER_READ;
    }
    if (events & ldd::net::FdEvent::kWritable) {
        interest |= ZOOKEEPER_WRITE;
    }
    Status s = Process(interest);
    if (!s.IsOk()) {
        CHECK(!s.IsBadArguments());
        //LOG(INFO) << "HandleEvent: " << s.ToString();
    }
    if (IsOpen() && !IsUnrecoverable()) {
        UpdateEvent();
    }
}

void Keeper::Impl::ClearEvent() {
    event_.Cancel();
}


} // namespace cage
} // namespace ldd
