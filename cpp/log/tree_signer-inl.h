/* -*- indent-tabs-mode: nil -*- */
#ifndef CERT_TRANS_LOG_TREE_SIGNER_INL_H_
#define CERT_TRANS_LOG_TREE_SIGNER_INL_H_

#include "log/tree_signer.h"

#include <algorithm>
#include <chrono>
#include <glog/logging.h>
#include <set>
#include <stdint.h>
#include <unordered_map>

#include "log/database.h"
#include "log/log_signer.h"
#include "proto/serializer.h"
#include "util/status.h"
#include "util/util.h"


namespace cert_trans {

// Comparator for ordering pending hashes.
// Order by timestamp then hash.
template <class Logged>
struct PendingEntriesOrder
    : std::binary_function<const cert_trans::EntryHandle<Logged>&,
                           const cert_trans::EntryHandle<Logged>&, bool> {
  bool operator()(const cert_trans::EntryHandle<Logged>& x,
                  const cert_trans::EntryHandle<Logged>& y) const {
    CHECK(x.Entry().contents().sct().has_timestamp());
    CHECK(y.Entry().contents().sct().has_timestamp());
    const uint64_t x_time(x.Entry().contents().sct().timestamp());
    const uint64_t y_time(y.Entry().contents().sct().timestamp());
    if (x_time < y_time) {
      return true;
    } else if (x_time > y_time) {
      return false;
    }

    // Fallback to Hash as a final tie-breaker:
    return x.Entry().Hash() < y.Entry().Hash();
  }
};


template <class Logged>
TreeSigner<Logged>::TreeSigner(
    const std::chrono::duration<double>& guard_window, Database<Logged>* db,
    cert_trans::ConsistentStore<Logged>* consistent_store, LogSigner* signer)
    : guard_window_(guard_window),
      db_(db),
      consistent_store_(consistent_store),
      signer_(signer),
      cert_tree_(new Sha256Hasher()),
      latest_tree_head_() {
  // Try to get any STH previously published by this node.
  const util::StatusOr<ct::ClusterNodeState> node_state(
      consistent_store_->GetClusterNodeState());
  CHECK(node_state.ok() ||
        node_state.status().CanonicalCode() == util::error::NOT_FOUND)
      << "Problem fetching this node's previous state: "
      << node_state.status();
  if (node_state.ok()) {
    latest_tree_head_ = node_state.ValueOrDie().newest_sth();
  }

  BuildTree();
}


template <class Logged>
uint64_t TreeSigner<Logged>::LastUpdateTime() const {
  return latest_tree_head_.timestamp();
}


template <class Logged>
util::Status TreeSigner<Logged>::SequenceNewEntries() {
  const std::chrono::system_clock::time_point now(
      std::chrono::system_clock::now());
  util::StatusOr<int64_t> status_or_sequence_number(
      consistent_store_->NextAvailableSequenceNumber());
  if (!status_or_sequence_number.ok()) {
    return status_or_sequence_number.status();
  }
  int64_t next_sequence_number(status_or_sequence_number.ValueOrDie());
  CHECK_GE(next_sequence_number, 0);
  VLOG(1) << "Next available sequence number: " << next_sequence_number;

  EntryHandle<ct::SequenceMapping> mapping;
  util::Status status(consistent_store_->GetSequenceMapping(&mapping));
  if (!status.ok()) {
    return status;
  }

  // Hashes which are already sequenced.
  std::unordered_map<std::string, int64_t> sequenced_hashes;
  for (auto& m : mapping.Entry().mapping()) {
    CHECK(sequenced_hashes.insert(std::make_pair(m.entry_hash(),
                                                 m.sequence_number())).second);
  }

  std::vector<cert_trans::EntryHandle<Logged>> pending_entries;
  status = consistent_store_->GetPendingEntries(&pending_entries);
  if (!status.ok()) {
    return status;
  }
  std::sort(pending_entries.begin(), pending_entries.end(),
            PendingEntriesOrder<Logged>());

  VLOG(1) << "Sequencing " << pending_entries.size() << " entr"
          << (pending_entries.size() == 1 ? "y" : "ies");

  std::map<int64_t, const Logged*> seq_to_entry;
  int num_sequenced(0);
  for (auto& pending_entry : pending_entries) {
    const std::string& pending_hash(pending_entry.Entry().Hash());
    const std::chrono::system_clock::time_point cert_time(
        std::chrono::milliseconds(pending_entry.Entry().timestamp()));
    if (now - cert_time < guard_window_) {
      VLOG(1) << "Entry too recent: "
              << util::ToBase64(pending_entry.Entry().Hash());
      continue;
    }
    const auto seq_it(sequenced_hashes.find(pending_hash));
    if (seq_it == sequenced_hashes.end()) {
      // Need to sequence this one.
      VLOG(1) << util::ToBase64(pending_hash) << " = " << next_sequence_number;

      // Record the sequence -> hash mapping
      ct::SequenceMapping::Mapping* m(mapping.MutableEntry()->add_mapping());
      m->set_sequence_number(next_sequence_number);
      m->set_entry_hash(pending_entry.Entry().Hash());
      pending_entry.MutableEntry()->set_sequence_number(next_sequence_number);
      ++num_sequenced;
      ++next_sequence_number;
    } else {
      VLOG(1) << "Previously sequenced " << util::ToBase64(pending_hash)
              << " = " << next_sequence_number;
      pending_entry.MutableEntry()->set_sequence_number(seq_it->second);
    }
    CHECK(seq_to_entry.insert(std::make_pair(
                                  pending_entry.Entry().sequence_number(),
                                  pending_entry.MutableEntry())).second);
  }

  // Store updated sequence->hash mappings in the consistent store
  status = consistent_store_->UpdateSequenceMapping(&mapping);
  if (!status.ok()) {
    return status;
  }

  // Now add the sequenced entries to our local DB so that the local signer can
  // incorporate them.
  for (auto it(seq_to_entry.find(db_->TreeSize())); it != seq_to_entry.end();
       ++it) {
    VLOG(1) << "Adding to local DB: " << it->first;
    CHECK_EQ(it->first, it->second->sequence_number());
    CHECK_EQ(Database<Logged>::OK, db_->CreateSequencedEntry(*(it->second)));
  }

  VLOG(1) << "Sequenced " << num_sequenced << " entries.";

  return util::Status::OK;
}


// DB_ERROR: the database is inconsistent with our inner self.
// However, if the database itself is giving inconsistent answers, or failing
// reads/writes, then we die.
template <class Logged>
typename TreeSigner<Logged>::UpdateResult TreeSigner<Logged>::UpdateTree() {
  // Try to make local timestamps unique, but there's always a chance that
  // multiple nodes in the cluster may make STHs with the same timestamp.
  // That'll get handled by the Serving STH selection code.
  uint64_t min_timestamp = LastUpdateTime() + 1;

  // Sequence any new sequenced entries from our local DB.
  for (int64_t i(cert_tree_.LeafCount());; ++i) {
    Logged logged;
    typename Database<Logged>::LookupResult result(
        db_->LookupByIndex(i, &logged));
    if (result == Database<Logged>::NOT_FOUND) {
      break;
    }
    CHECK_EQ(Database<Logged>::LOOKUP_OK, result);
    CHECK_EQ(logged.sequence_number(), i);
    AppendToTree(logged);
    min_timestamp = std::max(min_timestamp, logged.sct().timestamp());
  }
  int64_t next_seq(cert_tree_.LeafCount());
  CHECK_GE(next_seq, 0);

  // Our tree is consistent with the database, i.e., each leaf in the tree has
  // a matching sequence number in the database (at least assuming overwriting
  // the sequence number is not allowed).
  ct::SignedTreeHead new_sth;
  TimestampAndSign(min_timestamp, &new_sth);

  // We don't actually store this STH anywhere durable yet, but rather let the
  // caller decide what to do with it.  (In practice, this will mean that it's
  // pushed out to this node's ClusterNodeState so that it becomes a candidate
  // for the cluster-wide Serving STH.)
  latest_tree_head_.CopyFrom(new_sth);
  return OK;
}


template <class Logged>
void TreeSigner<Logged>::BuildTree() {
  DCHECK_EQ(0U, cert_tree_.LeafCount())
      << "Attempting to build a tree when one already exists";
  // Read the latest sth.
  ct::SignedTreeHead sth;
  typename Database<Logged>::LookupResult db_result =
      db_->LatestTreeHead(&sth);

  if (db_result == Database<Logged>::NOT_FOUND)
    return;

  CHECK(db_result == Database<Logged>::LOOKUP_OK);

  // If the timestamp is from the future, then either the database is corrupt
  // or our clock is corrupt; either way we shouldn't be signing things.
  uint64_t current_time = util::TimeInMilliseconds();
  CHECK_LE(sth.timestamp(), current_time)
      << "Database has a timestamp from the future.";

  // Read all logged and signed entries.
  for (size_t i = 0; i < sth.tree_size(); ++i) {
    Logged logged;
    CHECK_EQ(Database<Logged>::LOOKUP_OK, db_->LookupByIndex(i, &logged));
    CHECK_LE(logged.timestamp(), sth.timestamp());
    CHECK_EQ(logged.sequence_number(), i);

    AppendToTree(logged);
    VLOG_IF(1, ((i % 100000) == 0)) << "added entry index " << i
                                    << " to the tree signer";
  }

  // Check the root hash.
  CHECK_EQ(cert_tree_.CurrentRoot(), sth.sha256_root_hash());

  latest_tree_head_.CopyFrom(sth);

  // Read the remaining sequenced entries. Note that it is possible to have
  // more
  // entries with sequence numbers than what the latest sth says. This happens
  // when we assign some sequence numbers but die before we manage to sign the
  // sth. It's not an inconsistency and will be corrected with UpdateTree().
  for (size_t i = sth.tree_size();; ++i) {
    Logged logged;
    typename Database<Logged>::LookupResult db_result =
        db_->LookupByIndex(i, &logged);
    if (db_result == Database<Logged>::NOT_FOUND)
      break;
    CHECK_EQ(Database<Logged>::LOOKUP_OK, db_result);
    CHECK_EQ(logged.sequence_number(), i);

    AppendToTree(logged);
  }
}


template <class Logged>
bool TreeSigner<Logged>::Append(const Logged& logged) {
  // Serialize for inclusion in the tree.
  std::string serialized_leaf;
  CHECK(logged.SerializeForLeaf(&serialized_leaf));

  CHECK_EQ(logged.sequence_number(), cert_tree_.LeafCount());
  // Commit the sequence number of this certificate locally
  typename Database<Logged>::WriteResult db_result =
      db_->CreateSequencedEntry(logged);

  if (db_result != Database<Logged>::OK) {
    CHECK_EQ(Database<Logged>::SEQUENCE_NUMBER_ALREADY_IN_USE, db_result);
    LOG(ERROR) << "Attempt to assign duplicate sequence number "
               << cert_tree_.LeafCount();
    return false;
  }

  // Update in-memory tree.
  cert_tree_.AddLeaf(serialized_leaf);
  return true;
}


template <class Logged>
void TreeSigner<Logged>::AppendToTree(const Logged& logged) {
  // Serialize for inclusion in the tree.
  std::string serialized_leaf;
  CHECK(logged.SerializeForLeaf(&serialized_leaf));

  // Update in-memory tree.
  cert_tree_.AddLeaf(serialized_leaf);
}


template <class Logged>
void TreeSigner<Logged>::TimestampAndSign(uint64_t min_timestamp,
                                          ct::SignedTreeHead* sth) {
  sth->set_version(ct::V1);
  sth->set_sha256_root_hash(cert_tree_.CurrentRoot());
  uint64_t timestamp = util::TimeInMilliseconds();
  if (timestamp < min_timestamp)
    // TODO(ekasper): shouldn't really happen if everyone's clocks are in sync;
    // log a warning if the skew is over some threshold?
    timestamp = min_timestamp;
  sth->set_timestamp(timestamp);
  sth->set_tree_size(cert_tree_.LeafCount());
  LogSigner::SignResult ret = signer_->SignTreeHead(sth);
  if (ret != LogSigner::OK)
    // Make this one a hard fail. There is really no excuse for it.
    abort();
}


}  // namespace cert_trans


#endif  // CERT_TRANS_LOG_TREE_SIGNER_INL_H_
