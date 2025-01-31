/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "tensorflow_federated/cc/core/impl/aggregation/core/dp_tensor_aggregator_bundle.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow_federated/cc/core/impl/aggregation/base/monitoring.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/agg_core.pb.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/datatype.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/dp_fedsql_constants.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/dp_tensor_aggregator.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/intrinsic.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/tensor.pb.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/tensor_aggregator.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/tensor_aggregator_factory.h"
#include "tensorflow_federated/cc/core/impl/aggregation/core/tensor_aggregator_registry.h"

namespace tensorflow_federated {
namespace aggregation {

DPTensorAggregatorBundle::DPTensorAggregatorBundle(
    std::vector<std::unique_ptr<DPTensorAggregator>> aggregators,
    std::vector<int> num_tensors_per_agg, double epsilon_per_agg,
    double delta_per_agg, int num_inputs)
    : aggregators_(std::move(aggregators)),
      num_tensors_per_agg_(num_tensors_per_agg),
      epsilon_per_agg_(epsilon_per_agg),
      delta_per_agg_(delta_per_agg),
      num_inputs_(num_inputs) {}

StatusOr<std::unique_ptr<TensorAggregator>>
DPTensorAggregatorBundleFactory::CreateInternal(
    const Intrinsic& intrinsic,
    const DPTensorAggregatorBundleState* aggregator_state) const {
  // Check that there is at least one nested intrinsic.
  if (intrinsic.nested_intrinsics.empty()) {
    return TFF_STATUS(INVALID_ARGUMENT)
           << "DPTensorAggregatorBundleFactory::CreateInternal: Expected "
           << "at least one nested intrinsic, got none.";
  }

  int num_inputs =
      (aggregator_state == nullptr ? 0 : aggregator_state->num_inputs());

  // Create the nested aggregators. Along the way, track how many input tensors
  // will be fed into each nested aggregator during Accumulate.
  std::vector<std::unique_ptr<DPTensorAggregator>> nested_aggregators;
  std::vector<int> num_tensors_per_agg;
  for (int i = 0; i < intrinsic.nested_intrinsics.size(); ++i) {
    const Intrinsic& nested = intrinsic.nested_intrinsics[i];
    // Resolve the intrinsic_uri to the registered TensorAggregatorFactory.
    TFF_ASSIGN_OR_RETURN(const TensorAggregatorFactory* factory,
                         GetAggregatorFactory(nested.uri));
    std::unique_ptr<TensorAggregator> aggregator_ptr;
    if (aggregator_state != nullptr) {
      TFF_ASSIGN_OR_RETURN(
          aggregator_ptr,
          factory->Deserialize(nested,
                               aggregator_state->nested_serialized_states(i)));
    } else {
      TFF_ASSIGN_OR_RETURN(aggregator_ptr, factory->Create(nested));
    }
    auto* dp_aggregator_ptr =
        dynamic_cast<DPTensorAggregator*>(aggregator_ptr.get());
    if (dp_aggregator_ptr == nullptr) {
      return TFF_STATUS(INVALID_ARGUMENT)
             << "DPTensorAggregatorBundleFactory::CreateInternal: Expected "
             << "all nested intrinsics to be DPTensorAggregators, got "
             << nested.uri;
    }
    aggregator_ptr.release();  // NOMUTANTS -- Memory ownership transfer.
    nested_aggregators.push_back(
        std::unique_ptr<DPTensorAggregator>(dp_aggregator_ptr));
    num_tensors_per_agg.push_back(nested.inputs.size());
  }

  int num_nested_intrinsics = intrinsic.nested_intrinsics.size();

  // Ensure that there are epsilon and delta parameters.
  if (intrinsic.parameters.size() != 2) {
    return TFF_STATUS(INVALID_ARGUMENT)
           << "DPTensorAggregatorBundleFactory::CreateInternal: Expected "
           << "2 parameters, got " << intrinsic.parameters.size();
  }

  // Validate epsilon and delta before splitting them.
  if (internal::GetTypeKind(intrinsic.parameters[kEpsilonIndex].dtype()) !=
      internal::TypeKind::kNumeric) {
    return TFF_STATUS(INVALID_ARGUMENT)
           << "DPTensorAggregatorBundleFactory::CreateInternal: Epsilon must "
           << "be numerical.";
  }
  double epsilon = intrinsic.parameters[kEpsilonIndex].AsScalar<double>();
  if (internal::GetTypeKind(intrinsic.parameters[kDeltaIndex].dtype()) !=
      internal::TypeKind::kNumeric) {
    return TFF_STATUS(INVALID_ARGUMENT)
           << "DPTensorAggregatorBundleFactory::CreateInternal: Delta must "
           << "be numerical.";
  }
  double delta = intrinsic.parameters[kDeltaIndex].AsScalar<double>();
  if (epsilon <= 0) {
    return TFF_STATUS(INVALID_ARGUMENT) << "DPTensorAggregatorBundleFactory::"
                                           "CreateInternal: Epsilon must be "
                                           "positive, but got "
                                        << epsilon;
  }
  if (delta < 0 || delta >= 1) {
    return TFF_STATUS(INVALID_ARGUMENT)
           << "DPTensorAggregatorBundleFactory::CreateInternal: Delta must be "
              "non-negative and less than 1, but got "
           << delta;
  }

  double epsilon_per_agg =
      (epsilon < kEpsilonThreshold ? epsilon / num_nested_intrinsics
                                   : kEpsilonThreshold);
  double delta_per_agg = delta / num_nested_intrinsics;

  return std::make_unique<DPTensorAggregatorBundle>(
      std::move(nested_aggregators), std::move(num_tensors_per_agg),
      epsilon_per_agg, delta_per_agg, num_inputs);
}
REGISTER_AGGREGATOR_FACTORY(kDPTensorAggregatorBundleUri,
                            DPTensorAggregatorBundleFactory);

}  // namespace aggregation
}  // namespace tensorflow_federated
