#include "source/common/stats/isolated_store_impl.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "source/common/common/utility.h"
#include "source/common/stats/histogram_impl.h"
#include "source/common/stats/utility.h"

namespace Envoy {
namespace Stats {

IsolatedStoreImpl::IsolatedStoreImpl() : IsolatedStoreImpl(std::make_unique<SymbolTableImpl>()) {}

IsolatedStoreImpl::IsolatedStoreImpl(std::unique_ptr<SymbolTable>&& symbol_table)
    : IsolatedStoreImpl(*symbol_table) {
  symbol_table_storage_ = std::move(symbol_table);
}

IsolatedStoreImpl::IsolatedStoreImpl(SymbolTable& symbol_table)
    : alloc_(symbol_table), counters_([this](StatName name) -> CounterSharedPtr {
        return alloc_.makeCounter(name, name, StatNameTagVector{});
      }),
      gauges_([this](StatName name, Gauge::ImportMode import_mode) -> GaugeSharedPtr {
        return alloc_.makeGauge(name, name, StatNameTagVector{}, import_mode);
      }),
      histograms_([this](StatName name, Histogram::Unit unit) -> HistogramSharedPtr {
        return HistogramSharedPtr(new HistogramImpl(name, unit, *this, name, StatNameTagVector{}));
      }),
      text_readouts_([this](StatName name, TextReadout::Type) -> TextReadoutSharedPtr {
        return alloc_.makeTextReadout(name, name, StatNameTagVector{});
      }),
      null_counter_(new NullCounterImpl(symbol_table)),
      null_gauge_(new NullGaugeImpl(symbol_table)) {}

ScopeSharedPtr IsolatedStoreImpl::rootScope() {
  if (lazy_default_scope_ == nullptr) {
    StatNameManagedStorage name_storage("", symbolTable());
    lazy_default_scope_ = makeScope(name_storage.statName());
  }
  return lazy_default_scope_;
}

ConstScopeSharedPtr IsolatedStoreImpl::constRootScope() const {
  return const_cast<IsolatedStoreImpl*>(this)->rootScope();
}

IsolatedStoreImpl::~IsolatedStoreImpl() = default;

ScopeSharedPtr IsolatedScopeImpl::createScope(const std::string& name) {
  StatNameManagedStorage stat_name_storage(Utility::sanitizeStatsName(name), symbolTable());
  return scopeFromStatName(stat_name_storage.statName());
}

ScopeSharedPtr IsolatedScopeImpl::scopeFromStatName(StatName name) {
  SymbolTable::StoragePtr prefix_name_storage = symbolTable().join({prefix(), name});
  ScopeSharedPtr scope = store_.makeScope(StatName(prefix_name_storage.get()));
  addScopeToStore(scope);
  return scope;
}

ScopeSharedPtr IsolatedStoreImpl::makeScope(StatName name) {
  return std::make_shared<IsolatedScopeImpl>(name, *this);
}

} // namespace Stats
} // namespace Envoy
