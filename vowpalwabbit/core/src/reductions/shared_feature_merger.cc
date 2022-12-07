// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#include "vw/core/reductions/shared_feature_merger.h"

#include "vw/config/options.h"
#include "vw/core/cb.h"
#include "vw/core/example.h"
#include "vw/core/label_dictionary.h"
#include "vw/core/learner.h"
#include "vw/core/scope_exit.h"
#include "vw/core/setup_base.h"
#include "vw/core/vw.h"

#include <iterator>
#include <string>
#include <vector>
namespace
{
class sfm_metrics
{
public:
  size_t count_learn_example_with_shared = 0;
};

class sfm_data
{
public:
  std::unique_ptr<sfm_metrics> metrics;
  VW::label_type_t label_type = VW::label_type_t::CB;
  bool store_shared_ex_in_reduction_features = false;
};

template <bool is_learn>
void predict_or_learn(sfm_data& data, VW::LEARNER::multi_learner& base, VW::multi_ex& ec_seq)
{
  if (ec_seq.empty()) THROW("cb_adf: At least one action must be provided for an example to be valid.");

  VW::multi_ex::value_type shared_example = nullptr;
  const bool store_shared_ex_in_reduction_features = data.store_shared_ex_in_reduction_features;

  const bool has_example_header = VW::LEARNER::ec_is_example_header(*ec_seq[0], data.label_type);

  if (has_example_header)
  {
    shared_example = ec_seq[0];
    ec_seq.erase(ec_seq.begin());
    // merge sequences
    for (auto& example : ec_seq)
    {
      VW::details::append_example_namespaces_from_example(*example, *shared_example);
      if (store_shared_ex_in_reduction_features)
      {
        auto& red_features =
            example->ex_reduction_features.template get<VW::large_action_space::las_reduction_features>();
        red_features.shared_example = shared_example;
      }
    }

    std::swap(ec_seq[0]->pred, shared_example->pred);
    std::swap(ec_seq[0]->tag, shared_example->tag);
  }

  // Guard example state restore against throws
  auto restore_guard = VW::scope_exit(
      [has_example_header, &shared_example, &ec_seq, &store_shared_ex_in_reduction_features]
      {
        if (has_example_header)
        {
          for (auto& example : ec_seq)
          {
            VW::details::truncate_example_namespaces_from_example(*example, *shared_example);

            if (store_shared_ex_in_reduction_features)
            {
              auto& red_features =
                  example->ex_reduction_features.template get<VW::large_action_space::las_reduction_features>();
              red_features.reset_to_default();
            }
          }
          std::swap(shared_example->pred, ec_seq[0]->pred);
          std::swap(shared_example->tag, ec_seq[0]->tag);
          ec_seq.insert(ec_seq.begin(), shared_example);
        }
      });

  if (ec_seq.empty()) { return; }
  if (is_learn) { base.learn(ec_seq); }
  else { base.predict(ec_seq); }

  if (data.metrics)
  {
    VW_WARNING_STATE_PUSH
    VW_WARNING_DISABLE_COND_CONST_EXPR
    if (is_learn && has_example_header) { data.metrics->count_learn_example_with_shared++; }
    VW_WARNING_STATE_POP
  }
}

void persist(sfm_data& data, VW::metric_sink& metrics)
{
  if (data.metrics)
  {
    metrics.set_uint("sfm_count_learn_example_with_shared", data.metrics->count_learn_example_with_shared);
  }
}

struct options_shared_feature_merger_v1
{
  bool extra_metrics_supplied;
  bool large_action_space_supplied;
};

std::unique_ptr<options_shared_feature_merger_v1> get_shared_feature_merger_options_instance(
    const VW::workspace&, VW::io::logger&, VW::config::options_i& options)
{
  auto shared_feature_merger_opts = VW::make_unique<options_shared_feature_merger_v1>();
  shared_feature_merger_opts->extra_metrics_supplied = options.was_supplied("extra_metrics");
  shared_feature_merger_opts->large_action_space_supplied = options.was_supplied("large_action_space");
  return shared_feature_merger_opts;
}
}  // namespace

VW::LEARNER::base_learner* VW::reductions::shared_feature_merger_setup(VW::setup_base_i& stack_builder)
{
  VW::workspace& all = *stack_builder.get_all_pointer();
  auto shared_feature_merger_opts = get_shared_feature_merger_options_instance(all, all.logger, *stack_builder.get_options());
  if (shared_feature_merger_opts == nullptr) { return nullptr; }
  auto* base = stack_builder.setup_base_learner();
  if (base == nullptr) { return nullptr; }
  std::set<label_type_t> sfm_labels = {label_type_t::CB, label_type_t::CS};
  if (sfm_labels.find(base->get_input_label_type()) == sfm_labels.end() || !base->is_multiline()) { return base; }

  auto shared_feature_merger_data = VW::make_unique<sfm_data>();
  if (shared_feature_merger_opts->extra_metrics_supplied) { shared_feature_merger_data->metrics = VW::make_unique<sfm_metrics>(); }
  if (shared_feature_merger_opts->large_action_space_supplied) { shared_feature_merger_data->store_shared_ex_in_reduction_features = true; }

  auto* multi_base = VW::LEARNER::as_multiline(base);
  shared_feature_merger_data->label_type = all.example_parser->lbl_parser.label_type;

  // Both label and prediction types inherit that of base.
  auto* learner = VW::LEARNER::make_reduction_learner(std::move(shared_feature_merger_data), multi_base, predict_or_learn<true>,
      predict_or_learn<false>, stack_builder.get_setupfn_name(shared_feature_merger_setup))
                      .set_learn_returns_prediction(base->learn_returns_prediction)
                      .set_persist_metrics(persist)
                      .build();

  // TODO: Incorrect feature numbers will be reported without merging the example namespaces from the
  //       shared example in a finish_example function. However, its too expensive to perform the full operation.

  return VW::LEARNER::make_base(*learner);
}
