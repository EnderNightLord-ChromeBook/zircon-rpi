// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/iterator/allocated-node-iterator.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/status.h>
#include <stdint.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/node-finder.h"

namespace blobfs {

AllocatedNodeIterator::AllocatedNodeIterator(NodeFinder* finder, Inode* inode)
    : finder_(finder), inode_(inode) {
  ZX_ASSERT(finder_ && inode_);
}

bool AllocatedNodeIterator::Done() const {
  return extent_index_ + NodeExtentCount() >= inode_->extent_count;
}

zx::status<ExtentContainer*> AllocatedNodeIterator::Next() {
  ZX_DEBUG_ASSERT(!Done());

  auto next_node = finder_->GetNode(NextNodeIndex());
  if (next_node.is_error()) {
    FX_LOGS(ERROR) << "GetNode(" << NextNodeIndex() << ") failed: " << next_node.status_value();
    if (inode_) {
      FX_LOGS(ERROR) << "Inode: " << *inode_;
    }
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }
  ExtentContainer* next = next_node->AsExtentContainer();

  ZX_DEBUG_ASSERT(next != nullptr);
  bool is_container = next->header.IsAllocated() && next->header.IsExtentContainer();
  if (!is_container) {
    FX_LOGS(ERROR) << "Next node " << NextNodeIndex() << " invalid: " << *next;
    return zx::error(ZX_ERR_IO_DATA_INTEGRITY);
  }
  extent_index_ += NodeExtentCount();
  extent_node_ = next;

  return zx::ok(extent_node_);
}

uint32_t AllocatedNodeIterator::ExtentIndex() const { return extent_index_; }

uint32_t AllocatedNodeIterator::NextNodeIndex() const {
  ZX_DEBUG_ASSERT(!Done());
  return IsInode() ? inode_->header.next_node : extent_node_->header.next_node;
}

uint32_t AllocatedNodeIterator::NodeExtentCount() const {
  if (IsInode()) {
    return inode_->extent_count > 0 ? 1 : 0;
  }
  return extent_node_->extent_count;
}

bool AllocatedNodeIterator::IsInode() const { return extent_node_ == nullptr; }

}  // namespace blobfs
