// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-compilation-info.h"

#include "src/compiler/compilation-dependencies.h"
#include "src/compiler/js-heap-broker.h"
#include "src/execution/isolate.h"
#include "src/flags/flags.h"
#include "src/handles/persistent-handles.h"
#include "src/maglev/maglev-code-generator.h"
#include "src/maglev/maglev-compilation-unit.h"
#include "src/maglev/maglev-concurrent-dispatcher.h"
#include "src/maglev/maglev-graph-labeller.h"
#include "src/objects/js-function-inl.h"
#include "src/utils/identity-map.h"

namespace v8 {
namespace internal {
namespace maglev {

namespace {

constexpr char kMaglevZoneName[] = "maglev-compilation-job-zone";

class V8_NODISCARD MaglevCompilationHandleScope final {
 public:
  MaglevCompilationHandleScope(Isolate* isolate,
                               maglev::MaglevCompilationInfo* info)
      : info_(info), persistent_(isolate), exported_info_(info) {
    info->ReopenAndCanonicalizeHandlesInNewScope(isolate);
  }

  ~MaglevCompilationHandleScope() {
    info_->set_persistent_handles(persistent_.Detach());
  }

 private:
  maglev::MaglevCompilationInfo* const info_;
  PersistentHandlesScope persistent_;
  ExportedMaglevCompilationInfo exported_info_;
};

}  // namespace

MaglevCompilationInfo::MaglevCompilationInfo(Isolate* isolate,
                                             Handle<JSFunction> function,
                                             BytecodeOffset osr_offset)
    : zone_(isolate->allocator(), kMaglevZoneName),
      broker_(new compiler::JSHeapBroker(
          isolate, zone(), v8_flags.trace_heap_broker, CodeKind::MAGLEV)),
      toplevel_function_(function),
      osr_offset_(osr_offset)
#define V(Name) , Name##_(v8_flags.Name)
          MAGLEV_COMPILATION_FLAG_LIST(V)
#undef V
      ,
      specialize_to_function_context_(
          osr_offset == BytecodeOffset::None() &&
          v8_flags.maglev_function_context_specialization &&
          function->raw_feedback_cell().map() ==
              ReadOnlyRoots(isolate).one_closure_cell_map()) {
  DCHECK(maglev::IsMaglevEnabled());
  DCHECK_IMPLIES(osr_offset != BytecodeOffset::None(), v8_flags.maglev_osr);
  canonical_handles_ = std::make_unique<CanonicalHandlesMap>(
      isolate->heap(), ZoneAllocationPolicy(&zone_));
  compiler::CurrentHeapBrokerScope current_broker(broker_.get());

  collect_source_positions_ = isolate->NeedsDetailedOptimizedCodeLineInfo();
  if (collect_source_positions_) {
    SharedFunctionInfo::EnsureSourcePositionsAvailable(
        isolate, handle(function->shared(), isolate));
  }

  MaglevCompilationHandleScope compilation(isolate, this);

  compiler::CompilationDependencies* deps =
      zone()->New<compiler::CompilationDependencies>(broker(), zone());
  USE(deps);  // The deps register themselves in the heap broker.

  broker()->AttachCompilationInfo(this);

  // Heap broker initialization may already use IsPendingAllocation.
  isolate->heap()->PublishPendingAllocations();
  broker()->InitializeAndStartSerializing(
      handle(function->native_context(), isolate));
  broker()->StopSerializing();

  // Serialization may have allocated.
  isolate->heap()->PublishPendingAllocations();

  toplevel_compilation_unit_ =
      MaglevCompilationUnit::New(zone(), this, function);
}

MaglevCompilationInfo::~MaglevCompilationInfo() = default;

void MaglevCompilationInfo::set_graph_labeller(
    MaglevGraphLabeller* graph_labeller) {
  graph_labeller_.reset(graph_labeller);
}

void MaglevCompilationInfo::set_code_generator(
    std::unique_ptr<MaglevCodeGenerator> code_generator) {
  code_generator_ = std::move(code_generator);
}

namespace {
template <typename T>
Handle<T> CanonicalHandle(CanonicalHandlesMap* canonical_handles,
                          Tagged<T> object, Isolate* isolate) {
  DCHECK_NOT_NULL(canonical_handles);
  DCHECK(PersistentHandlesScope::IsActive(isolate));
  auto find_result = canonical_handles->FindOrInsert(object);
  if (!find_result.already_exists) {
    *find_result.entry = Handle<T>(object, isolate).location();
  }
  return Handle<T>(*find_result.entry);
}
}  // namespace

void MaglevCompilationInfo::ReopenAndCanonicalizeHandlesInNewScope(
    Isolate* isolate) {
  toplevel_function_ =
      CanonicalHandle(canonical_handles_.get(), *toplevel_function_, isolate);
}

void MaglevCompilationInfo::set_persistent_handles(
    std::unique_ptr<PersistentHandles>&& persistent_handles) {
  DCHECK_NULL(ph_);
  ph_ = std::move(persistent_handles);
  DCHECK_NOT_NULL(ph_);
}

std::unique_ptr<PersistentHandles>
MaglevCompilationInfo::DetachPersistentHandles() {
  DCHECK_NOT_NULL(ph_);
  return std::move(ph_);
}

void MaglevCompilationInfo::set_canonical_handles(
    std::unique_ptr<CanonicalHandlesMap>&& canonical_handles) {
  DCHECK_NULL(canonical_handles_);
  canonical_handles_ = std::move(canonical_handles);
  DCHECK_NOT_NULL(canonical_handles_);
}

std::unique_ptr<CanonicalHandlesMap>
MaglevCompilationInfo::DetachCanonicalHandles() {
  DCHECK_NOT_NULL(canonical_handles_);
  return std::move(canonical_handles_);
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
