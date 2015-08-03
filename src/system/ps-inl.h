/**
 * @file   ps-inl.h
 * @brief  Implementation of ps.h
 */
#pragma once
#include "ps.h"
namespace ps {

inline Filter* SyncOpts::AddFilter(Filter::Type type) {
  filters.push_back(Filter());
  filters.back().set_type(type);
  return &(filters.back());
}

template<typename Val>
Task KVWorker<Val>::GetTask(const SyncOpts& opts) {
  Task req;
  req.set_request(true);
  for (int l : opts.deps) req.add_wait_time(l);
  for (const auto& f : opts.filters) req.add_filter()->CopyFrom(f);
  if (opts.cmd != 0) req.set_cmd(opts.cmd);
  return req;
}

inline int NextCustomerID() {
  return Postoffice::instance().manager().NextCustomerID();
}

inline Range<Key> MyKeyRange() { return Range<Key>(MyNode().key()); }


// The app this node runs
inline App* MyApp() { return Postoffice::instance().manager().app(); }

/*! \brief The global unique string ID of this node */
inline Node MyNode() { return Postoffice::instance().manager().van().my_node(); }
// Each unique string id of my node
inline std::string MyNodeID() { return MyNode().id(); }
/*! \brief Return true if this node is a worker node. */
inline int IsWorkerNode() { return MyNode().role() == Node::WORKER; }
/*! \brief Return true if this node is a server node. */
inline int IsServerNode() { return MyNode().role() == Node::SERVER; }
/*! \brief Return true if this node is a scheduler node. */
inline int IsSchedulerNode() { return MyNode().role() == Node::SCHEDULER; }

inline std::string SchedulerID() {
  return Postoffice::instance().manager().van().scheduler().id();
}

// The rank ID of this node in its group. Assume this a worker node in a worker
// group with N workers. Then this node will be assigned an unique ID from 0,
// ..., N. Similarly for server and scheduler.
inline int MyRank() { return MyNode().rank(); }
// Total nodes in this node group.
inline int RankSize() {
  auto& mng = Postoffice::instance().manager();
  return IsWorkerNode() ? mng.num_workers() : (IsServerNode() ? mng.num_servers() : 1);
}

inline int NumWorkers() { return FLAGS_num_workers; }
inline int NumServers() { return FLAGS_num_servers; }

}  // namespace ps
