// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/rpc/connection.h"

#include <iostream>

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/human_readable.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/auth_store.h"
#include "yb/rpc/rpc_introspection.pb.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/negotiation.h"
#include "yb/rpc/reactor.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/util/trace.h"

using std::shared_ptr;
using std::vector;
using strings::Substitute;

namespace yb {
namespace rpc {

METRIC_DEFINE_histogram(
    server, handler_latency_outbound_transfer, "Time taken to transfer the response ",
    yb::MetricUnit::kMicroseconds, "Microseconds spent to queue and write the response to the wire",
    60000000LU, 2);

///
/// Connection
///
Connection::Connection(ReactorThread* reactor_thread,
                       Sockaddr remote,
                       int socket,
                       Direction direction)
    : reactor_thread_(reactor_thread),
      socket_(socket),
      remote_(std::move(remote)),
      direction_(direction),
      last_activity_time_(MonoTime::Now(MonoTime::FINE)),
      is_epoll_registered_(false),
      next_call_id_(1),
      negotiation_complete_(false) {
  const auto metric_entity = reactor_thread->reactor()->messenger()->metric_entity();
  handler_latency_outbound_transfer_ = metric_entity ?
      METRIC_handler_latency_outbound_transfer.Instantiate(metric_entity) : nullptr;
}

Status Connection::SetNonBlocking(bool enabled) {
  return socket_.SetNonBlocking(enabled);
}

void Connection::EpollRegister(ev::loop_ref& loop) {  // NOLINT
  DCHECK(reactor_thread_->IsCurrentThread());
  DVLOG(4) << "Registering connection for epoll: " << ToString();
  write_io_.set(loop);
  write_io_.set(socket_.GetFd(), ev::WRITE);
  write_io_.set<Connection, &Connection::WriteHandler>(this);
  if (direction_ == CLIENT && negotiation_complete_) {
    write_io_.start();
  }
  read_io_.set(loop);
  read_io_.set(socket_.GetFd(), ev::READ);
  read_io_.set<Connection, &Connection::ReadHandler>(this);
  read_io_.start();
  is_epoll_registered_ = true;
}

Connection::~Connection() {
  // Must clear the outbound_transfers_ list before deleting.
  CHECK(outbound_transfers_.begin() == outbound_transfers_.end());

  // It's crucial that the connection is Shutdown first -- otherwise
  // our destructor will end up calling read_io_.stop() and write_io_.stop()
  // from a possibly non-reactor thread context. This can then make all
  // hell break loose with libev.
  CHECK(!is_epoll_registered_);
}

bool Connection::Idle() const {
  DCHECK(reactor_thread_->IsCurrentThread());
  // check if we're in the middle of receiving something
  AbstractInboundTransfer* transfer = inbound();
  if (transfer && (transfer->TransferStarted())) {
    return false;
  }
  // check if we still need to send something
  if (!outbound_transfers_.empty()) {
    return false;
  }
  // can't kill a connection if calls are waiting response
  if (!awaiting_response_.empty()) {
    return false;
  }

  if (!calls_being_handled_.empty()) {
    return false;
  }

  // We are not idle if we are in the middle of connection negotiation.
  if (!negotiation_complete_) {
    return false;
  }

  return true;
}

void Connection::Shutdown(const Status& status) {
  DCHECK(reactor_thread_->IsCurrentThread());
  shutdown_status_ = status;

  if (inbound() != nullptr && inbound()->TransferStarted()) {
    double secs_since_active = reactor_thread_->cur_time()
        .GetDeltaSince(last_activity_time_).ToSeconds();
    LOG(WARNING) << "Shutting down connection " << ToString() << " with pending inbound data ("
                 << inbound()->StatusAsString() << ", last active "
                 << HumanReadableElapsedTime::ToShortString(secs_since_active)
                 << " ago, status=" << status.ToString() << ")";
  }

  // Clear any calls which have been sent and were awaiting a response.
  for (const car_map_t::value_type& v : awaiting_response_) {
    CallAwaitingResponse* c = v.second;
    if (c->call) {
      c->call->SetFailed(status);
    }
    // And we must return the CallAwaitingResponse to the pool
    car_pool_.Destroy(c);
  }
  awaiting_response_.clear();

  // Clear any outbound transfers.
  while (!outbound_transfers_.empty()) {
    OutboundTransfer* t = &outbound_transfers_.front();
    outbound_transfers_.pop_front();
    delete t;
  }

  read_io_.stop();
  write_io_.stop();
  is_epoll_registered_ = false;
  WARN_NOT_OK(socket_.Close(), "Error closing socket");
}

void Connection::QueueOutbound(gscoped_ptr<OutboundTransfer> transfer) {
  DCHECK(reactor_thread_->IsCurrentThread());

  if (!shutdown_status_.ok()) {
    // If we've already shut down, then we just need to abort the
    // transfer rather than bothering to queue it.
    transfer->Abort(shutdown_status_);
    return;
  }

  DVLOG(3) << "Queueing transfer: " << transfer->HexDump();

  outbound_transfers_.push_back(*transfer.release());

  if (negotiation_complete_ && !write_io_.is_active()) {
    // If we weren't currently in the middle of sending anything,
    // then our write_io_ interest is stopped. Need to re-start it.
    // Only do this after connection negotiation is done doing its work.
    write_io_.start();
  }
}

Connection::CallAwaitingResponse::~CallAwaitingResponse() {
  DCHECK(conn->reactor_thread_->IsCurrentThread());
}

void Connection::CallAwaitingResponse::HandleTimeout(ev::timer& watcher, int revents) {  // NOLINT
  conn->HandleOutboundCallTimeout(this);
}

void Connection::HandleOutboundCallTimeout(CallAwaitingResponse* car) {
  DCHECK(reactor_thread_->IsCurrentThread());
  DCHECK(car->call);
  // The timeout timer is stopped by the car destructor exiting Connection::HandleCallResponse()
  DCHECK(!car->call->IsFinished());

  // Mark the call object as failed.
  car->call->SetTimedOut();

  // Drop the reference to the call. If the original caller has moved on after
  // seeing the timeout, we no longer need to hold onto the allocated memory
  // from the request.
  car->call.reset();

  // We still leave the CallAwaitingResponse in the map -- this is because we may still
  // receive a response from the server, and we don't want a spurious log message
  // when we do finally receive the response. The fact that CallAwaitingResponse::call
  // is a NULL pointer indicates to the response processing code that the call
  // already timed out.
}


// Callbacks after sending a call on the wire.
// This notifies the OutboundCall object to change its state to SENT once it
// has been fully transmitted.
struct CallTransferCallbacks : public TransferCallbacks {
 public:
  explicit CallTransferCallbacks(shared_ptr<OutboundCall> call)
      : call_(std::move(call)) {}

  void NotifyTransferFinished() override {
    // TODO: would be better to cancel the transfer while it is still on the queue if we
    // timed out before the transfer started, but there is still a race in the case of
    // a partial send that we have to handle here
    if (call_->IsFinished()) {
      DCHECK(call_->IsTimedOut());
    } else {
      call_->SetSent();
    }
    delete this;
  }

  void NotifyTransferAborted(const Status& status) override {
    VLOG(1) << "Connection torn down before " << call_->ToString()
            << " could send its call: " << status.ToString();
    delete this;
  }

 private:
  shared_ptr<OutboundCall> call_;
};

void Connection::QueueOutboundCall(const shared_ptr<OutboundCall>& call) {
  DCHECK(call);
  DCHECK_EQ(direction_, CLIENT);
  DCHECK(reactor_thread_->IsCurrentThread());

  if (PREDICT_FALSE(!shutdown_status_.ok())) {
    // Already shutdown
    call->SetFailed(shutdown_status_);
    return;
  }

  // At this point the call has a serialized request, but no call header, since we haven't
  // yet assigned a call ID.
  DCHECK(!call->call_id_assigned());

  // Assign the call ID.
  int32_t call_id = GetNextCallId();
  call->set_call_id(call_id);

  // Serialize the actual bytes to be put on the wire.
  slices_tmp_.clear();
  Status s = call->SerializeTo(&slices_tmp_);
  if (PREDICT_FALSE(!s.ok())) {
    call->SetFailed(s);
    return;
  }

  call->SetQueued();

  scoped_car car(car_pool_.make_scoped_ptr(car_pool_.Construct()));
  car->conn = this;
  car->call = call;

  // Set up the timeout timer.
  const MonoDelta& timeout = call->controller()->timeout();
  if (timeout.Initialized()) {
    reactor_thread_->RegisterTimeout(&car->timeout_timer);
    car->timeout_timer.set<CallAwaitingResponse, // NOLINT(*)
        &CallAwaitingResponse::HandleTimeout>(car.get());
    car->timeout_timer.set(timeout.ToSeconds(), 0);
    car->timeout_timer.start();
  }

  TransferCallbacks* cb = new CallTransferCallbacks(call);
  awaiting_response_[call_id] = car.release();
  QueueOutbound(gscoped_ptr<OutboundTransfer>(new OutboundTransfer(slices_tmp_, cb,
      handler_latency_outbound_transfer_)));
}

// Callbacks for sending an RPC call response from the server.
// This takes ownership of the InboundCall object so that, once it has
// been responded to, we can free up all of the associated memory.
struct ResponseTransferCallbacks : public TransferCallbacks {
 public:
  ResponseTransferCallbacks() {}

  virtual ~ResponseTransferCallbacks() {}

  virtual void NotifyTransferFinished() override {
    delete this;
  }

  virtual void NotifyTransferAborted(const Status& status) override {
    LOG(WARNING) << "Connection torn down before "
                 << call()->ToString() << " could send its response";
    delete this;
  }

 protected:
  virtual InboundCall* call() = 0;
};

class YBResponseTransferCallbacks : public ResponseTransferCallbacks {
 public:
  YBResponseTransferCallbacks(gscoped_ptr<YBInboundCall> call, YBConnection* conn)
      : call_(call.Pass()), conn_(conn) {}

  ~YBResponseTransferCallbacks() {
    // Remove the call from the map.
    InboundCall* call_from_map = EraseKeyReturnValuePtr(
        &conn_->calls_being_handled_, call_->call_id());
    DCHECK_EQ(call_from_map, call_.get());
  }

 protected:
  InboundCall* call() override {
    return call_.get();
  }

 private:
  gscoped_ptr<YBInboundCall> call_;
  YBConnection* conn_;
};

class RedisResponseTransferCallbacks : public ResponseTransferCallbacks {
 public:
  RedisResponseTransferCallbacks(gscoped_ptr<RedisInboundCall> call, RedisConnection* conn)
      : call_(call.Pass()), conn_(conn) {}

  ~RedisResponseTransferCallbacks() {
    conn_->FinishedHandlingACall();
  }

 protected:
  InboundCall* call() override {
    return call_.get();
  }

 private:
  gscoped_ptr<RedisInboundCall> call_;
  RedisConnection* conn_;
};

class CQLResponseTransferCallbacks : public ResponseTransferCallbacks {
 public:
  CQLResponseTransferCallbacks(gscoped_ptr<CQLInboundCall> call, CQLConnection* conn)
      : call_(call.Pass()), conn_(conn) {}

  ~CQLResponseTransferCallbacks() {
    conn_->FinishedHandlingACall();
  }

 protected:
  InboundCall* call() override {
    return call_.get();
  }

 private:
  gscoped_ptr<CQLInboundCall> call_;
  CQLConnection* conn_;
};

// Reactor task which puts a transfer on the outbound transfer queue.
class QueueTransferTask : public ReactorTask {
 public:
  QueueTransferTask(gscoped_ptr<OutboundTransfer> transfer, Connection* conn)
      : transfer_(transfer.Pass()), conn_(conn) {}

  virtual void Run(ReactorThread* thr) override {
    conn_->QueueOutbound(transfer_.Pass());
    delete this;
  }

  virtual void Abort(const Status& status) override {
    transfer_->Abort(status);
    delete this;
  }

 private:
  gscoped_ptr<OutboundTransfer> transfer_;
  Connection* conn_;
};

void Connection::set_user_credentials(const UserCredentials& user_credentials) {
  user_credentials_.CopyFrom(user_credentials);
}

void Connection::ReadHandler(ev::io& watcher, int revents) {  // NOLINT
  DCHECK(reactor_thread_->IsCurrentThread());

  DVLOG(3) << ToString() << " ReadHandler(revents=" << revents << ")";
  if (revents & EV_ERROR) {
    reactor_thread_->DestroyConnection(this, STATUS(NetworkError, ToString() +
                                     ": ReadHandler encountered an error"));
    return;
  }
  last_activity_time_ = reactor_thread_->cur_time();

  while (true) {
    if (inbound() == nullptr) {
      CreateInboundTransfer();
    }
    Status status = inbound()->ReceiveBuffer(socket_);
    if (PREDICT_FALSE(!status.ok())) {
      if (status.posix_code() == ESHUTDOWN) {
        VLOG(1) << ToString() << " shut down by remote end.";
      } else {
        LOG(WARNING) << ToString() << " recv error: " << status.ToString();
      }
      reactor_thread_->DestroyConnection(this, status);
      return;
    }
    if (!inbound()->TransferFinished()) {
      DVLOG(3) << ToString() << ": read is not yet finished yet.";
      return;
    }
    HandleFinishedTransfer();

    // TODO: it would seem that it would be good to loop around and see if
    // there is more data on the socket by trying another recv(), but it turns
    // out that it really hurts throughput to do so. A better approach
    // might be for each InboundTransfer to actually try to read an extra byte,
    // and if it succeeds, then we'd copy that byte into a new InboundTransfer
    // and loop around, since it's likely the next call also arrived at the
    // same time.
    break;
  }
}

void Connection::HandleCallResponse(gscoped_ptr<AbstractInboundTransfer> transfer) {
  DCHECK(reactor_thread_->IsCurrentThread());
  gscoped_ptr<CallResponse> resp(new CallResponse);
  CHECK_OK(resp->ParseFrom(transfer.Pass()));

  CallAwaitingResponse* car_ptr =
      EraseKeyReturnValuePtr(&awaiting_response_, resp->call_id());
  if (PREDICT_FALSE(car_ptr == nullptr)) {
    LOG(WARNING) << ToString() << ": Got a response for call id " << resp->call_id() << " which "
                 << "was not pending! Ignoring.";
    return;
  }

  // The car->timeout_timer ev::timer will be stopped automatically by its destructor.
  scoped_car car(car_pool_.make_scoped_ptr(car_ptr));

  if (PREDICT_FALSE(car->call.get() == nullptr)) {
    // The call already failed due to a timeout.
    VLOG(1) << "Got response to call id " << resp->call_id() << " after client already timed out";
    return;
  }

  car->call->SetResponse(resp.Pass());
}

void Connection::WriteHandler(ev::io& watcher, int revents) {  // NOLINT
  DCHECK(reactor_thread_->IsCurrentThread());

  if (revents & EV_ERROR) {
    reactor_thread_->DestroyConnection(this, STATUS(NetworkError, ToString() +
          ": writeHandler encountered an error"));
    return;
  }
  DVLOG(3) << ToString() << ": writeHandler: revents = " << revents;

  OutboundTransfer* transfer;
  if (outbound_transfers_.empty()) {
    LOG(WARNING) << ToString() << " got a ready-to-write callback, but there is "
                                  "nothing to write.";
    write_io_.stop();
    return;
  }

  while (!outbound_transfers_.empty()) {
    transfer = &(outbound_transfers_.front());

    last_activity_time_ = reactor_thread_->cur_time();
    Status status = transfer->SendBuffer(socket_);
    if (PREDICT_FALSE(!status.ok())) {
      LOG(WARNING) << ToString() << " send error: " << status.ToString();
      reactor_thread_->DestroyConnection(this, status);
      return;
    }

    if (!transfer->TransferFinished()) {
      DVLOG(3) << ToString() << ": writeHandler: xfer not finished.";
      return;
    }

    outbound_transfers_.pop_front();
    delete transfer;
  }


  // If we were able to write all of our outbound transfers,
  // we don't have any more to write.
  write_io_.stop();
}

std::string Connection::ToString() const {
  // This may be called from other threads, so we cannot
  // include anything in the output about the current state,
  // which might concurrently change from another thread.
  return strings::Substitute(
    "Connection ($0) $1 $2", this,
    direction_ == SERVER ? "server connection from" : "client connection to",
    remote_.ToString());
}

// Reactor task that transitions this Connection from connection negotiation to
// regular RPC handling. Destroys Connection on negotiation error.
class NegotiationCompletedTask : public ReactorTask {
 public:
  NegotiationCompletedTask(Connection* conn,
                           const Status& negotiation_status)
      : conn_(conn),
        negotiation_status_(negotiation_status) {
  }

  virtual void Run(ReactorThread* rthread) override {
    rthread->CompleteConnectionNegotiation(conn_, negotiation_status_);
    delete this;
  }

  virtual void Abort(const Status& status) override {
    DCHECK(conn_->reactor_thread()->reactor()->closing());
    VLOG(1) << "Failed connection negotiation due to shut down reactor thread: "
            << status.ToString();
    delete this;
  }

 private:
  scoped_refptr<Connection> conn_;
  Status negotiation_status_;
};

void Connection::CompleteNegotiation(const Status& negotiation_status) {
  auto task = new NegotiationCompletedTask(this, negotiation_status);
  reactor_thread_->reactor()->ScheduleReactorTask(task);
}

void Connection::MarkNegotiationComplete() {
  DCHECK(reactor_thread_->IsCurrentThread());
  negotiation_complete_ = true;
}

Status Connection::DumpPB(const DumpRunningRpcsRequestPB& req,
                          RpcConnectionPB* resp) {
  DCHECK(reactor_thread_->IsCurrentThread());
  resp->set_remote_ip(remote_.ToString());
  if (negotiation_complete_) {
    resp->set_state(RpcConnectionPB::OPEN);
    resp->set_remote_user_credentials(user_credentials_.ToString());
  } else {
    // It's racy to dump credentials while negotiating, since the Connection
    // object is owned by the negotiation thread at that point.
    resp->set_state(RpcConnectionPB::NEGOTIATING);
  }

  if (direction_ == CLIENT) {
    for (const car_map_t::value_type& entry : awaiting_response_) {
      CallAwaitingResponse* c = entry.second;
      if (c->call) {
        c->call->DumpPB(req, resp->add_calls_in_flight());
      }
    }
  } else if (direction_ == SERVER) {
    for (const inbound_call_map_t::value_type& entry : calls_being_handled_) {
      InboundCall* c = entry.second;
      c->DumpPB(req, resp->add_calls_in_flight());
    }
  } else {
    LOG(FATAL);
  }
  return Status::OK();
}

YBConnection::YBConnection(ReactorThread* reactor_thread,
                           Sockaddr remote,
                           int socket,
                           Direction direction)
    : Connection(reactor_thread, remote, socket, direction),
      sasl_client_(kSaslAppName, socket),
      sasl_server_(kSaslAppName, socket) {}

void YBConnection::RunNegotiation(const MonoTime& deadline) {
  Negotiation::YBNegotiation(this, deadline);
}

void YBConnection::CreateInboundTransfer() {
  return inbound_.reset(new YBInboundTransfer());
}

AbstractInboundTransfer *YBConnection::inbound() const {
  return inbound_.get();
}

TransferCallbacks* YBConnection::GetResponseTransferCallback(gscoped_ptr<InboundCall> call) {
  gscoped_ptr<YBInboundCall> yb_call(down_cast<YBInboundCall*>(call.release()));
  return new YBResponseTransferCallbacks(yb_call.Pass(), this);
}

void Connection::QueueResponseForCall(gscoped_ptr<InboundCall> call) {
  // This is usually called by the IPC worker thread when the response
  // is set, but in some circumstances may also be called by the
  // reactor thread (e.g. if the service has shut down)

  DCHECK_EQ(direction_, SERVER);

  // If the connection is torn down, then the QueueOutbound() call that
  // eventually runs in the reactor thread will take care of calling
  // ResponseTransferCallbacks::NotifyTransferAborted.

  std::vector<Slice> slices;  // Will point to data, that is owned by InboundCall.
  call->SerializeResponseTo(&slices);

  TransferCallbacks* cb = GetResponseTransferCallback(call.Pass());
  // After the response is sent, can delete the InboundCall object.
  gscoped_ptr<OutboundTransfer> t(new OutboundTransfer(slices, cb,
      handler_latency_outbound_transfer_));

  QueueTransferTask* task = new QueueTransferTask(t.Pass(), this);
  reactor_thread_->reactor()->ScheduleReactorTask(task);
}

Status YBConnection::InitSaslClient() {
  RETURN_NOT_OK(sasl_client().Init(kSaslProtoName));
  RETURN_NOT_OK(sasl_client().EnableAnonymous());
  RETURN_NOT_OK(sasl_client().EnablePlain(user_credentials().real_user(),
                                          user_credentials().password()));
  return Status::OK();
}

Status YBConnection::InitSaslServer() {
  // TODO: Do necessary configuration plumbing to enable user authentication.
  // Right now we just enable PLAIN with a "dummy" auth store, which allows everyone in.
  RETURN_NOT_OK(sasl_server().Init(kSaslProtoName));
  gscoped_ptr<AuthStore> auth_store(new DummyAuthStore());
  RETURN_NOT_OK(sasl_server().EnablePlain(auth_store.Pass()));
  return Status::OK();
}

void YBConnection::HandleFinishedTransfer() {
  if (direction_ == CLIENT) {
    HandleCallResponse(inbound_.PassAs<AbstractInboundTransfer>());
  } else if (direction_ == SERVER) {
    HandleIncomingCall(inbound_.PassAs<AbstractInboundTransfer>());
  } else {
    LOG(FATAL) << "Invalid direction: " << direction_;
  }
}

void YBConnection::HandleIncomingCall(gscoped_ptr<AbstractInboundTransfer> transfer) {
  DCHECK(reactor_thread_->IsCurrentThread());

  gscoped_ptr<YBInboundCall> call(new YBInboundCall(this));

  Status s = call->ParseFrom(transfer.Pass());
  if (!s.ok()) {
    LOG(WARNING) << ToString() << ": received bad data: " << s.ToString();
    reactor_thread_->DestroyConnection(this, s);
    return;
  }

  // call_id exists only for YB. Not for Redis.
  if (!InsertIfNotPresent(&calls_being_handled_, call->call_id(), call.get())) {
    LOG(WARNING) << ToString() << ": received call ID " << call->call_id()
                 << " but was already processing this ID! Ignoring";
    reactor_thread_->DestroyConnection(
        this, STATUS(RuntimeError, "Received duplicate call id",
                                   Substitute("$0", call->call_id())));
    return;
  }

  reactor_thread_->reactor()->messenger()->QueueInboundCall(call.PassAs<InboundCall>());
}

RedisConnection::RedisConnection(ReactorThread* reactor_thread,
                                 Sockaddr remote,
                                 int socket,
                                 Direction direction)
    : Connection(reactor_thread, remote, socket, direction), processing_call_(false) {}

void RedisConnection::RunNegotiation(const MonoTime& deadline) {
  Negotiation::RedisNegotiation(this, deadline);
}

void RedisConnection::CreateInboundTransfer() {
  return inbound_.reset(new RedisInboundTransfer());
}

AbstractInboundTransfer *RedisConnection::inbound() const {
  return inbound_.get();
}

TransferCallbacks* RedisConnection::GetResponseTransferCallback(gscoped_ptr<InboundCall> call) {
  gscoped_ptr<RedisInboundCall> redis_call(down_cast<RedisInboundCall *>(call.release()));
  return new RedisResponseTransferCallbacks(redis_call.Pass(), this);
}

void RedisConnection::HandleFinishedTransfer() {
  if (processing_call_) {
    DVLOG(4) << "Already handling a call from the client. Need to wait. " << ToString();
    return;
  }

  DCHECK_EQ(direction_, SERVER) << "Invalid direction for Redis: " << direction_;
  RedisInboundTransfer* next_transfer = inbound_->ExcessData();
  HandleIncomingCall(inbound_.PassAs<AbstractInboundTransfer>());
  inbound_.reset(next_transfer);
}

void RedisConnection::HandleIncomingCall(gscoped_ptr<AbstractInboundTransfer> transfer) {
  DCHECK(reactor_thread_->IsCurrentThread());

  gscoped_ptr<RedisInboundCall> call(new RedisInboundCall(this));

  Status s = call->ParseFrom(transfer.Pass());
  if (!s.ok()) {
    LOG(WARNING) << ToString() << ": received bad data: " << s.ToString();
    reactor_thread_->DestroyConnection(this, s);
    return;
  }

  processing_call_ = true;
  reactor_thread_->reactor()->messenger()->QueueInboundCall(call.PassAs<InboundCall>());
}

void RedisConnection::FinishedHandlingACall() {
  // If the next client call has already been received by the server. Check if it is
  // ready to be handled.
  processing_call_ = false;
  if (inbound_ && inbound_->TransferFinished()) {
    HandleFinishedTransfer();
  }
}

CQLConnection::CQLConnection(ReactorThread* reactor_thread,
                             Sockaddr remote,
                             int socket,
                             Direction direction)
    : Connection(reactor_thread, remote, socket, direction), sql_session_(new sql::SqlSession()) {}

void CQLConnection::RunNegotiation(const MonoTime& deadline) {
  Negotiation::CQLNegotiation(this, deadline);
}

void CQLConnection::CreateInboundTransfer() {
  return inbound_.reset(new CQLInboundTransfer());
}

AbstractInboundTransfer *CQLConnection::inbound() const {
  return inbound_.get();
}

TransferCallbacks* CQLConnection::GetResponseTransferCallback(gscoped_ptr<InboundCall> call) {
  gscoped_ptr<CQLInboundCall> cql_call(down_cast<CQLInboundCall *>(call.release()));
  return new CQLResponseTransferCallbacks(cql_call.Pass(), this);
}

void CQLConnection::HandleFinishedTransfer() {
  CHECK(direction_ == SERVER) << "Invalid direction for CQL: " << direction_;
  HandleIncomingCall(inbound_.PassAs<AbstractInboundTransfer>());
}

void CQLConnection::HandleIncomingCall(gscoped_ptr<AbstractInboundTransfer> transfer) {
  DCHECK(reactor_thread_->IsCurrentThread());

  gscoped_ptr<CQLInboundCall> call(new CQLInboundCall(this));

  Status s = call->ParseFrom(transfer.Pass());
  if (!s.ok()) {
    LOG(WARNING) << ToString() << ": received bad data: " << s.ToString();
    // TODO: shutdown? probably, since any future stuff on this socket will be
    // "unsynchronized"
    return;
  }

  reactor_thread_->reactor()->messenger()->QueueInboundCall(call.PassAs<InboundCall>());
}

void CQLConnection::FinishedHandlingACall() {
  // If the next client call has already been received by the server. Check if it is
  // ready to be handled.
  if (inbound_ && inbound_->TransferFinished()) {
    HandleFinishedTransfer();
  }
}

}  // namespace rpc
}  // namespace yb
