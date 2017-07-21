#include <algorithm>

#include "TreeInfo.hpp"
#include "ParallelContext.hpp"

using namespace std;

TreeInfo::TreeInfo (const Options &opts, const Tree& tree, const PartitionedMSA& parted_msa,
                    const PartitionAssignment& part_assign)
{
  init(opts, tree, parted_msa, part_assign, std::vector<uintVector>());
}

TreeInfo::TreeInfo (const Options &opts, const Tree& tree, const PartitionedMSA& parted_msa,
                    const PartitionAssignment& part_assign,
                    const std::vector<uintVector>& site_weights)
{
  init(opts, tree, parted_msa, part_assign, site_weights);
}

void TreeInfo::init(const Options &opts, const Tree& tree, const PartitionedMSA& parted_msa,
                    const PartitionAssignment& part_assign,
                    const std::vector<uintVector>& site_weights)
{
  _pll_treeinfo = pllmod_treeinfo_create(pll_utree_graph_clone(&tree.pll_utree_root()),
                                         tree.num_tips(),
                                         parted_msa.part_count(), opts.brlen_linkage);

  if (!_pll_treeinfo)
    throw runtime_error("ERROR creating treeinfo structure: " + string(pll_errmsg));

  if (ParallelContext::num_procs() > 1)
  {
    pllmod_treeinfo_set_parallel_context(_pll_treeinfo, (void *) nullptr,
                                         ParallelContext::parallel_reduce_cb);
  }

  // init partitions
  int optimize_branches = opts.optimize_brlen ? PLLMOD_OPT_PARAM_BRANCHES_ITERATIVE : 0;
  if (opts.optimize_model && opts.brlen_linkage == PLLMOD_TREE_BRLEN_SCALED &&
      parted_msa.part_count() > 1)
  {
    optimize_branches |= PLLMOD_OPT_PARAM_BRANCH_LEN_SCALER;
  }

  for (size_t p = 0; p < parted_msa.part_count(); ++p)
  {
    const PartitionInfo& pinfo = parted_msa.part_info(p);
    int params_to_optimize = opts.optimize_model ? pinfo.model().params_to_optimize() : 0;
    params_to_optimize |= optimize_branches;

    PartitionAssignment::const_iterator part_range = part_assign.find(p);
    if (part_range != part_assign.end())
    {
      /* create and init PLL partition structure */
      const auto& weights = site_weights.empty() ? pinfo.msa().weights() : site_weights.at(p);
      pll_partition_t * partition = create_pll_partition(opts, pinfo, *part_range, weights);

      int retval = pllmod_treeinfo_init_partition(_pll_treeinfo, p, partition,
                                                  params_to_optimize,
                                                  pinfo.model().gamma_mode(),
                                                  pinfo.model().alpha(),
                                                  pinfo.model().ratecat_submodels().data(),
                                                  pinfo.model().submodel(0).rate_sym().data());

      if (!retval)
        throw runtime_error("ERROR adding treeinfo partition: " + string(pll_errmsg));

      if (part_range->master())
        _parts_master.insert(p);
    }
    else
    {
      // this partition will be processed by other threads, but we still need to know
      // which parameters to optimize
      _pll_treeinfo->params_to_optimize[p] = params_to_optimize;
    }
  }
}

TreeInfo::~TreeInfo ()
{
  if (_pll_treeinfo)
  {
    for (unsigned int i = 0; i < _pll_treeinfo->partition_count; ++i)
    {
      if (_pll_treeinfo->partitions[i])
        pll_partition_destroy(_pll_treeinfo->partitions[i]);
    }

    pll_utree_graph_destroy(_pll_treeinfo->root, NULL);
    pllmod_treeinfo_destroy(_pll_treeinfo);
  }
}

Tree TreeInfo::tree() const
{
  return _pll_treeinfo ? Tree(_pll_treeinfo->tip_count, *_pll_treeinfo->root) : Tree();
}

double TreeInfo::loglh(bool incremental)
{
  return pllmod_treeinfo_compute_loglh(_pll_treeinfo, incremental ? 1 : 0);
}

void TreeInfo::model(size_t partition_id, const Model& model)
{
  if (partition_id >= _pll_treeinfo->partition_count)
    throw out_of_range("Partition ID out of range");

  if (!_pll_treeinfo->partitions[partition_id])
    return;

  assign(_pll_treeinfo->partitions[partition_id], model);
  _pll_treeinfo->alphas[partition_id] = model.alpha();
  if (_pll_treeinfo->brlen_scalers)
    _pll_treeinfo->brlen_scalers[partition_id] = model.brlen_scaler();
}

//#define DBG printf

double TreeInfo::optimize_branches(double lh_epsilon, double brlen_smooth_factor)
{
  /* update all CLVs and p-matrices before calling BLO */
  double new_loglh = loglh();

  if (_pll_treeinfo->params_to_optimize[0] & PLLMOD_OPT_PARAM_BRANCHES_ITERATIVE)
  {
    new_loglh = -1 * pllmod_opt_optimize_branch_lengths_local_multi(_pll_treeinfo->partitions,
                                                                    _pll_treeinfo->partition_count,
                                                                    _pll_treeinfo->root,
                                                                    _pll_treeinfo->param_indices,
                                                                    _pll_treeinfo->deriv_precomp,
                                                                    _pll_treeinfo->brlen_scalers,
                                                                    RAXML_BRLEN_MIN,
                                                                    RAXML_BRLEN_MAX,
                                                                    lh_epsilon,
                                                                    brlen_smooth_factor * RAXML_BRLEN_SMOOTHINGS,
                                                                    -1,  /* radius */
                                                                    1,    /* keep_update */
                                                                    _pll_treeinfo->parallel_context,
                                                                    _pll_treeinfo->parallel_reduce_cb
                                                                    );

    LOG_DEBUG << "\t - after brlen: logLH = " << new_loglh << endl;

    if (pll_errno)
      throw runtime_error("ERROR in branch lenght optimization: " + string(pll_errmsg));
  }

  /* optimize brlen scalers, if needed */
  if (_pll_treeinfo->brlen_linkage == PLLMOD_TREE_BRLEN_SCALED &&
      _pll_treeinfo->partition_count > 1)
  {
    new_loglh = -1 * pllmod_algo_opt_onedim_treeinfo(_pll_treeinfo,
                                                    PLLMOD_OPT_PARAM_BRANCH_LEN_SCALER,
                                                    RAXML_BRLEN_SCALER_MIN,
                                                    RAXML_BRLEN_SCALER_MAX,
                                                    RAXML_PARAM_EPSILON);

    /* normalize scalers and scale the branches accordingly */
    pllmod_treeinfo_normalize_brlen_scalers(_pll_treeinfo);

    LOG_DEBUG << "\t - after brlen scalers: logLH = " << new_loglh << endl;
  }


  return new_loglh;
}

double TreeInfo::optimize_params(int params_to_optimize, double lh_epsilon)
{
  double new_loglh;

  /* optimize SUBSTITUTION RATES */
  if (params_to_optimize & PLLMOD_OPT_PARAM_SUBST_RATES)
  {
    new_loglh = -1 * pllmod_algo_opt_subst_rates_treeinfo(_pll_treeinfo,
                                                          0,
                                                          PLLMOD_OPT_MIN_SUBST_RATE,
                                                          PLLMOD_OPT_MAX_SUBST_RATE,
                                                          RAXML_BFGS_FACTOR,
                                                          RAXML_PARAM_EPSILON);

    LOG_DEBUG << "\t - after rates: logLH = " << new_loglh << endl;
  }

  /* optimize BASE FREQS */
  if (params_to_optimize & PLLMOD_OPT_PARAM_FREQUENCIES)
  {
    new_loglh = -1 * pllmod_algo_opt_frequencies_treeinfo(_pll_treeinfo,
                                                          0,
                                                          PLLMOD_OPT_MIN_FREQ,
                                                          PLLMOD_OPT_MAX_FREQ,
                                                          RAXML_BFGS_FACTOR,
                                                          RAXML_PARAM_EPSILON);

    LOG_DEBUG << "\t - after freqs: logLH = " << new_loglh << endl;
  }

  /* optimize ALPHA */
  if (params_to_optimize & PLLMOD_OPT_PARAM_ALPHA)
  {
    new_loglh = -1 * pllmod_algo_opt_onedim_treeinfo(_pll_treeinfo,
                                                      PLLMOD_OPT_PARAM_ALPHA,
                                                      PLLMOD_OPT_MIN_ALPHA,
                                                      PLLMOD_OPT_MAX_ALPHA,
                                                      RAXML_PARAM_EPSILON);

   LOG_DEBUG << "\t - after alpha: logLH = " << new_loglh << endl;
  }

  if (params_to_optimize & PLLMOD_OPT_PARAM_PINV)
  {
    new_loglh = -1 * pllmod_algo_opt_onedim_treeinfo(_pll_treeinfo,
                                                      PLLMOD_OPT_PARAM_PINV,
                                                      PLLMOD_OPT_MIN_PINV,
                                                      PLLMOD_OPT_MAX_PINV,
                                                      RAXML_PARAM_EPSILON);

    LOG_DEBUG << "\t - after p-inv: logLH = " << new_loglh << endl;
  }

  /* optimize FREE RATES and WEIGHTS */
  if (params_to_optimize & PLLMOD_OPT_PARAM_FREE_RATES)
  {
    new_loglh = -1 * pllmod_algo_opt_rates_weights_treeinfo (_pll_treeinfo,
                                                          RAXML_FREERATE_MIN,
                                                          RAXML_FREERATE_MAX,
                                                          RAXML_BFGS_FACTOR,
                                                          RAXML_PARAM_EPSILON);

    /* normalize scalers and scale the branches accordingly */
    if (_pll_treeinfo->brlen_linkage == PLLMOD_TREE_BRLEN_SCALED &&
        _pll_treeinfo->partition_count > 1)
      pllmod_treeinfo_normalize_brlen_scalers(_pll_treeinfo);

    LOG_DEBUG << "\t - after freeR: logLH = " << new_loglh << endl;
//    LOG_DEBUG << "\t - after freeR/crosscheck: logLH = " << loglh() << endl;
  }

  if (params_to_optimize & PLLMOD_OPT_PARAM_BRANCHES_ITERATIVE)
  {
    new_loglh = optimize_branches(lh_epsilon, 0.25);
  }

  return new_loglh;
}

double TreeInfo::spr_round(spr_round_params& params)
{
  double loglh = pllmod_algo_spr_round(_pll_treeinfo, params.radius_min, params.radius_max,
                               params.ntopol_keep, params.thorough,
                               RAXML_BRLEN_MIN, RAXML_BRLEN_MAX, RAXML_BRLEN_SMOOTHINGS,
                               0.1,
                               params.subtree_cutoff > 0. ? &params.cutoff_info : nullptr,
                               params.subtree_cutoff);

  if (loglh)
    return loglh;
  else
    throw runtime_error("ERROR in SPR round: " + string(pll_errmsg));
}


void assign(PartitionedMSA& parted_msa, const TreeInfo& treeinfo)
{
  const pllmod_treeinfo_t& pll_treeinfo = treeinfo.pll_treeinfo();

  if (parted_msa.part_count() != pll_treeinfo.partition_count)
    throw runtime_error("Incompatible arguments");

  for (size_t p = 0; p < parted_msa.part_count(); ++p)
  {
    if (!pll_treeinfo.partitions[p])
      continue;

    Model model(parted_msa.model(p));
    assign(model, treeinfo, p);
    parted_msa.model(p, move(model));
  }
}

void assign(Model& model, const TreeInfo& treeinfo, size_t partition_id)
{
  const pllmod_treeinfo_t& pll_treeinfo = treeinfo.pll_treeinfo();

  if (partition_id >= pll_treeinfo.partition_count)
    throw out_of_range("Partition ID out of range");

  if (!pll_treeinfo.partitions[partition_id])
    return;

  assign(model, pll_treeinfo.partitions[partition_id]);
  model.alpha(pll_treeinfo.alphas[partition_id]);
  if (pll_treeinfo.brlen_scalers)
    model.brlen_scaler(pll_treeinfo.brlen_scalers[partition_id]);
}

void build_clv(ProbVector::const_iterator probs, size_t sites, WeightVector::const_iterator weights,
               pll_partition_t* partition, bool normalize, std::vector<double>& clv)
{
  const auto states = partition->states;
  auto clvp = clv.begin();

  for (size_t i = 0; i < sites; ++i)
  {
    if (weights[i] > 0)
    {
      double sum = 0.;
      for (size_t j = 0; j < states; ++j)
        sum += probs[j];

      for (size_t j = 0; j < states; ++j)
      {
        if (sum > 0.)
          clvp[j] =  normalize ? probs[j] / sum : probs[j];
        else
          clvp[j] = 1.0;
      }

      clvp += states;
    }

    /* NB: clv has to be padded, but msa arrays are not! */
    probs += states;
  }

  assert(clvp == clv.end());
}

void set_partition_tips(const Options& opts, const MSA& msa, const PartitionRange& part_region,
                        pll_partition_t* partition, const unsigned int * charmap)
{
  /* set pattern weights */
  if (!msa.weights().empty())
    pll_set_pattern_weights(partition, msa.weights().data() + part_region.start);

  if (opts.use_prob_msa && msa.probabilistic())
  {
    assert(!(partition->attributes & PLL_ATTRIB_PATTERN_TIP));
    assert(partition->states == msa.states());

    auto normalize = !msa.normalized();
    auto weights_start = msa.weights().cbegin() + part_region.start;

    // we need a libpll function for that!
    auto clv_size = partition->sites * partition->states;
    std::vector<double> tmp_clv(clv_size);
    for (size_t i = 0; i < msa.size(); ++i)
    {
      auto prob_start = msa.probs(i, part_region.start);
      build_clv(prob_start, partition->sites, weights_start, partition, normalize, tmp_clv);
      pll_set_tip_clv(partition, i, tmp_clv.data(), PLL_FALSE);
    }
  }
  else
  {
    for (size_t i = 0; i < msa.size(); ++i)
    {
      pll_set_tip_states(partition, i, charmap, msa.at(i).c_str() + part_region.start);
    }
  }
}

void set_partition_tips(const Options& opts, const MSA& msa, const PartitionRange& part_region,
                        pll_partition_t* partition, const unsigned int * charmap,
                        const WeightVector& weights)
{
  assert(!weights.empty());

  const auto pstart = part_region.start;
  const auto pend = part_region.start + part_region.length;

  /* compress weights array by removing all zero entries */
  uintVector comp_weights;
  for (size_t j = pstart; j < pend; ++j)
  {
    if (weights[j] > 0)
      comp_weights.push_back(weights[j]);
  }

  /* now set tip sequences, ignoring all columns with zero weights */
  if (opts.use_prob_msa && msa.probabilistic())
  {
    assert(!(partition->attributes & PLL_ATTRIB_PATTERN_TIP));
    assert(partition->states == msa.states());

    auto normalize = !msa.normalized();
    auto weights_start = msa.weights().cbegin() + part_region.start;

    // we need a libpll function for that!
    auto clv_size = part_region.length * partition->states;
    std::vector<double> tmp_clv(clv_size);
    for (size_t i = 0; i < msa.size(); ++i)
    {
      auto prob_start = msa.probs(i, part_region.start);
      build_clv(prob_start, part_region.length, weights_start, partition, normalize, tmp_clv);
      pll_set_tip_clv(partition, i, tmp_clv.data(), PLL_FALSE);
    }
  }
  else
  {
    std::vector<char> bs_seq(part_region.length);
    for (size_t i = 0; i < msa.size(); ++i)
    {
      const char * full_seq = msa.at(i).c_str();
      size_t pos = 0;
      for (size_t j = pstart; j < pend; ++j)
      {
        if (weights[j] > 0)
          bs_seq[pos++] = full_seq[j];
      }
      assert(pos == comp_weights.size());

      pll_set_tip_states(partition, i, charmap, bs_seq.data());
    }
  }

  pll_set_pattern_weights(partition, comp_weights.data());
}

pll_partition_t* create_pll_partition(const Options& opts, const PartitionInfo& pinfo,
                                      const PartitionRange& part_region, const uintVector& weights)
{
  const MSA& msa = pinfo.msa();
  const Model& model = pinfo.model();

  unsigned int attrs = opts.simd_arch;

  if (opts.use_rate_scalers && model.num_ratecats() > 1)
  {
    attrs |= PLL_ATTRIB_RATE_SCALERS;

    if (model.num_states() != 4)
    {
      throw runtime_error("Per-rate scalers are implemented for DNA data only!\n");
    }
  }

  if (opts.use_tip_inner)
  {
    assert(!(opts.use_prob_msa));
    // TODO: use proper auto-tuning
    const unsigned long min_len_ti = 100;
    if ((unsigned long) msa.length() > min_len_ti)
      attrs |= PLL_ATTRIB_PATTERN_TIP;
  }
  if (opts.use_repeats)
  {
    if (opts.use_tip_inner)
    {
      throw runtime_error("Repeats and tip-inner optimizations are incompatible\n");
    }
    const unsigned long min_len_ti = 16;
    if ((unsigned long) msa.length() > min_len_ti)
      attrs |= PLL_ATTRIB_SITE_REPEATS;
  }

  // NOTE: if partition is split among multiple threads, asc. bias correction must be applied only once!
  if (model.ascbias_type() == AscBiasCorrection::lewis ||
      (model.ascbias_type() != AscBiasCorrection::none && part_region.master()))
  {
    attrs |=  PLL_ATTRIB_AB_FLAG;
    attrs |= (unsigned int) model.ascbias_type();
  }

  /* part_length doesn't include columns with zero weight */
  const size_t part_length = weights.empty() ? part_region.length :
                             std::count_if(weights.begin() + part_region.start,
                                           weights.begin() + part_region.start + part_region.length,
                                           [](uintVector::value_type w) -> bool
                                             { return w > 0; }
                                           );

  BasicTree tree(msa.size());
  pll_partition_t * partition = pll_partition_create(
      tree.num_tips(),         /* number of tip sequences */
      tree.num_inner(),        /* number of CLV buffers */
      model.num_states(),      /* number of states in the data */
      part_length,             /* number of alignment sites/patterns */
      model.num_submodels(),   /* number of different substitution models (LG4 = 4) */
      tree.num_branches(),     /* number of probability matrices */
      model.num_ratecats(),    /* number of (GAMMA) rate categories */
      tree.num_inner(),        /* number of scaling buffers */
      attrs                    /* list of flags (SSE3/AVX, TIP-INNER special cases etc.) */
  );

  if (!partition)
    throw runtime_error("ERROR creating pll_partition: " + string(pll_errmsg));

  if (part_region.master() && !model.ascbias_weights().empty())
    pll_set_asc_state_weights(partition, model.ascbias_weights().data());

  if (part_length == part_region.length)
    set_partition_tips(opts, msa, part_region, partition, model.charmap());
  else
    set_partition_tips(opts, msa, part_region, partition, model.charmap(), weights);

  assign(partition, model);

  return partition;
}


