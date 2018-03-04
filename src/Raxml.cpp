/*
    Copyright (C) 2017 Alexey Kozlov, Alexandros Stamatakis, Diego Darriba, Tomas Flouri

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Contact: Alexey Kozlov <Alexey.Kozlov@h-its.org>,
    Heidelberg Institute for Theoretical Studies,
    Schloss-Wolfsbrunnenweg 35, D-69118 Heidelberg, Germany
*/
#include <algorithm>
#include <chrono>

#include <memory>

#include "version.h"
#include "common.h"
#include "MSA.hpp"
#include "Options.hpp"
#include "CommandLineParser.hpp"
#include "Optimizer.hpp"
#include "PartitionInfo.hpp"
#include "TreeInfo.hpp"
#include "io/file_io.hpp"
#include "io/binary_io.hpp"
#include "ParallelContext.hpp"
#include "LoadBalancer.hpp"
#include "bootstrap/BootstrapGenerator.hpp"

#ifdef _RAXML_TERRAPHAST
#include "terraces/TerraceWrapper.hpp"
#endif

using namespace std;

typedef vector<Tree> TreeList;
struct RaxmlInstance
{
  Options opts;
  PartitionedMSA parted_msa;
  unique_ptr<PartitionedMSA> parted_msa_parsimony;
  TreeList start_trees;
  BootstrapReplicateList bs_reps;
  PartitionAssignmentList proc_part_assign;
  unique_ptr<LoadBalancer> load_balancer;
  unique_ptr<BootstrapTree> bs_tree;

 // unique_ptr<TerraceWrapper> terrace_wrapper;

//  unique_ptr<RandomGenerator> starttree_seed_gen;
//  unique_ptr<RandomGenerator> bootstrap_seed_gen;

  unique_ptr<NewickStream> start_tree_stream;

  /* this is just a dummy random tree used for convenience, e,g, if we need tip labels or
   * just 'any' valid tree for the alignment at hand */
  Tree random_tree;
};

void print_banner()
{
  LOG_INFO << endl << "RAxML-NG v. " << RAXML_VERSION << " released on " << RAXML_DATE <<
      " by The Exelixis Lab." << endl;
  LOG_INFO << "Authors: Alexey Kozlov, Alexandros Stamatakis, Diego Darriba, "
              "Tomas Flouri, Benoit Morel." << endl;
  LOG_INFO << "Latest version: https://github.com/amkozlov/raxml-ng" << endl;
  LOG_INFO << "Questions/problems/suggestions? "
              "Please visit: https://groups.google.com/forum/#!forum/raxml" << endl;
  LOG_INFO << endl << "WARNING: This is a BETA release, please use at your own risk!" << endl << endl;
}

void init_part_info(RaxmlInstance& instance)
{
  auto& opts = instance.opts;
  auto& parted_msa = instance.parted_msa;

  if (!sysutil_file_exists(opts.msa_file))
  {
    throw runtime_error("Alignment file not found: " + opts.msa_file);
  }

  /* check if we have a binary input file */
  if (opts.msa_format == FileFormat::binary ||
      (opts.msa_format == FileFormat::autodetect && RBAStream::rba_file(opts.msa_file)))
  {
    LOG_INFO_TS << "Loading binary alignment from file: " << opts.msa_file << endl;

    RBAStream bs(opts.msa_file);
    bs >> parted_msa;

    // binary probMSAs are not supported yet
    instance.opts.use_prob_msa = false;

    LOG_INFO_TS << "Alignment comprises " << parted_msa.taxon_count() << " taxa, " <<
        parted_msa.part_count() << " partitions and " <<
        parted_msa.total_length() << " patterns\n" << endl;

    LOG_INFO << parted_msa;

    LOG_INFO << endl;
  }
  /* check if model is a file */
  else if (sysutil_file_exists(opts.model_file))
  {
    // read partition definitions from file
    RaxmlPartitionStream partfile(opts.model_file, ios::in);
    partfile >> parted_msa;

//    DBG("partitions found: %d\n", useropt->part_count);
  }
  else if (!opts.model_file.empty())
  {
    // create and init single pseudo-partition
    parted_msa.emplace_part_info("noname", opts.data_type, opts.model_file);
  }
  else
    throw runtime_error("Please specify an evolutionary model with --model switch");

  /* make sure that linked branch length mode is set for unpartitioned alignments */
  if (parted_msa.part_count() == 1)
    opts.brlen_linkage = PLLMOD_COMMON_BRLEN_LINKED;

  /* in the scaled brlen mode, use ML optimization of brlen scalers by default */
  if (opts.brlen_linkage == PLLMOD_COMMON_BRLEN_SCALED)
  {
    for (auto& pinfo: parted_msa.part_list())
      pinfo.model().set_param_mode_default(PLLMOD_OPT_PARAM_BRANCH_LEN_SCALER, ParamValue::ML);
  }

  int freerate_count = 0;

  for (const auto& pinfo: parted_msa.part_list())
  {
    LOG_DEBUG << "|" << pinfo.name() << "|   |" << pinfo.model().to_string() << "|   |" <<
        pinfo.range_string() << "|" << endl;

    if (pinfo.model().ratehet_mode() == PLLMOD_UTIL_MIXTYPE_FREE)
      freerate_count++;
  }

  if (parted_msa.part_count() > 1 && freerate_count > 0 &&
      opts.brlen_linkage == PLLMOD_COMMON_BRLEN_LINKED)
  {
    throw runtime_error("LG4X and FreeRate models are not supported in linked branch length mode.\n"
        "Please use the '--brlen scaled' option to switch into proportional branch length mode.");
  }
}

void check_msa(RaxmlInstance& instance)
{
  LOG_VERB_TS << "Checking the alignment...\n";

  const auto& full_msa = instance.parted_msa.full_msa();
  const auto pll_msa = full_msa.pll_msa();
  const auto taxon_count = instance.parted_msa.taxon_count();

  if (taxon_count < 4)
  {
    throw runtime_error("Your alignment contains less than 4 sequences!");
  }

  unsigned long stats_mask = PLLMOD_MSA_STATS_DUP_TAXA | PLLMOD_MSA_STATS_DUP_SEQS;

  pllmod_msa_stats_t * stats = pllmod_msa_compute_stats(pll_msa,
                                                        4,
                                                        pll_map_nt, // map is not used here
                                                        NULL,
                                                        stats_mask);

  libpll_check_error("ERROR computing MSA stats");
  assert(stats);

  if (stats->dup_taxa_pairs_count > 0)
  {
    LOG_ERROR << "\nERROR: Duplicate sequence names found: " << stats->dup_taxa_pairs_count << endl;
    for (unsigned long c = 0; c < stats->dup_taxa_pairs_count; ++c)
    {
      const unsigned long idx1 = stats->dup_taxa_pairs[c*2];
      const unsigned long idx2 = stats->dup_taxa_pairs[c*2+1];
      LOG_ERROR << "ERROR: Sequences " << idx1 << " and " << idx2 << " have identical name: " <<
          pll_msa->label[idx1] << endl;
    }
    throw runtime_error("Please fix your alignment!");
  }

  if (stats->dup_seqs_pairs_count > 0)
  {
    LOG_WARN << "\nWARNING: Duplicate sequences found: " << stats->dup_seqs_pairs_count << endl;
    for (unsigned long c = 0; c < stats->dup_seqs_pairs_count; ++c)
    {
      const unsigned long idx1 = stats->dup_seqs_pairs[c*2];
      const unsigned long idx2 = stats->dup_seqs_pairs[c*2+1];
      LOG_WARN << "WARNING: Sequences " << pll_msa->label[idx1] << " and " <<
          pll_msa->label[idx2] << " are exactly identical!" << endl;
    }
  }

  pllmod_msa_destroy_stats(stats);

  std::set<size_t> gap_seqs;
  size_t total_gap_cols = 0;
  size_t part_num = 0;
  for (auto& pinfo: instance.parted_msa.part_list())
  {
    stats_mask = PLLMOD_MSA_STATS_GAP_SEQS | PLLMOD_MSA_STATS_GAP_COLS;

    pllmod_msa_stats_t * stats = pinfo.compute_stats(stats_mask);

    if (stats->gap_cols_count > 0)
    {
      total_gap_cols += stats->gap_cols_count;
      std::vector<size_t> gap_cols(stats->gap_cols, stats->gap_cols + stats->gap_cols_count);
      pinfo.msa().remove_sites(gap_cols);
    }

    std::set<size_t> cur_gap_seq(stats->gap_seqs, stats->gap_seqs + stats->gap_seqs_count);

    if (!part_num)
    {
      gap_seqs = cur_gap_seq;
    }
    else
    {
      for(auto it = gap_seqs.begin(); it != gap_seqs.end();)
      {
        if(cur_gap_seq.find(*it) == cur_gap_seq.end())
          it = gap_seqs.erase(it);
        else
          ++it;
      }
    }

    pllmod_msa_destroy_stats(stats);

    part_num++;
  }

  if (total_gap_cols > 0)
  {
    LOG_WARN << "\nWARNING: Fully undetermined columns found: " << total_gap_cols << endl;
//    for (unsigned long c = 0; c < stats->gap_cols_count; ++c)
//      LOG_VERB << "WARNING: Column " << stats->gap_cols[c]+1 << " contains only gaps!" << endl;
  }

  if (!gap_seqs.empty())
  {
   LOG_WARN << "\nWARNING: Fully undetermined sequences found: " << gap_seqs.size() << endl;
   for (auto c : gap_seqs)
     LOG_VERB << "WARNING: Sequence " << c << " " << pll_msa->label[c] << " contains only gaps!" << endl;
  }


  if (total_gap_cols > 0 || !gap_seqs.empty())
  {
    // save reduced MSA and partition files
    auto reduced_msa_fname = instance.opts.output_fname("reduced.phy");
    PhylipStream ps(reduced_msa_fname);

    ps << instance.parted_msa;

    LOG_INFO << "\nNOTE: Reduced alignment (with gap-only columns removed) was printed to:\n";
    LOG_INFO << sysutil_realpath(reduced_msa_fname) << endl;

    // save reduced partition file
    if (sysutil_file_exists(instance.opts.model_file))
    {
      auto reduced_part_fname = instance.opts.output_fname("reduced.partition");
      RaxmlPartitionStream ps(reduced_part_fname, ios::out);

      ps << instance.parted_msa;

      LOG_INFO << "\nNOTE: The corresponding reduced partition file was printed to:\n";
      LOG_INFO << sysutil_realpath(reduced_part_fname) << endl;
    }
  }

  if (taxon_count > RAXML_RATESCALERS_TAXA && !instance.opts.use_rate_scalers)
  {
    LOG_INFO << "\nNOTE: Per-rate scalers were automatically enabled to prevent numerical issues "
        "on taxa-rich alignments.\n";
    LOG_INFO << "NOTE: You can use --force switch to skip this check and fall back to per-site scalers.\n";
    instance.opts.use_rate_scalers = true;
  }

}

void check_models(const RaxmlInstance& instance)
{
  for (const auto& pinfo: instance.parted_msa.part_list())
  {
    auto stats = pinfo.stats();
    auto model = pinfo.model();

    // check for non-recommended model combinations
    if ((model.name() == "LG4X" || model.name() == "LG4M") &&
        model.param_mode(PLLMOD_OPT_PARAM_FREQUENCIES) != ParamValue::model)
    {
      throw runtime_error("Partition \"" + pinfo.name() +
                          "\": You specified LG4M or LG4X model with shared stationary based frequencies (" +
                          model.to_string(false) + ").\n"
                          "Please be warned, that this is against the idea of LG4 models and hence it's not recommended!" + "\n"
                          "If you know what you're doing, you can add --force command line switch to disable this safety check.");
    }

    // check for zero state frequencies
    if (model.param_mode(PLLMOD_OPT_PARAM_FREQUENCIES) == ParamValue::empirical)
    {
      const auto& freqs = stats.emp_base_freqs;
      for (unsigned int i = 0; i < freqs.size(); ++i)
      {
        if (!(freqs[i] > 0.))
        {
          LOG_ERROR << "\nBase frequencies: ";
          for (unsigned int j = 0; j < freqs.size(); ++j)
            LOG_ERROR << freqs[j] <<  " ";
          LOG_ERROR << endl;

          throw runtime_error("Frequency of state " + to_string(i) +
                              " in partition " + pinfo.name() + " is 0!\n"
                              "Please either change your partitioning scheme or "
                              "use model state frequencies for this partition!");
        }
      }
    }

    // check partitions which contain invariant sites and have ascertainment bias enabled
    if (model.ascbias_type() != AscBiasCorrection::none && stats.inv_count > 0)
    {
      throw runtime_error("You enabled ascertainment bias correction for partition " +
                           pinfo.name() + ", but it contains " +
                           to_string(stats.inv_count) + " invariant sites.\n"
                          "This is not allowed! Please either remove invariant sites or "
                          "disable ascertainment bias correction.");
    }
  }
}

void check_tree(const PartitionedMSA& msa, const Tree& tree)
{
  auto missing_taxa = 0;
  auto duplicate_taxa = 0;

  if (msa.taxon_count() > tree.num_tips())
    throw runtime_error("Alignment file contains more sequences than expected");
  else if (msa.taxon_count() != tree.num_tips())
    throw runtime_error("Some taxa are missing from the alignment file");

  unordered_set<string> tree_labels;
  unordered_set<string> msa_labels(msa.taxon_names().cbegin(), msa.taxon_names().cend());

  for (const auto& tip: tree.tip_labels())
  {
    if (!tree_labels.insert(tip.second).second)
    {
      LOG_ERROR << "ERROR: Taxon name appears more than once in the tree: " << tip.second << endl;
      duplicate_taxa++;
    }

    if (msa_labels.count(tip.second) == 0)
    {
      LOG_ERROR << "ERROR: Taxon name not found in the alignment: " << tip.second << endl;
      missing_taxa++;
    }
  }

  if (duplicate_taxa > 0)
    throw runtime_error("Tree contains duplicate taxon names (see above)!");

  if (missing_taxa > 0)
    throw runtime_error("Please check that sequence labels in the alignment and in the tree file are identical!");

  /* check for negative branch length */
  for (const auto& branch: tree.topology())
  {
    if (branch.length < 0.)
      throw runtime_error("Tree file contains negative branch lengths!");
  }
}

void load_msa(RaxmlInstance& instance)
{
  const auto& opts = instance.opts;
  auto& parted_msa = instance.parted_msa;

  LOG_INFO_TS << "Reading alignment from file: " << opts.msa_file << endl;

  /* load MSA */
  auto msa = msa_load_from_file(opts.msa_file, opts.msa_format);

  LOG_INFO_TS << "Loaded alignment with " << msa.size() << " taxa and " <<
      msa.num_sites() << " sites" << endl;

  if (msa.probabilistic() && opts.use_prob_msa)
  {
    instance.opts.use_pattern_compression = false;
    instance.opts.use_tip_inner = false;

    if (parted_msa.part_count() > 1)
      throw runtime_error("Partitioned probabilistic alignments are not supported yet, sorry...");
  }
  else
    instance.opts.use_prob_msa = false;

  parted_msa.full_msa(std::move(msa));

  LOG_VERB_TS << "Extracting partitions... " << endl;

  parted_msa.split_msa();

  /* check alignment */
  if (!opts.force_mode)
  {
    LOG_VERB_TS << "Validating alignment... " << endl;
    check_msa(instance);
  }

  if (opts.use_pattern_compression)
  {
    LOG_VERB_TS << "Compressing alignment patterns... " << endl;
    parted_msa.compress_patterns();
  }

//  if (parted_msa.part_count() > 1)
//    instance.terrace_wrapper.reset(new TerraceWrapper(parted_msa));

  parted_msa.set_model_empirical_params();

  if (!opts.force_mode)
    check_models(instance);

  LOG_INFO << endl;

  LOG_INFO << "Alignment comprises " << parted_msa.part_count() << " partitions and " <<
      parted_msa.total_length() << " patterns\n" << endl;

  LOG_INFO << parted_msa;

  LOG_INFO << endl;

  if (!instance.opts.use_prob_msa)
  {
    auto binary_msa_fname = instance.opts.binary_msa_file();
    if (sysutil_file_exists(binary_msa_fname) && !opts.redo_mode &&
        opts.command != Command::parse)
    {
      LOG_INFO << "NOTE: Binary MSA file already exists: " << binary_msa_fname << endl << endl;
    }
    else
    {
      RBAStream bs(binary_msa_fname);
      bs << parted_msa;
      LOG_INFO << "NOTE: Binary MSA file created: " << binary_msa_fname << endl << endl;
    }
  }
}

Tree generate_tree(const RaxmlInstance& instance, StartingTree type)
{
  Tree tree;

  const auto& opts = instance.opts;
  const auto& parted_msa = instance.parted_msa;

  switch (type)
  {
    case StartingTree::user:
    {
      assert(instance.start_tree_stream);

      /* parse the unrooted binary tree in newick format, and store the number
         of tip nodes in tip_nodes_count */
      *instance.start_tree_stream >> tree;

      LOG_DEBUG << "Loaded user starting tree with " << tree.num_tips() << " taxa from: "
                           << opts.tree_file << endl;

      check_tree(parted_msa, tree);

      break;
    }
    case StartingTree::random:
      /* no starting tree provided, generate a random one */

      LOG_DEBUG << "Generating a random starting tree with " << parted_msa.taxon_count()
                << " taxa" << endl;

      tree = Tree::buildRandom(parted_msa.taxon_names());

      break;
    case StartingTree::parsimony:
    {
      LOG_DEBUG << "Generating a parsimony starting tree with " << parted_msa.taxon_count()
                << " taxa" << endl;

      unsigned int score;
      unsigned int attrs = opts.simd_arch;

      // TODO: check if there is any reason not to use tip-inner
      attrs |= PLL_ATTRIB_PATTERN_TIP;

      const PartitionedMSA& pars_msa = instance.parted_msa_parsimony ?
                                    *instance.parted_msa_parsimony.get() :  instance.parted_msa;
      tree = Tree::buildParsimony(pars_msa, rand(), attrs, &score);

      LOG_DEBUG << "Parsimony score of the starting tree: " << score << endl;

      break;
    }
    default:
      sysutil_fatal("Unknown starting tree type: %d\n", opts.start_tree);
  }

  assert(!tree.empty());

  return tree;
}

void load_checkpoint(RaxmlInstance& instance, CheckpointManager& cm)
{
  /* init checkpoint and set to the manager */
  {
    Checkpoint ckp;
    for (size_t p = 0; p < instance.parted_msa.part_count(); ++p)
      ckp.models[p] = instance.parted_msa.part_info(p).model();

    // this is a "template" tree, which provides tip labels and node ids
    ckp.tree = instance.random_tree;

    cm.checkpoint(move(ckp));
  }

  if (!instance.opts.redo_mode && cm.read())
  {
    const auto& ckp = cm.checkpoint();
    for (const auto& m: ckp.models)
      instance.parted_msa.model(m.first, m.second);

    LOG_INFO_TS << "NOTE: Resuming execution from checkpoint " <<
        "(logLH: " << ckp.loglh() <<
        ", ML trees: " << ckp.ml_trees.size() <<
        ", bootstraps: " << ckp.bs_trees.size() <<
        ")"
        << endl;
  }
}

void build_parsimony_msa(RaxmlInstance& instance)
{
  // create 1 partition per datatype
  const PartitionedMSA& orig_msa = instance.parted_msa;

  instance.parted_msa_parsimony.reset(new PartitionedMSA(orig_msa.taxon_names()));
  PartitionedMSA& pars_msa = *instance.parted_msa_parsimony.get();

  std::unordered_map<string, PartitionInfo*> datatype_pinfo_map;
  for (const auto& pinfo: orig_msa.part_list())
  {
    const auto& model = pinfo.model();
    auto data_type_name = model.data_type_name();

    auto iter = datatype_pinfo_map.find(data_type_name);
    if (iter == datatype_pinfo_map.end())
    {
      pars_msa.emplace_part_info(data_type_name, model.data_type(), model.name());
      auto& pars_pinfo = pars_msa.part_list().back();
      pars_pinfo.msa(MSA(pinfo.msa().num_sites()));
      datatype_pinfo_map[data_type_name] = &pars_pinfo;
    }
    else
    {
      auto& msa = iter->second->msa();
      msa.num_sites(msa.num_sites() + pinfo.msa().num_sites());
    }
  }

  // set_per-datatype MSA
  for (size_t j = 0; j < orig_msa.taxon_count(); ++j)
  {
    for (auto& pars_pinfo: pars_msa.part_list())
    {
      auto pars_datatype = pars_pinfo.model().data_type_name();
      std::string sequence;
      sequence.resize(pars_pinfo.msa().num_sites());
      size_t offset = 0;

      for (const auto& pinfo: orig_msa.part_list())
      {
        // different datatype -> skip for now
        if (pinfo.model().data_type_name() != pars_datatype)
          continue;

        const auto w = pinfo.msa().weights();
        const auto s = pinfo.msa().at(j);

        for (size_t k = 0; k < w.size(); ++k)
        {
          auto wk = w[k];
          while(wk-- > 0)
            sequence[offset++] = s[k];
        }
      }

      assert(offset == sequence.size());

      pars_pinfo.msa().append(sequence);
    }
  }

  // compress patterns
  if (instance.opts.use_pattern_compression)
  {
    for (auto& pinfo: pars_msa.part_list())
    {
      pinfo.compress_patterns();
    }
  }
}

void build_start_trees(RaxmlInstance& instance, CheckpointManager& cm)
{
  const auto& opts = instance.opts;
  const auto& parted_msa = instance.parted_msa;

  switch (opts.start_tree)
  {
    case StartingTree::user:
      LOG_INFO_TS << "Loading user starting tree(s) from: " << opts.tree_file << endl;
      if (!sysutil_file_exists(opts.tree_file))
        throw runtime_error("File not found: " + opts.tree_file);
      instance.start_tree_stream.reset(new NewickStream(opts.tree_file, std::ios::in));
      break;
    case StartingTree::random:
      LOG_INFO_TS << "Generating random starting tree(s) with " << parted_msa.taxon_count() <<
                     " taxa" << endl;
      break;
    case StartingTree::parsimony:
      if (parted_msa.part_count() > 1)
      {
        LOG_DEBUG_TS << "Generating MSA partitioned by data type for parsimony computation" << endl;
        build_parsimony_msa(instance);
      }
      LOG_INFO_TS << "Generating parsimony starting tree(s) with " << parted_msa.taxon_count()
                  << " taxa" << endl;
      break;
    default:
      assert(0);
  }

  for (size_t i = 0; i < opts.num_searches; ++i)
  {
    auto tree = generate_tree(instance, opts.start_tree);

    // TODO use universal starting tree generator
    if (opts.start_tree == StartingTree::user)
    {
      if (instance.start_tree_stream->peek() != EOF)
        instance.opts.num_searches++;
    }

    // TODO: skip generation
    if (i < cm.checkpoint().ml_trees.size())
      continue;

    /* fix missing branch lengths */
    tree.fix_missing_brlens();

    /* make sure tip indices are consistent between MSA and pll_tree */
    assert(!parted_msa.taxon_id_map().empty());
    tree.reset_tip_ids(parted_msa.taxon_id_map());

    instance.start_trees.emplace_back(move(tree));
  }

  // free memory used for parsimony MSA
  instance.parted_msa_parsimony.release();

  if (::ParallelContext::master_rank())
  {
    NewickStream nw_start(opts.start_tree_file());
    for (auto const& tree: instance.start_trees)
      nw_start << tree;
  }
}

void balance_load(RaxmlInstance& instance)
{
  PartitionAssignment part_sizes;

  /* init list of partition sizes */
  size_t i = 0;
  for (auto const& pinfo: instance.parted_msa.part_list())
  {
    part_sizes.assign_sites(i, 0, pinfo.msa().length());
    ++i;
  }

  instance.proc_part_assign =
      instance.load_balancer->get_all_assignments(part_sizes, ParallelContext::num_procs());

  LOG_INFO_TS << "Data distribution: " << PartitionAssignmentStats(instance.proc_part_assign) << endl;
  LOG_VERB << endl << instance.proc_part_assign;
}

void balance_load(RaxmlInstance& instance, WeightVectorList part_site_weights)
{
  /* This function is used to re-distribute sites across processes for each bootstrap replicate.
   * Since during bootstrapping alignment sites are sampled with replacement, some sites will be
   * absent from BS alignment. Therefore, site distribution computed for original alignment can
   * be suboptimal for BS replicates. Here, we recompute the site distribution, ignoring all sites
   * that are not present in BS replicate (i.e., have weight of 0 in part_site_weights).
   * */

  PartitionAssignment part_sizes;
  WeightVectorList comp_pos_map(part_site_weights.size());

  /* init list of partition sizes */
  size_t i = 0;
  for (auto const& weights: part_site_weights)
  {
    /* build mapping from compressed indices to the original/uncompressed ones */
    comp_pos_map[i].reserve(weights.size());
    for (size_t s = 0; s < weights.size(); ++s)
    {
      if (weights[s] > 0)
      {
        comp_pos_map[i].push_back(s);
      }
    }

    LOG_DEBUG << "Partition #" << i << ": " << comp_pos_map[i].size() << endl;

    /* add compressed partition length to the */
    part_sizes.assign_sites(i, 0, comp_pos_map[i].size());
    ++i;
  }

  instance.proc_part_assign =
      instance.load_balancer->get_all_assignments(part_sizes, ParallelContext::num_procs());

  LOG_VERB_TS << "Data distribution: " << PartitionAssignmentStats(instance.proc_part_assign) << endl;
  LOG_DEBUG << endl << instance.proc_part_assign;

  // translate partition range coordinates: compressed -> uncompressed
  for (auto& part_assign: instance.proc_part_assign)
  {
    for (auto& part_range: part_assign)
    {
      const auto& pos_map = comp_pos_map[part_range.part_id];
      const auto comp_start = part_range.start;
      part_range.start = comp_start > 0 ? pos_map[comp_start] : 0;
      part_range.length = pos_map[comp_start + part_range.length - 1] - part_range.start + 1;
    }
  }

//  LOG_VERB_TS << "(uncompressed) Data distribution: " << PartitionAssignmentStats(instance.proc_part_assign) << endl;
//  LOG_DEBUG << endl << instance.proc_part_assign;
}

void generate_bootstraps(RaxmlInstance& instance, const Checkpoint& checkp)
{
  if (instance.opts.command == Command::bootstrap || instance.opts.command == Command::all)
  {
    BootstrapGenerator bg;
    for (size_t b = 0; b < instance.opts.num_bootstraps; ++b)
    {
      auto seed = rand();

      /* check if this BS was already computed in the previous run and saved in checkpoint */
      if (b < checkp.bs_trees.size())
        continue;

      instance.bs_reps.emplace_back(bg.generate(instance.parted_msa, seed));
    }
  }
}

void draw_bootstrap_support(const Options& opts)
{
  LOG_INFO << "Reading reference tree from file: " << opts.tree_file << endl;

  Tree ref_tree;
  NewickStream refs(opts.tree_file, std::ios::in);
  refs >> ref_tree;

  LOG_INFO << "Reference tree size: " << to_string(ref_tree.num_tips()) << endl << endl;

  auto ref_tip_ids = ref_tree.tip_ids();

  BootstrapTree sup_tree(ref_tree);

  LOG_INFO << "Reading bootstrap trees from file: " << opts.bootstrap_trees_file() << endl;

  NewickStream boots(opts.bootstrap_trees_file(), std::ios::in);
  unsigned int bs_num = 0;
  while (boots.peek() != EOF)
  {
    Tree bs_tree;
    boots >> bs_tree;
    try
    {
      bs_tree.reset_tip_ids(ref_tip_ids);
    }
    catch (out_of_range& e)
    {
      throw runtime_error("Bootstrap tree #" + to_string(bs_num+1) +
                          " is not compatible with the reference tree!");
    }
    catch (invalid_argument& e)
    {
      throw runtime_error("Bootstrap tree #" + to_string(bs_num+1) +
                          " has wrong number of tips: " + to_string(bs_tree.num_tips()));
    }
    sup_tree.add_bootstrap_tree(bs_tree);
    bs_num++;
  }

  LOG_INFO << "Bootstrap trees found: " << bs_num << endl << endl;

  if (bs_num < 2)
  {
    throw runtime_error("You must provide a file with multiple bootstrap trees!");
  }

  sup_tree.calc_support();

  NewickStream sups(opts.support_tree_file(), std::ios::out);
  sups << sup_tree;

  LOG_INFO << "Best ML tree with bootstrap support values saved to: " <<
      sysutil_realpath(opts.support_tree_file()) << endl << endl;
}

void draw_bootstrap_support(RaxmlInstance& instance, const Checkpoint& checkp)
{
  Tree tree = checkp.tree;
  tree.topology(checkp.ml_trees.best_topology());

  instance.bs_tree.reset(new BootstrapTree(tree));

  for (auto bs: checkp.bs_trees)
  {
    tree.topology(bs.second);
    instance.bs_tree->add_bootstrap_tree(tree);
  }
  instance.bs_tree->calc_support();
}

void check_terrace(const RaxmlInstance& instance, const Tree& tree)
{
#ifdef _RAXML_TERRAPHAST
  if (instance.parted_msa.part_count() > 1)
  {
    auto newick_str = to_newick_string_rooted(tree);
    LOG_DEBUG << newick_str << endl << endl;
//      auto terrace_size = instance.terrace_wrapper->get_terrace_size(newick_str);
    TerraceWrapper terrace_wrapper(instance.parted_msa, newick_str);
    try
    {
      auto terrace_size = terrace_wrapper.terrace_size();
      if (terrace_size > 1)
      {
        LOG_WARN << "WARNING: Best-found ML tree lies on a terrace of size: "
                 << terrace_size << endl << endl;

        ofstream fs(instance.opts.terrace_file());
        terrace_wrapper.print_terrace(fs);
        LOG_INFO << "Tree terrace (in compressed Newick format) was saved to: "
            << sysutil_realpath(instance.opts.terrace_file()) << endl << endl;

        // TODO partial prints to multiline newick?
        // if (terrace_size <= instance.opts.terrace_maxsize)
      }
      else
      {
        LOG_INFO << "NOTE: Tree does not lie on a phylogenetic terrace." << endl << endl;
      }
    }
    catch (std::exception& e)
    {
      LOG_ERROR << "ERROR: Failed to compute terrace: " << e.what() << endl << endl;
    }
  }
#else
  RAXML_UNUSED(instance);
  RAXML_UNUSED(tree);
#endif
}

void save_ml_trees(const Options& opts, const Checkpoint& checkp)
{
  NewickStream nw(opts.ml_trees_file(), std::ios::out);
  Tree ml_tree = checkp.tree;
  for (auto topol: checkp.ml_trees)
  {
    ml_tree.topology(topol.second);
    nw << ml_tree;
  }
}

void print_final_output(const RaxmlInstance& instance, const Checkpoint& checkp)
{
  auto const& opts = instance.opts;

  auto model_log_lvl = instance.parted_msa.part_count() > 1 ? LogLevel::verbose : LogLevel::info;

  RAXML_LOG(model_log_lvl) << "\nOptimized model parameters:" << endl;

  for (size_t p = 0; p < instance.parted_msa.part_count(); ++p)
  {
    RAXML_LOG(model_log_lvl) << "\n   Partition " << p << ": " <<
        instance.parted_msa.part_info(p).name().c_str() << endl;
    RAXML_LOG(model_log_lvl) << checkp.models.at(p);
  }

  if (opts.command == Command::evaluate)
  {
    save_ml_trees(opts, checkp);

    LOG_INFO << "\nAll optimized tree(s) saved to: " << sysutil_realpath(opts.ml_trees_file()) << endl;
  }

  if (opts.command == Command::search || opts.command == Command::all)
  {
    auto best = checkp.ml_trees.best();

    LOG_INFO << "\nFinal LogLikelihood: " << FMT_LH(best->first) << endl << endl;

    Tree best_tree = checkp.tree;

    best_tree.topology(best->second);

    NewickStream nw_result(opts.best_tree_file());
    nw_result << best_tree;

    check_terrace(instance, best_tree);

    if (checkp.ml_trees.size() > 1)
    {
      save_ml_trees(opts, checkp);

      LOG_INFO << "All ML trees saved to: " << sysutil_realpath(opts.ml_trees_file()) << endl;
    }

    LOG_INFO << "Best ML tree saved to: " << sysutil_realpath(opts.best_tree_file()) << endl;

    if (opts.command == Command::all)
    {
      assert(instance.bs_tree);

      NewickStream nw(opts.support_tree_file(), std::ios::out);
      nw << *instance.bs_tree;

      LOG_INFO << "Best ML tree with bootstrap support values saved to: " <<
          sysutil_realpath(opts.support_tree_file()) << endl;
    }
  }

  if (opts.command == Command::search || opts.command == Command::all ||
      opts.command == Command::evaluate)
  {
    RaxmlPartitionStream model_stream(opts.best_model_file(), true);
    model_stream.print_model_params(true);
    model_stream << instance.parted_msa;

    LOG_INFO << "Optimized model saved to: " << sysutil_realpath(opts.best_model_file()) << endl;
  }

  if (opts.command == Command::bootstrap || opts.command == Command::all)
  {
    // TODO now only master process writes the output, this will have to change with
    // coarse-grained parallelization scheme (parallel start trees/bootstraps)
//    NewickStream nw(opts.bootstrap_trees_file(), std::ios::out | std::ios::app);
    NewickStream nw(opts.bootstrap_trees_file(), std::ios::out);

    Tree bs_tree = checkp.tree;
    for (auto topol: checkp.bs_trees)
    {
      bs_tree.topology(topol.second);
      nw << bs_tree;
    }

    LOG_INFO << "Bootstrap trees saved to: " << sysutil_realpath(opts.bootstrap_trees_file()) << endl;
  }

  LOG_INFO << "\nExecution log saved to: " << sysutil_realpath(opts.log_file()) << endl;

  LOG_INFO << "\nAnalysis started: " << global_timer().start_time();
  LOG_INFO << " / finished: " << global_timer().current_time() << std::endl;
  LOG_INFO << "\nElapsed time: " << FMT_PREC3(global_timer().elapsed_seconds()) << " seconds";
  if (checkp.elapsed_seconds > 0.)
  {
    LOG_INFO << " (this run) / ";
    LOG_INFO << FMT_PREC3(checkp.elapsed_seconds + global_timer().elapsed_seconds()) <<
        " seconds (total with restarts)";
  }

  LOG_INFO << endl << endl;
}

void thread_main(RaxmlInstance& instance, CheckpointManager& cm)
{
  unique_ptr<TreeInfo> treeinfo;

//  printf("thread %lu / %lu\n", ParallelContext::thread_id(), ParallelContext::num_procs());

  /* wait until master thread prepares all global data */
  ParallelContext::thread_barrier();

  auto const& master_msa = instance.parted_msa;
  auto const& opts = instance.opts;

  /* get partitions assigned to the current thread */
  auto const& part_assign = instance.proc_part_assign.at(ParallelContext::proc_id());

  if ((opts.command == Command::search || opts.command == Command::all ||
      opts.command == Command::evaluate ) && !instance.start_trees.empty())
  {

    if (opts.command == Command::evaluate)
    {
      LOG_INFO << "\nEvaluating " << opts.num_searches <<
          " trees" << endl << endl;
    }
    else
    {
      LOG_INFO << "\nStarting ML tree search with " << opts.num_searches <<
          " distinct starting trees" << endl << endl;
    }

    size_t start_tree_num = cm.checkpoint().ml_trees.size();
    bool use_ckp_tree = cm.checkpoint().search_state.step != CheckpointStep::start;
    for (const auto& tree: instance.start_trees)
    {
      assert(!tree.empty());

      start_tree_num++;

      if (use_ckp_tree)
      {
        treeinfo.reset(new TreeInfo(opts, cm.checkpoint().tree, master_msa, part_assign));
        use_ckp_tree = false;
      }
      else
        treeinfo.reset(new TreeInfo(opts, tree, master_msa, part_assign));

      Optimizer optimizer(opts);
      if (opts.command == Command::evaluate)
      {
        LOG_INFO_TS << "Tree #" << start_tree_num <<
            ", initial LogLikelihood: " << FMT_LH(treeinfo->loglh()) << endl;
        LOG_PROGR << endl;
        optimizer.evaluate(*treeinfo, cm);
      }
      else
      {
        optimizer.optimize_topology(*treeinfo, cm);
      }

      LOG_PROGR << endl;
      if (opts.command == Command::evaluate)
      {
        LOG_INFO_TS << "Tree #" << start_tree_num <<
            ", final logLikelihood: " << FMT_LH(cm.checkpoint().loglh()) << endl;
      }
      else
      {
        LOG_INFO_TS << "ML tree search #" << start_tree_num <<
            ", logLikelihood: " << FMT_LH(cm.checkpoint().loglh()) << endl;
      }
      LOG_PROGR << endl;

      cm.save_ml_tree();
      cm.reset_search_state();
    }
  }

  ParallelContext::thread_barrier();

  if (!instance.bs_reps.empty())
  {
    if (opts.command == Command::all)
    {
      LOG_INFO << endl;
      LOG_INFO_TS << "ML tree search completed, best tree logLH: " <<
          FMT_LH(cm.checkpoint().ml_trees.best_score()) << endl << endl;
    }

    LOG_INFO_TS << "Starting bootstrapping analysis with " << opts.num_bootstraps
             << " replicates." << endl << endl;
  }

  /* infer bootstrap trees if needed */
  size_t bs_num = cm.checkpoint().bs_trees.size();
  for (const auto bs: instance.bs_reps)
  {
    ++bs_num;

    // rebalance sites
    if (ParallelContext::master_thread())
    {
      balance_load(instance, bs.site_weights);
    }
    ParallelContext::thread_barrier();

    auto const& bs_part_assign = instance.proc_part_assign.at(ParallelContext::proc_id());

//    Tree tree = Tree::buildRandom(master_msa.full_msa());
    /* for now, use the same random tree for all bootstraps */
    const Tree& tree = instance.random_tree;
    treeinfo.reset(new TreeInfo(opts, tree, master_msa, bs_part_assign, bs.site_weights));

//    size_t sumw = 0;
//    for (auto sw: bs.site_weights)
//      for (auto w: sw)
//      {
//        sumw += w;
//        LOG_INFO << w << "  ";
//      }
//
//    LOG_INFO << "\n\nTotal BS sites: " << sumw << endl;

    Optimizer optimizer(opts);
    optimizer.optimize_topology(*treeinfo, cm);

    LOG_PROGR << endl;
    LOG_INFO_TS << "Bootstrap tree #" << bs_num <<
                ", logLikelihood: " << FMT_LH(cm.checkpoint().loglh()) << endl;
    LOG_PROGR << endl;

    cm.save_bs_tree();
    cm.reset_search_state();
  }

  ParallelContext::thread_barrier();
}

void master_main(RaxmlInstance& instance, CheckpointManager& cm)
{
  auto const& opts = instance.opts;
  auto& parted_msa = instance.parted_msa;

  init_part_info(instance);

  if (instance.parted_msa.part_info(0).msa().empty())
    load_msa(instance);

  // we need 2 doubles for each partition AND threads to perform parallel reduction,
  // so resize the buffer accordingly
  const size_t reduce_buffer_size = std::max(1024lu, 2 * sizeof(double) *
                                     parted_msa.part_count() * ParallelContext::num_threads());
  LOG_DEBUG << "Parallel reduction buffer size: " << reduce_buffer_size/1024 << " KB\n\n";
  ParallelContext::resize_buffer(reduce_buffer_size);

  /* init template tree */
  instance.random_tree = generate_tree(instance, StartingTree::random);

  /* load checkpoint */
  load_checkpoint(instance, cm);

  /* load/create starting tree */
  build_start_trees(instance, cm);

  LOG_VERB << endl << "Initial model parameters:" << endl;
  for (size_t p = 0; p < parted_msa.part_count(); ++p)
  {
    LOG_VERB << "   Partition: " << parted_msa.part_info(p).name() << endl <<
        parted_msa.model(p) << endl;
  }

  /* run load balancing algorithm */
  balance_load(instance);

  /* check that we have enough patterns per thread */
  if (ParallelContext::master_rank() && ParallelContext::num_procs() > 1)
  {
    PartitionAssignmentStats stats(instance.proc_part_assign);

    const size_t soft_limit = 600;
    const size_t hard_limit = 150;

    // TODO: adapt for mixed alignments (e.g., DNA + AA partitions)
    auto states = parted_msa.part_info(0).model().num_states();
    for (const auto& p: parted_msa.part_list())
      states = std::max(states, p.model().num_states());

    const size_t norm_thread_pats = stats.min_thread_sites * (((double) states) / 4.) *
        (ParallelContext::num_threads() < 8 ? 3 : 1);
    if (norm_thread_pats < soft_limit)
    {
      size_t opt_threads = trunc(stats.total_sites / (soft_limit*2)) + 1;
      LOG_WARN << endl;
      LOG_WARN << "WARNING: You are probably using too many threads (" << ParallelContext::num_threads() <<
          ") for your alignment with " << stats.total_sites << " unique patterns." << endl;
      LOG_WARN << "NOTE:    For the optimal throughput, please consider using " << opt_threads <<
          " threads ('--threads " << opt_threads << "' option)" << endl;
      LOG_WARN << "NOTE:    and parallelize across starting trees/bootstrap replicates." << endl;
      LOG_WARN << "NOTE:    As a general rule-of-thumb, please assign at least 200-1000 "
          "alignment patterns per thread." << endl;

      if (norm_thread_pats < hard_limit && !instance.opts.force_mode)
        throw runtime_error("Too few patterns per thread! "
                            "RAxML-NG will terminate now to avoid wasting resources.\n"
                            "NOTE:  Please reduce the number of threads (see guidelines above).\n"
                            "NOTE:  This check can be disabled with the '--force' option.");
    }
  }

  /* generate bootstrap replicates */
  generate_bootstraps(instance, cm.checkpoint());

  if (ParallelContext::master_rank())
    instance.opts.remove_result_files();

  thread_main(instance, cm);

  if (ParallelContext::master_rank())
  {
    if (opts.command == Command::all)
      draw_bootstrap_support(instance, cm.checkpoint());

    assert(cm.checkpoint().models.size() == instance.parted_msa.part_count());
    for (size_t p = 0; p < instance.parted_msa.part_count(); ++p)
    {
      instance.parted_msa.model(p, cm.checkpoint().models.at(p));
    }

    print_final_output(instance, cm.checkpoint());

    /* analysis finished successfully, remove checkpoint file */
    cm.remove();
  }
}

extern "C" int raxml_main(int argc, char** argv, void *communicator)
{
  int retval = EXIT_SUCCESS;

  RaxmlInstance instance;
  
  ParallelContext::init_mpi(argc, argv, communicator);

  instance.opts.num_ranks = ParallelContext::num_ranks();

  logger().add_log_stream(&cout);

  CommandLineParser cmdline;
  try
  {
    cmdline.parse_options(argc, argv, instance.opts);
  }
  catch (OptionException &e)
  {
    LOG_INFO << "ERROR: " << e.message() << std::endl;
    return ParallelContext::clean_exit(EXIT_FAILURE);
  }

  /* handle trivial commands first */
  switch (instance.opts.command)
  {
    case Command::help:
      print_banner();
      cmdline.print_help();
      return ParallelContext::clean_exit(EXIT_SUCCESS);
      break;
    case Command::version:
      print_banner();
      return ParallelContext::clean_exit(EXIT_SUCCESS);
      break;
    case Command::evaluate:
    case Command::search:
    case Command::bootstrap:
    case Command::all:
    case Command::support:
      if (!instance.opts.redo_mode && instance.opts.result_files_exist())
      {
        LOG_ERROR << endl << "ERROR: Result files for the run with prefix `" <<
                            (instance.opts.outfile_prefix.empty() ?
                                instance.opts.msa_file : instance.opts.outfile_prefix) <<
                            "` already exist!\n" <<
                            "Please either choose a new prefix, remove old files, or add "
                            "--redo command line switch to overwrite them." << endl << endl;
        return ParallelContext::clean_exit(EXIT_FAILURE);
      }
      break;
    default:
      break;
  }

  /* now get to the real stuff */
  srand(instance.opts.random_seed);
  logger().set_log_level(instance.opts.log_level);

  /* only master process writes the log file */
  if (ParallelContext::master())
  {
    auto mode = !instance.opts.redo_mode && sysutil_file_exists(instance.opts.checkp_file()) ?
        ios::app : ios::out;
    logger().set_log_filename(instance.opts.log_file(), mode);
  }

  print_banner();
  LOG_INFO << instance.opts;

  try
  {
    switch (instance.opts.command)
    {
      case Command::evaluate:
      case Command::search:
      case Command::bootstrap:
      case Command::all:
      {
        if (instance.opts.redo_mode)
        {
          LOG_WARN << "WARNING: Running in REDO mode: existing checkpoints are ignored, "
              "and all result files will be overwritten!" << endl << endl;
        }

        if (instance.opts.force_mode)
        {
          LOG_WARN << "WARNING: Running in FORCE mode: all safety checks are disabled!"
              << endl << endl;
        }

        /* init load balancer */
        instance.load_balancer.reset(new KassianLoadBalancer());

        CheckpointManager cm(instance.opts.checkp_file());
        ParallelContext::init_pthreads(instance.opts, std::bind(thread_main,
                                                                std::ref(instance),
                                                                std::ref(cm)));

        master_main(instance, cm);
        break;
      }
      case Command::support:
        draw_bootstrap_support(instance.opts);
        break;
      case Command::terrace:
      {
        init_part_info(instance);
        load_msa(instance);
        assert(instance.opts.start_tree == StartingTree::user);
        LOG_INFO << "Loading tree from: " << instance.opts.tree_file << endl << endl;
        if (!sysutil_file_exists(instance.opts.tree_file))
          throw runtime_error("File not found: " + instance.opts.tree_file);
        instance.start_tree_stream.reset(new NewickStream(instance.opts.tree_file, std::ios::in));
        Tree tree = generate_tree(instance, instance.opts.start_tree);
        check_terrace(instance, tree);
        break;
      }
      case Command::check:
        instance.opts.use_pattern_compression = false;
      case Command::parse:
      {
        init_part_info(instance);
        load_msa(instance);
        if (instance.opts.start_tree == StartingTree::user)
        {
          LOG_INFO << "Loading tree from: " << instance.opts.tree_file << endl << endl;
          if (!sysutil_file_exists(instance.opts.tree_file))
            throw runtime_error("File not found: " + instance.opts.tree_file);
          instance.start_tree_stream.reset(new NewickStream(instance.opts.tree_file, std::ios::in));
          Tree tree = generate_tree(instance, instance.opts.start_tree);
        }
        LOG_INFO << "Alignment can be successfully read by RAxML-NG." << endl << endl;
        break;
      }
      case Command::none:
      default:
        LOG_ERROR << "Unknown command!" << endl;
        retval = EXIT_FAILURE;
    }
  }
  catch(exception& e)
  {
    LOG_ERROR << endl << "ERROR: " << e.what() << endl << endl;
    retval = EXIT_FAILURE;
  }

  return ParallelContext::clean_exit(retval);
}

