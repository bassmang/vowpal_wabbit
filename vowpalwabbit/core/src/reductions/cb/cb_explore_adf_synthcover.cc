// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#include "vw/core/reductions/cb/cb_explore_adf_synthcover.h"

#include "vw/config/options.h"
#include "vw/core/action_score.h"
#include "vw/core/gen_cs_example.h"
#include "vw/core/global_data.h"
#include "vw/core/label_parser.h"
#include "vw/core/numeric_casts.h"
#include "vw/core/parser.h"
#include "vw/core/rand48.h"
#include "vw/core/reductions/cb/cb_adf.h"
#include "vw/core/reductions/cb/cb_explore.h"
#include "vw/core/reductions/cb/cb_explore_adf_common.h"
#include "vw/core/setup_base.h"
#include "vw/core/version.h"
#include "vw/core/vw_versions.h"
#include "vw/explore/explore.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
using namespace VW::cb_explore_adf;

// All exploration algorithms return a vector of id, probability tuples, sorted in order of scores. The probabilities
// are the probability with which each action should be replaced to the top of the list.

namespace
{
class cb_explore_adf_synthcover
{
public:
  cb_explore_adf_synthcover(float epsilon, float psi, size_t synthcoversize,
      std::shared_ptr<VW::rand_state> random_state, VW::version_struct model_file_version);

  // Should be called through cb_explore_adf_base for pre/post-processing
  void predict(VW::LEARNER::multi_learner& base, VW::multi_ex& examples)
  {
    predict_or_learn_impl<false>(base, examples);
  }
  void learn(VW::LEARNER::multi_learner& base, VW::multi_ex& examples) { predict_or_learn_impl<true>(base, examples); }
  void save_load(io_buf& model_file, bool read, bool text);

private:
  float _epsilon;
  float _psi;
  size_t _synthcoversize;
  std::shared_ptr<VW::rand_state> _random_state;

  VW::version_struct _model_file_version;

  VW::v_array<VW::action_score> _action_probs;
  float _min_cost;
  float _max_cost;
  template <bool is_learn>
  void predict_or_learn_impl(VW::LEARNER::multi_learner& base, VW::multi_ex& examples);
};

cb_explore_adf_synthcover::cb_explore_adf_synthcover(float epsilon, float psi, size_t synthcoversize,
    std::shared_ptr<VW::rand_state> random_state, VW::version_struct model_file_version)
    : _epsilon(epsilon)
    , _psi(psi)
    , _synthcoversize(synthcoversize)
    , _random_state(std::move(random_state))
    , _model_file_version(model_file_version)
    , _min_cost(0.0)
    , _max_cost(0.0)
{
}

template <bool is_learn>
void cb_explore_adf_synthcover::predict_or_learn_impl(VW::LEARNER::multi_learner& base, VW::multi_ex& examples)
{
  VW::LEARNER::multiline_learn_or_predict<is_learn>(base, examples, examples[0]->ft_offset);

  const auto it =
      std::find_if(examples.begin(), examples.end(), [](VW::example* item) { return !item->l.cb.costs.empty(); });

  if (it != examples.end())
  {
    const CB::cb_class logged = (*it)->l.cb.costs[0];

    _min_cost = std::min(logged.cost, _min_cost);
    _max_cost = std::max(logged.cost, _max_cost);
  }

  VW::action_scores& preds = examples[0]->pred.a_s;
  uint32_t num_actions = static_cast<uint32_t>(examples.size());
  if (num_actions == 0)
  {
    preds.clear();
    return;
  }
  else if (num_actions == 1)
  {
    preds[0].score = 1;
    return;
  }

  for (size_t i = 0; i < num_actions; i++) { preds[i].score = VW::math::clamp(preds[i].score, _min_cost, _max_cost); }
  std::make_heap(preds.begin(), preds.end(), std::greater<VW::action_score>());

  _action_probs.clear();
  for (uint32_t i = 0; i < num_actions; i++) { _action_probs.push_back({i, 0.}); }

  for (uint32_t i = 0; i < _synthcoversize;)
  {
    std::pop_heap(preds.begin(), preds.end(), std::greater<VW::action_score>());
    auto minpred = preds.back();
    preds.pop_back();

    auto secondminpred = preds[0];
    for (; secondminpred.score >= minpred.score && i < _synthcoversize; i++)
    {
      _action_probs[minpred.action].score += 1.0f / _synthcoversize;
      minpred.score += (1.0f / _synthcoversize) * _psi;
    }

    preds.push_back(minpred);
    std::push_heap(preds.begin(), preds.end(), std::greater<VW::action_score>());
  }

  exploration::enforce_minimum_probability(_epsilon, true, begin_scores(_action_probs), end_scores(_action_probs));

  std::sort(_action_probs.begin(), _action_probs.end(), std::greater<VW::action_score>());

  for (size_t i = 0; i < num_actions; i++) { preds[i] = _action_probs[i]; }
}

void cb_explore_adf_synthcover::save_load(io_buf& model_file, bool read, bool text)
{
  if (model_file.num_files() == 0) { return; }
  if (!read || _model_file_version >= VW::version_definitions::VERSION_FILE_WITH_CCB_MULTI_SLOTS_SEEN_FLAG)
  {
    std::stringstream msg;
    if (!read) { msg << "_min_cost " << _min_cost << "\n"; }
    bin_text_read_write_fixed(model_file, reinterpret_cast<char*>(&_min_cost), sizeof(_min_cost), read, msg, text);

    if (!read) { msg << "_max_cost " << _max_cost << "\n"; }
    bin_text_read_write_fixed(model_file, reinterpret_cast<char*>(&_max_cost), sizeof(_max_cost), read, msg, text);
  }
}

struct options_cbea_synthcover_v1
{
  bool cb_explore_adf_option = false;
  float epsilon = 0.;
  uint64_t synthcoversize;
  bool use_synthcover = false;
  float psi;
  bool with_metrics;
};

std::unique_ptr<options_cbea_synthcover_v1> get_cbea_synthcover_options_instance(
    const VW::workspace&, VW::io::logger&, VW::config::options_i& options)
{
  auto cbea_synthcover_opts = VW::make_unique<options_cbea_synthcover_v1>();
  using VW::config::make_option;
  VW::config::option_group_definition new_options(
      "[Reduction] Contextual Bandit Exploration with ADF (synthetic cover)");
  new_options
      .add(make_option("cb_explore_adf", cbea_synthcover_opts->cb_explore_adf_option)
               .keep()
               .necessary()
               .help("Online explore-exploit for a contextual bandit problem with multiline action dependent features"))
      .add(make_option("epsilon", cbea_synthcover_opts->epsilon)
               .default_value(0.f)
               .keep()
               .allow_override()
               .help("Epsilon-greedy exploration"))
      .add(make_option("synthcover", cbea_synthcover_opts->use_synthcover)
               .keep()
               .necessary()
               .help("Use synthetic cover exploration"))
      .add(make_option("synthcoverpsi", cbea_synthcover_opts->psi)
               .keep()
               .default_value(0.1f)
               .allow_override()
               .help("Exploration reward bonus"))
      .add(make_option("synthcoversize", cbea_synthcover_opts->synthcoversize)
               .keep()
               .default_value(100)
               .allow_override()
               .help("Number of policies in cover"));

  if (!options.add_parse_and_check_necessary(new_options)) { return nullptr; }

  // Ensure serialization of cb_adf in all cases.
  if (!options.was_supplied("cb_adf")) { options.insert("cb_adf", ""); }
  cbea_synthcover_opts->with_metrics = options.was_supplied("extra_metrics");

  return cbea_synthcover_opts;
}
}  // namespace

VW::LEARNER::base_learner* VW::reductions::cb_explore_adf_synthcover_setup(VW::setup_base_i& stack_builder)
{
  VW::workspace& all = *stack_builder.get_all_pointer();
  auto cbea_synthcover_opts = get_cbea_synthcover_options_instance(all, all.logger, *stack_builder.get_options());
  if (cbea_synthcover_opts == nullptr) { return nullptr; }

  if (cbea_synthcover_opts->synthcoversize <= 0) { THROW("synthcoversize must be >= 1"); }
  if (cbea_synthcover_opts->epsilon < 0) { THROW("epsilon must be non-negative"); }
  if (cbea_synthcover_opts->psi <= 0) { THROW("synthcoverpsi must be positive"); }

  if (!all.quiet)
  {
    *(all.trace_message) << "Using synthcover for CB exploration" << std::endl;
    *(all.trace_message) << "synthcoversize = " << cbea_synthcover_opts->synthcoversize << std::endl;
    if (cbea_synthcover_opts->epsilon > 0)
    {
      *(all.trace_message) << "epsilon = " << cbea_synthcover_opts->epsilon << std::endl;
    }
    *(all.trace_message) << "synthcoverpsi = " << cbea_synthcover_opts->psi << std::endl;
  }

  size_t problem_multiplier = 1;
  VW::LEARNER::multi_learner* base = as_multiline(stack_builder.setup_base_learner());
  all.example_parser->lbl_parser = CB::cb_label;

  using explore_type = cb_explore_adf_base<cb_explore_adf_synthcover>;
  auto cbea_synthcover_data =
      VW::make_unique<explore_type>(cbea_synthcover_opts->with_metrics, cbea_synthcover_opts->epsilon,
          cbea_synthcover_opts->psi, VW::cast_to_smaller_type<size_t>(cbea_synthcover_opts->synthcoversize),
          all.get_random_state(), all.model_file_ver);
  auto* l = make_reduction_learner(std::move(cbea_synthcover_data), base, explore_type::learn, explore_type::predict,
      stack_builder.get_setupfn_name(cb_explore_adf_synthcover_setup))
                .set_input_label_type(VW::label_type_t::CB)
                .set_output_label_type(VW::label_type_t::CB)
                .set_input_prediction_type(VW::prediction_type_t::ACTION_SCORES)
                .set_output_prediction_type(VW::prediction_type_t::ACTION_PROBS)
                .set_params_per_weight(problem_multiplier)
                .set_finish_example(explore_type::finish_multiline_example)
                .set_print_example(explore_type::print_multiline_example)
                .set_save_load(explore_type::save_load)
                .set_persist_metrics(explore_type::persist_metrics)
                .build(&all.logger);
  return make_base(*l);
}
