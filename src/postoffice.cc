/**
 *  Copyright (c) 2015 by Contributors
 */
#include <unistd.h>
#include <thread>
#include <chrono>
#include "ps/internal/postoffice.h"
#include "ps/internal/message.h"
#include "ps/base.h"
#include "elastic_training.cc"

namespace ps {
Postoffice::Postoffice() {
  van_ = Van::Create("zmq");
  env_ref_ = Environment::_GetSharedRef();
}

void Postoffice::InitEnvironment() {
  const char* val = NULL;
  val = CHECK_NOTNULL(Environment::Get()->find("DMLC_NUM_WORKER"));
  num_workers_ = atoi(val);
  val =  CHECK_NOTNULL(Environment::Get()->find("DMLC_NUM_SERVER"));
  num_servers_ = atoi(val);
  val = CHECK_NOTNULL(Environment::Get()->find("DMLC_ROLE"));
  std::string role(val);
  is_worker_ = role == "worker";
  is_server_ = role == "server";
  is_scheduler_ = role == "scheduler";
  verbose_ = GetEnv("PS_VERBOSE", 0);
  is_new_worker_ = GetEnv("NEW_WORKER", 0);
}

void Postoffice::updateNumWorker(const char* val){
  int prev_num_worker = num_workers_;
  num_workers_ = atoi(val);
  PS_VLOG(1) << "Process:" << getpid() << " Updated num workers";
  // init node info.
  for (int i = prev_num_worker; i < num_workers_; ++i) {
    int id = WorkerRankToID(i);
    for (int g : {id, kWorkerGroup, kWorkerGroup + kServerGroup,
                    kWorkerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
      node_ids_[g].push_back(id);
      PS_VLOG(1) << "Process:" << getpid() << " pushed worker id: " << id << " to group :" << g;
    }
  }  
}

void Postoffice::updateEnvironmentVariable(const std::string& env_var, const std::string& val) {
  CHECK_EQ(env_var, "DMLC_NUM_WORKER") << " Only updating num_workers is allowed";
   // if environment variable is DMLC_NUM_WORKER, then update_num_worker
  // update your own environment variable
  if(is_scheduler_) {
    Message req;
    req.meta.request = true;
    req.meta.control.cmd = Control::Command::UPDATE_ENV_VAR;
    req.meta.app_id = 0;
    SArray<std::string> key {env_var,val};
    req.AddData(key);

  //  req.meta.customer_id = customer_id;
    req.meta.timestamp = van_->GetTimestamp();
    update_req_sent = 0;
    PS_VLOG(1) << "Process:" << getpid() << " In scheduler sending message to others";
    for (int r : GetNodeIDs(kWorkerGroup + kServerGroup)) {
      int recver_id = r;
      
      req.meta.recver = recver_id;
      req.meta.timestamp = van_->GetTimestamp();
      PS_VLOG(1)<< "Process:" << getpid() << " In scheduler sending message to receiver r:" << recver_id;
      CHECK_GT(van_->Send(req), 0);
      update_req_sent++;
    }
  }
  // update my own environment variable    
  updateNumWorker(val.c_str()); 
}

void Postoffice::notifyUpdateEnvReceived() {
  --update_req_sent;
  if(update_req_sent <= 0){
    PS_VLOG(1) << "PId:" <<getpid()<<" Invoking UpdateEnvSuccessCb";
    et_node_manager_->invokeSuccessResponseCallback();
  }
}

void Postoffice::Start(int customer_id, const char* argv0, const bool do_barrier) {
  start_mu_.lock();
  if (init_stage_ == 0) {
    InitEnvironment();
    // init glog
    if (argv0) {
      dmlc::InitLogging(argv0);
    } else {
      dmlc::InitLogging("ps-lite\0");
    }
    PS_VLOG(1) << " Process: " << getpid() << " in PS Start() init stage 0";


    // init node info.
    for (int i = 0; i < num_workers_; ++i) {
      int id = WorkerRankToID(i);
      for (int g : {id, kWorkerGroup, kWorkerGroup + kServerGroup,
                    kWorkerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
        node_ids_[g].push_back(id);
        PS_VLOG(1) << "Process:" << getpid() << " pushed worker id: " << id << " to group :" << g;
      }
    }

    for (int i = 0; i < num_servers_; ++i) {
      int id = ServerRankToID(i);
      for (int g : {id, kServerGroup, kWorkerGroup + kServerGroup,
                    kServerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
        node_ids_[g].push_back(id);
        PS_VLOG(1) << "Process:" << getpid() << " pushed server id: " << id << " to group :" << g;
      }
    }

    for (int g : {kScheduler, kScheduler + kServerGroup + kWorkerGroup,
                  kScheduler + kWorkerGroup, kScheduler + kServerGroup}) {
      node_ids_[g].push_back(kScheduler);
      PS_VLOG(1) << "Process:" << getpid() << " pushed scheduler id: " << kScheduler << " to group :" << g;

    }
    init_stage_++;
  }
  start_mu_.unlock();
  PS_VLOG(1) << " Process:" << getpid() << " Starting van_ with customerID:"<< customer_id;
  // start van
  van_->Start(customer_id);

  start_mu_.lock();
  if (init_stage_ == 1) {
    // record start time
    start_time_ = time(NULL);
    init_stage_++;
  }
  start_mu_.unlock();
  PS_VLOG(1) << "Process:" << getpid() << " do barrier is: " << do_barrier; 
  if(is_scheduler_){
    const char* instance_pool = NULL;
    instance_pool = CHECK_NOTNULL(Environment::Get()->find("INSTANCE_POOL"));
    if(strcmp(instance_pool, "DEFAULT") == 0) {
      et_node_manager_ = std::make_shared<ETDefaultNodeManager>();
    } else {
      PS_VLOG(1) << "FATAL unknown instance pool";
      et_node_manager_ = nullptr;
    }
  } else {
    et_node_manager_ = nullptr;
  }
  // do a barrier here
  if (do_barrier) {
    Barrier(customer_id, kWorkerGroup + kServerGroup + kScheduler);
  }

}

void Postoffice::Finalize(const int customer_id, const bool do_barrier) {
  if (do_barrier) Barrier(customer_id, kWorkerGroup + kServerGroup + kScheduler);
  if (customer_id == 0) {
    num_workers_ = 0;
    num_servers_ = 0;
    van_->Stop();
    init_stage_ = 0;
    customers_.clear();
    node_ids_.clear();
    barrier_done_.clear();
    server_key_ranges_.clear();
    heartbeats_.clear();
    if (exit_callback_) exit_callback_();
  }
}


void Postoffice::AddCustomer(Customer* customer) {
  std::lock_guard<std::mutex> lk(mu_);
  int app_id = CHECK_NOTNULL(customer)->app_id();
  // check if the customer id has existed
  int customer_id = CHECK_NOTNULL(customer)->customer_id();
  CHECK_EQ(customers_[app_id].count(customer_id), (size_t) 0) << "customer_id " \
    << customer_id << " already exists\n";
  customers_[app_id].insert(std::make_pair(customer_id, customer));
  std::unique_lock<std::mutex> ulk(barrier_mu_);
  barrier_done_[app_id].insert(std::make_pair(customer_id, false));
  PS_VLOG(1) << " Process:" << getpid() << " inserted in customers and barrier map:"
  << " appid:"<< app_id << " customer_id:" << customer_id; 
}


void Postoffice::RemoveCustomer(Customer* customer) {
  std::lock_guard<std::mutex> lk(mu_);
  int app_id = CHECK_NOTNULL(customer)->app_id();
  int customer_id = CHECK_NOTNULL(customer)->customer_id();
  customers_[app_id].erase(customer_id);
  if (customers_[app_id].empty()) {
    customers_.erase(app_id);
  }
  PS_VLOG(1)<< " Process:" << getpid() << " removed custId:" << customer_id 
  << " from appId:"<< app_id;  
}


Customer* Postoffice::GetCustomer(int app_id, int customer_id, int timeout) const {
  Customer* obj = nullptr;
  for (int i = 0; i < timeout * 1000 + 1; ++i) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      const auto it = customers_.find(app_id);
      if (it != customers_.end()) {
        std::unordered_map<int, Customer*> customers_in_app = it->second;
        obj = customers_in_app[customer_id];
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return obj;
}

void Postoffice::Barrier(int customer_id, int node_group, bool is_membership_change_barrier, const std::vector<std::pair<std::string, std::string> >
                              & data) {
  if (GetNodeIDs(node_group).size() <= 1) return;
  auto role = van_->my_node().role;
  if (role == Node::SCHEDULER) {
    CHECK(node_group & kScheduler);
  } else if (role == Node::WORKER) {
    CHECK(node_group & kWorkerGroup);
  } else if (role == Node::SERVER) {
    CHECK(node_group & kServerGroup);
  }

  std::unique_lock<std::mutex> ulk(barrier_mu_);
  PS_VLOG(1) << "Process:" << getpid() << " Barrier called for cust: " <<customer_id
  << " node_group:" << node_group << " My node role:" << role;
  if(is_new_worker_){
  PS_VLOG(1) << "Slept in barrier for new worker";
  sleep(100000);
  PS_VLOG(1) << " Woke up";
  }
  barrier_done_[0][customer_id] = false;
  Message req;
  req.meta.recver = kScheduler;
  req.meta.request = true;
  if(is_membership_change_barrier) {
    PS_VLOG(1) << "Process:" << getpid() << " MCBarrier called for cust: " <<customer_id
  << " node_group:" << node_group << " My node role:" << role;
    req.meta.control.cmd = Control::Command::MEMBERSHIP_CHANGE_BARRIER;
    if(data.size() > 0 ){
      for(auto entry : data){
        PS_VLOG(1) << " MCBarrier data " << entry.first << " " << entry.second; 

        SArray<std::string> key {entry.first, entry.second};
        req.AddData(key);
      }
    }
    req.meta.sender = van_->my_node().id;
  } else {
    req.meta.control.cmd = Control::BARRIER;
  }

  req.meta.app_id = 0;
  req.meta.customer_id = customer_id;
  req.meta.control.barrier_group = node_group;
  req.meta.timestamp = van_->GetTimestamp();
  PS_VLOG(1) << " Process:" << getpid() << " Sending: " << req.DebugString();

  CHECK_GT(van_->Send(req), 0);
  barrier_cond_.wait(ulk, [this, customer_id] {
      return barrier_done_[0][customer_id];
    });
}

const std::vector<Range>& Postoffice::GetServerKeyRanges() {
  server_key_ranges_mu_.lock();
  if (server_key_ranges_.empty()) {
    for (int i = 0; i < num_servers_; ++i) {
      server_key_ranges_.push_back(Range(
          kMaxKey / num_servers_ * i,
          kMaxKey / num_servers_ * (i+1)));
    }
  }
  server_key_ranges_mu_.unlock();
  return server_key_ranges_;
}

void Postoffice::Manage(const Message& recv) {
  CHECK(!recv.meta.control.empty());
  const auto& ctrl = recv.meta.control;
  if ((ctrl.cmd == Control::BARRIER || ctrl.cmd == Control::MEMBERSHIP_CHANGE_BARRIER) && !recv.meta.request) {
    barrier_mu_.lock();
    auto size = barrier_done_[recv.meta.app_id].size();
    for (size_t customer_id = 0; customer_id < size; customer_id++) {
      barrier_done_[recv.meta.app_id][customer_id] = true;
    }
    barrier_mu_.unlock();
    barrier_cond_.notify_all();
  }
}

std::vector<int> Postoffice::GetDeadNodes(int t) {
  std::vector<int> dead_nodes;
  if (!van_->IsReady() || t == 0) return dead_nodes;

  time_t curr_time = time(NULL);
  const auto& nodes = is_scheduler_
    ? GetNodeIDs(kWorkerGroup + kServerGroup)
    : GetNodeIDs(kScheduler);
  {
    std::lock_guard<std::mutex> lk(heartbeat_mu_);
    for (int r : nodes) {
      auto it = heartbeats_.find(r);
      if ((it == heartbeats_.end() || it->second + t < curr_time)
            && start_time_ + t < curr_time) {
        dead_nodes.push_back(r);
      }
    }
  }
  return dead_nodes;
}
}  // namespace ps
