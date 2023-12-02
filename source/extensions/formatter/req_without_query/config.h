#pragma once

#include "source/common/formatter/substitution_formatter.h"

namespace Envoy {
namespace Extensions {
namespace Formatter {

class ReqWithoutQueryFactory : public ::Envoy::Formatter::CommandParserFactory {
public:
  ::Envoy::Formatter::CommandParserPtr
  createCommandParserFromProto(const Protobuf::Message&,
                               Server::Configuration::CommonFactoryContext&) override;
  ProtobufTypes::MessagePtr createEmptyConfigProto() override;
  std::string name() const override;
};

} // namespace Formatter
} // namespace Extensions
} // namespace Envoy
