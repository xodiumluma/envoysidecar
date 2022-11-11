#include "contrib/generic_proxy/filters/network/source/proxy.h"

#include "envoy/common/exception.h"
#include "envoy/network/connection.h"

#include "source/common/config/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/stream_info/stream_info_impl.h"

#include "contrib/generic_proxy/filters/network/source/interface/config.h"
#include "contrib/generic_proxy/filters/network/source/interface/filter.h"
#include "contrib/generic_proxy/filters/network/source/route.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace GenericProxy {

CodecFactoryPtr FilterConfig::codecFactoryFromProto(
    const envoy::config::core::v3::TypedExtensionConfig& codec_config,
    Envoy::Server::Configuration::FactoryContext& context) {
  auto& factory = Config::Utility::getAndCheckFactory<CodecFactoryConfig>(codec_config);

  ProtobufTypes::MessagePtr message = factory.createEmptyConfigProto();
  Envoy::Config::Utility::translateOpaqueConfig(codec_config.typed_config(),
                                                context.messageValidationVisitor(), *message);
  return factory.createFactory(*message, context);
}

Rds::RouteConfigProviderSharedPtr FilterConfig::routeConfigProviderFromProto(
    const ProxyConfig& config, Server::Configuration::FactoryContext& context,
    RouteConfigProviderManager& route_config_provider_manager) {
  if (config.has_generic_rds()) {
    if (config.generic_rds().config_source().config_source_specifier_case() ==
        envoy::config::core::v3::ConfigSource::kApiConfigSource) {
      const auto api_type = config.generic_rds().config_source().api_config_source().api_type();
      if (api_type != envoy::config::core::v3::ApiConfigSource::AGGREGATED_GRPC &&
          api_type != envoy::config::core::v3::ApiConfigSource::AGGREGATED_DELTA_GRPC) {
        throw EnvoyException("genericrds supports only aggregated api_type in api_config_source");
      }
    }

    return route_config_provider_manager.createRdsRouteConfigProvider(
        config.generic_rds(), context.getServerFactoryContext(), config.stat_prefix(),
        context.initManager());
  } else {
    return route_config_provider_manager.createStaticRouteConfigProvider(
        config.route_config(), context.getServerFactoryContext());
  }
}

std::vector<NamedFilterFactoryCb> FilterConfig::filtersFactoryFromProto(
    const ProtobufWkt::RepeatedPtrField<envoy::config::core::v3::TypedExtensionConfig>& filters,
    const std::string stats_prefix, Envoy::Server::Configuration::FactoryContext& context) {

  std::vector<NamedFilterFactoryCb> factories;
  bool has_terminal_filter = false;
  std::string terminal_filter_name;
  for (const auto& filter : filters) {
    if (has_terminal_filter) {
      throw EnvoyException(fmt::format("Terminal filter: {} must be the last generic L7 filter",
                                       terminal_filter_name));
    }

    auto& factory = Config::Utility::getAndCheckFactory<NamedFilterConfigFactory>(filter);

    ProtobufTypes::MessagePtr message = factory.createEmptyConfigProto();
    ASSERT(message != nullptr);
    Envoy::Config::Utility::translateOpaqueConfig(filter.typed_config(),
                                                  context.messageValidationVisitor(), *message);

    factories.push_back(
        {filter.name(), factory.createFilterFactoryFromProto(*message, stats_prefix, context)});

    if (factory.isTerminalFilter()) {
      terminal_filter_name = filter.name();
      has_terminal_filter = true;
    }
  }

  if (!has_terminal_filter) {
    throw EnvoyException("A terminal L7 filter is necessary for generic proxy");
  }
  return factories;
}

ActiveStream::ActiveStream(Filter& parent, RequestPtr request)
    : parent_(parent), downstream_request_stream_(std::move(request)) {}

ActiveStream::~ActiveStream() {
  for (auto& filter : decoder_filters_) {
    filter->filter_->onDestroy();
  }
  for (auto& filter : encoder_filters_) {
    if (filter->isDualFilter()) {
      continue;
    }
    filter->filter_->onDestroy();
  }
}

Envoy::Event::Dispatcher& ActiveStream::dispatcher() { return parent_.connection().dispatcher(); }
const CodecFactory& ActiveStream::downstreamCodec() { return *parent_.config_->codec_factory_; }
void ActiveStream::resetStream() {
  if (active_stream_reset_) {
    return;
  }
  active_stream_reset_ = true;
  parent_.deferredStream(*this);
}

void ActiveStream::sendLocalReply(Status status, ResponseUpdateFunction&& func) {
  ASSERT(parent_.creator_ != nullptr);
  local_or_upstream_response_stream_ =
      parent_.creator_->response(status, *downstream_request_stream_);

  ASSERT(local_or_upstream_response_stream_ != nullptr);

  if (func != nullptr) {
    func(*local_or_upstream_response_stream_);
  }

  parent_.sendReplyDownstream(*local_or_upstream_response_stream_, *this);
}

void ActiveStream::continueDecoding() {
  if (active_stream_reset_ || downstream_request_stream_ == nullptr) {
    return;
  }

  if (cached_route_entry_ == nullptr) {
    cached_route_entry_ = parent_.config_->routeEntry(*downstream_request_stream_);
  }

  ASSERT(downstream_request_stream_ != nullptr);
  for (; next_decoder_filter_index_ < decoder_filters_.size();) {
    auto status = decoder_filters_[next_decoder_filter_index_]->filter_->onStreamDecoded(
        *downstream_request_stream_);
    next_decoder_filter_index_++;
    if (status == FilterStatus::StopIteration) {
      break;
    }
  }
  if (next_decoder_filter_index_ == decoder_filters_.size()) {
    ENVOY_LOG(debug, "Complete decoder filters");
  }
}

void ActiveStream::upstreamResponse(ResponsePtr response) {
  local_or_upstream_response_stream_ = std::move(response);
  continueEncoding();
}

void ActiveStream::completeDirectly() { parent_.deferredStream(*this); };

void ActiveStream::continueEncoding() {
  if (active_stream_reset_ || local_or_upstream_response_stream_ == nullptr) {
    return;
  }

  ASSERT(local_or_upstream_response_stream_ != nullptr);
  for (; next_encoder_filter_index_ < encoder_filters_.size();) {
    auto status = encoder_filters_[next_encoder_filter_index_]->filter_->onStreamEncoded(
        *local_or_upstream_response_stream_);
    next_encoder_filter_index_++;
    if (status == FilterStatus::StopIteration) {
      break;
    }
  }

  if (next_encoder_filter_index_ == encoder_filters_.size()) {
    ENVOY_LOG(debug, "Complete decoder filters");
    parent_.sendReplyDownstream(*local_or_upstream_response_stream_, *this);
  }
}

void ActiveStream::onEncodingSuccess(Buffer::Instance& buffer, bool close_connection) {
  ASSERT(parent_.connection().state() == Network::Connection::State::Open);
  parent_.deferredStream(*this);
  parent_.connection().write(buffer, close_connection);
}

void ActiveStream::initializeFilterChain(FilterChainFactory& factory) {
  factory.createFilterChain(*this);
  // Reverse the encoder filter chain so that the first encoder filter is the last filter in the
  // chain.
  std::reverse(encoder_filters_.begin(), encoder_filters_.end());
}

Envoy::Network::FilterStatus Filter::onData(Envoy::Buffer::Instance& data, bool) {
  if (downstream_connection_closed_) {
    return Envoy::Network::FilterStatus::StopIteration;
  }

  decoder_->decode(data);
  return Envoy::Network::FilterStatus::StopIteration;
}

void Filter::onDecodingSuccess(RequestPtr request) { newDownstreamRequest(std::move(request)); }

void Filter::onDecodingFailure() {
  resetStreamsForUnexpectedError();
  connection().close(Network::ConnectionCloseType::FlushWrite);
}

void Filter::sendReplyDownstream(Response& response, ResponseEncoderCallback& callback) {
  response_encoder_->encode(response, callback);
}

void Filter::newDownstreamRequest(RequestPtr request) {
  auto stream = std::make_unique<ActiveStream>(*this, std::move(request));
  auto raw_stream = stream.get();
  LinkedList::moveIntoList(std::move(stream), active_streams_);

  // Initialize filter chian.
  raw_stream->initializeFilterChain(*config_);
  // Start request.
  raw_stream->continueDecoding();
}

void Filter::deferredStream(ActiveStream& stream) {
  if (!stream.inserted()) {
    return;
  }
  callbacks_->connection().dispatcher().deferredDelete(stream.removeFromList(active_streams_));
}

void Filter::resetStreamsForUnexpectedError() {
  while (!active_streams_.empty()) {
    active_streams_.front()->resetStream();
  }
}

} // namespace GenericProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
