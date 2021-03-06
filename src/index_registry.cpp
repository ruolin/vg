// index_registry.cpp: index registry system implementation

#include "index_registry.hpp"

#include <iostream>
#include <sstream>
#include <vector>
#include <map>

#include <bdsg/hash_graph.hpp>
#include <bdsg/packed_graph.hpp>
#include <bdsg/odgi.hpp>
#include <xg.hpp>
#include <gbwt/variants.h>
#include <vg/io/vpkg.hpp>
#include <gcsa/gcsa.h>
#include <gcsa/algorithms.h>

#include "vg.hpp"
#include "handle.hpp"
#include "utility.hpp"
#include "constructor.hpp"
#include "hash_map.hpp"
#include "haplotype_indexer.hpp"
#include "phase_unfolder.hpp"
#include "gbwt_helper.hpp"
#include "kmer.hpp"
#include "source_sink_overlay.hpp"

#include "io/save_handle_graph.hpp"

#include "algorithms/gfa_to_handle.hpp"
#include "algorithms/prune.hpp"

//#define debug_index_registry
//#define debug_index_registry_path_state

namespace vg {

IndexingParameters::MutableGraphImplementation IndexingParameters::mut_graph_impl = HashGraph;
int IndexingParameters::max_node_size = 32;
int IndexingParameters::pruning_max_node_degree = 128;
int IndexingParameters::pruning_walk_length = 24;
int IndexingParameters::pruning_max_edge_count = 3;
int IndexingParameters::pruning_min_component_size = 33;
int IndexingParameters::gcsa_initial_kmer_length = gcsa::Key::MAX_LENGTH;
int IndexingParameters::gcsa_doubling_steps = gcsa::ConstructionParameters::DOUBLING_STEPS;
bool IndexingParameters::verbose = false;

IndexRegistry VGIndexes::get_vg_index_registry() {
    
    IndexRegistry registry;
    
    /*********************
     * Register all of the VG indexes and input files
     ***********************/
    
    /// Data files
    registry.register_index("Reference FASTA", "fasta");
    registry.register_index("VCF", "vcf");
    registry.register_index("Phased VCF", "phased.vcf");
    registry.register_index("Insertion Sequence FASTA", "insertions.fasta");
    registry.register_index("Reference GFA", "gfa");
    
    /// True indexes
    registry.register_index("VG + Variant Paths", "varpaths.vg");
    //registry.register_index("VG + NodeMapping", "vg_mapping");
    //registry.register_index("VG + Variant Paths + NodeMapping", "vg_mapping");
    registry.register_index("VG", "vg");
    registry.register_index("XG", "xg");
    registry.register_index("GBWT", "gbwt");
    registry.register_index("NodeMapping", "mapping");
    registry.register_index("Pruned VG", "pruned.vg");
    registry.register_index("Haplotype-Pruned VG + NodeMapping", "haplopruned.vg");
    registry.register_index("GCSA + LCP", "gcsa");
    
    /*********************
     * A few handy lambda functions
     ***********************/
    
    auto init_in = [](ifstream& in, const string& name) {
        in.open(name);
        if (!in) {
            cerr << "error:[IndexRegistry] could not open input file " << name << endl;
            exit(1);
        }
    };
    auto init_out = [](ofstream& out, const string& name) {
        out.open(name);
        if (!out) {
            cerr << "error:[IndexRegistry] could not write output to " << name << endl;
            exit(1);
        }
    };
    auto init_in_out = [](fstream& strm, const string& name) {
        strm.open(name);
        if (!strm) {
            cerr << "error:[IndexRegistry] could not open " << name << endl;
            exit(1);
        }
    };
    
    auto init_mutable_graph = [&]() -> unique_ptr<MutablePathDeletableHandleGraph> {
        unique_ptr<MutablePathDeletableHandleGraph> graph;
        switch (IndexingParameters::mut_graph_impl) {
            case IndexingParameters::HashGraph:
                graph = make_unique<bdsg::HashGraph>();
                break;
            case IndexingParameters::ODGI:
                graph = make_unique<bdsg::ODGI>();
                break;
            case IndexingParameters::PackedGraph:
                graph = make_unique<bdsg::PackedGraph>();
                break;
            case IndexingParameters::VG:
                graph = make_unique<VG>();
                break;
            default:
                cerr << "error:[IndexRegistry] unrecognized mutable graph implementation format" << endl;
                exit(1);
                break;
        }
        return graph;
    };
    
    
    /*********************
     * Register all recipes
     ***********************/
    
    ////////////////////////////////////
    // VCF Recipes
    ////////////////////////////////////
    
#ifdef debug_index_registry
    cerr << "registering VCF recipes" << endl;
#endif
    
    // alias a phased VCF as an unphased one
    registry.register_recipe("VCF", {"Phased VCF"},
                             [](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        return inputs.front()->get_filenames();
    });
    
    ////////////////////////////////////
    // VG Recipes
    ////////////////////////////////////
    
#ifdef debug_index_registry
    cerr << "registering VG recipes" << endl;
#endif
    
//    // alias the VG from a co-created VG and node mapping
//    registry.register_recipe("VG", {"VG + NodeMapping"},
//                             [](const vector<const IndexFile*>& inputs,
//                                const string& prefix, const string& suffix) {
//        return inputs.front()->get_filenames().front();
//    });
    
    // strip alt allele paths from a graph that has them
    registry.register_recipe("VG", {"VG + Variant Paths"},
                             [&](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Stripping allele paths from VG." << endl;
        }
        
        // test streams for I/O
        ifstream infile;
        init_in(infile, inputs.at(0)->get_filenames().front());
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        
        unique_ptr<MutablePathHandleGraph> graph
            = vg::io::VPKG::load_one<MutablePathHandleGraph>(infile);
        
        // gather handles to the alt allele paths
        vector<path_handle_t> alt_paths;
        graph->for_each_path_handle([&](const path_handle_t& path) {
            auto name = graph->get_path_name(path);
            if (!name.empty() && name.substr(0, 5) == "_alt_") {
                alt_paths.push_back(path);
            }
        });
        
        // delete them
        for (auto path : alt_paths) {
            graph->destroy_path(path);
        }
        
        // and save the graph
        vg::io::save_handle_graph(graph.get(), outfile);
        
        // return the filename
        return vector<string>(1, output_name);
    });
        
    // meta-recipe for creating a VG from a GFA
    auto construct_from_gfa = [&](const vector<const IndexFile*>& inputs,
                                  const string& prefix, const string& suffix,
                                  nid_t* max_node_id_out) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Constructing VG graph from GFA input." << endl;
        }
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        auto graph = init_mutable_graph();
        
        // make the graph from GFA
        try {
            algorithms::gfa_to_path_handle_graph(inputs.at(0)->get_filenames().front(), graph.get(), true,
                                                 IndexingParameters::mut_graph_impl == IndexingParameters::ODGI);
        }
        catch (algorithms::GFAFormatError& e) {
            cerr << "error:[IndexRegistry] Input GFA is not usuable in VG." << endl;
            cerr << e.what() << endl;
            exit(1);
        }
        
        if (max_node_id_out) {
            *max_node_id_out = graph->max_node_id();
        }
        
        // save the file
        vg::io::save_handle_graph(graph.get(), outfile);
        
        // return the filename
        return vector<string>(1, output_name);
    };
    
    registry.register_recipe("VG", {"Reference GFA"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        return construct_from_gfa(inputs, prefix, suffix, nullptr);
    });
    
    // A meta-recipe to make VG files using the Constructor
    // Expects inputs to be ordered: FASTA, VCF[, Insertion FASTA]
    auto construct_with_constructor = [&](const vector<const IndexFile*>& inputs,
                                          const string& prefix,
                                          const string& suffix,
                                          bool alt_paths,
                                          nid_t* max_node_id_out) {
        
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Constructing VG graph from FASTA and VCF input." << endl;
        }
        assert(inputs.size() == 2 || inputs.size() == 3);
        
        // init and configure the constructor
        Constructor constructor;
        //constructor.do_svs = true; // TODO: this crashes the Constructor on simple input. why?
        constructor.alt_paths = alt_paths;
        constructor.max_node_size = IndexingParameters::max_node_size;
        constructor.show_progress = IndexingParameters::verbose;
        
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        
        auto graph = init_mutable_graph();
        
        vector<string> insertion_filenames;
        if (inputs.size() == 3) {
            insertion_filenames = inputs.back()->get_filenames();
        }
        
        // do the construction
        constructor.construct_graph(inputs.at(0)->get_filenames(),
                                    inputs.at(1)->get_filenames(),
                                    insertion_filenames,
                                    graph.get());
        
        // save the file
        vg::io::save_handle_graph(graph.get(), outfile);
        
        if (max_node_id_out) {
            // TODO: maybe use this for initializing a NodeMapping
            *max_node_id_out = graph->max_node_id();
        }
        
        // return the filename
        return vector<string>(1, output_name);
    };
    
    // the specific instantiations of the meta-recipe above
    registry.register_recipe("VG", {"Reference FASTA", "VCF", "Insertion Sequence FASTA"},
                             [&](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        return construct_with_constructor(inputs, prefix, suffix, false, nullptr);
    });
    registry.register_recipe("VG", {"Reference FASTA", "VCF"},
                             [&](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        return construct_with_constructor(inputs, prefix, suffix, false, nullptr);
    });
    registry.register_recipe("VG + Variant Paths", {"Reference FASTA", "Phased VCF", "Insertion Sequence FASTA"},
                             [&](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        return construct_with_constructor(inputs, prefix, suffix, true, nullptr);
    });
    registry.register_recipe("VG + Variant Paths", {"Reference FASTA", "Phased VCF"},
                             [&](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        return construct_with_constructor(inputs, prefix, suffix, true, nullptr);
    });
    
    ////////////////////////////////////
    // VG + NodeMapping Recipes
    ////////////////////////////////////
    
//    registry.register_recipe("VG + NodeMapping", {"Reference GFA"},
//                             [](const vector<const IndexFile*>& inputs,
//                                const string& prefix, const string& suffix) {
//
//        // TODO: i don't like how the system makes me double-enter suffixes here
//        string mapping_filename = prefix +
//        ofstream outfile
//        nid_t max_node_id = 0;
//        auto filepaths = construct_with_constructor(inputs, prefix, "vg", false, &max_node_id);
//        gcsa::NodeMapping mapping(max_node_id + 1);
//    });
    
    
    ////////////////////////////////////
    // XG Recipes
    ////////////////////////////////////
    
#ifdef debug_index_registry
    cerr << "registering XG recipes" << endl;
#endif
    
    registry.register_recipe("XG", {"Reference GFA"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Constructing XG graph from GFA input." << endl;
        }
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        
        xg::XG xg_index;
        xg_index.from_gfa(inputs.front()->get_filenames().front());
        
        vg::io::save_handle_graph(&xg_index, outfile);
        
        // return the filename
        return vector<string>(1, output_name);
    });
    
    registry.register_recipe("XG", {"VG"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Constructing XG graph from VG graph." << endl;
        }
        // test streams for I/O
        ifstream infile;
        init_in(infile, inputs.at(0)->get_filenames().front());
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        
        unique_ptr<PathHandleGraph> graph = vg::io::VPKG::load_one<PathHandleGraph>(infile);
        
        xg::XG xg_index;
        xg_index.from_path_handle_graph(*graph);
        
        vg::io::save_handle_graph(&xg_index, outfile);
        
        // return the filename
        return vector<string>(1, output_name);
    });
    
    ////////////////////////////////////
    // NodeMapping Recipes
    ////////////////////////////////////
    
//    // alias the VG from a co-created VG and node mapping
//    registry.register_recipe("NodeMapping", {"VG + NodeMapping"},
//                             [](const vector<const IndexFile*>& inputs,
//                                const string& prefix, const string& suffix) {
//        return inputs.front()->get_filenames().back();
//    });
    
    
#ifdef debug_index_registry
    cerr << "registering NodeMapping recipes" << endl;
#endif
    
    registry.register_recipe("NodeMapping", {"VG"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Initializing NodeMapping from VG." << endl;
        }
        // TODO: this is pretty unoptimized in that we have to load the whole graph just
        // to read the max node id
        ifstream infile;
        init_in(infile, inputs.at(0)->get_filenames().front());
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        
        unique_ptr<PathHandleGraph> graph = vg::io::VPKG::load_one<PathHandleGraph>(infile);
        
        gcsa::NodeMapping mapping(graph->max_node_id() + 1);
        mapping.serialize(outfile);
        
        return vector<string>(1, output_name);
    });
    
    ////////////////////////////////////
    // GBWT Recipes
    ////////////////////////////////////
    
#ifdef debug_index_registry
    cerr << "registering GBWT recipes" << endl;
#endif
    
    registry.register_recipe("GBWT", {"VG + Variant Paths", "Phased VCF"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Constructing GBWT from VG graph and phased VCF input." << endl;
        }
        // test streams for I/O
        ifstream infile;
        init_in(infile, inputs.at(0)->get_filenames().front());
        string output_name = prefix + "." + suffix;
        ofstream outfile;
        init_out(outfile, output_name);
        
        HaplotypeIndexer haplotype_indexer;
        haplotype_indexer.show_progress = IndexingParameters::verbose;
        
        unique_ptr<PathHandleGraph> graph
            = vg::io::VPKG::load_one<PathHandleGraph>(infile);
        
        vector<string> parse_files = haplotype_indexer.parse_vcf(inputs.front()->get_filenames().front(),
                                                                 *graph);
        graph.reset(); // Save memory by deleting the graph.
        std::unique_ptr<gbwt::DynamicGBWT> gbwt_index = haplotype_indexer.build_gbwt(parse_files);
        
        vg::io::VPKG::save(*gbwt_index, output_name);
        
        return vector<string>(1, output_name);
    });
    
    ////////////////////////////////////
    // Pruned VG Recipes
    ////////////////////////////////////
    
#ifdef debug_index_registry
    cerr << "registering pruning recipes" << endl;
#endif
    
    // meta-recipe for pruning with/without GBWT
    auto prune_graph = [&](const vector<const IndexFile*>& inputs,
                           const string& prefix, const string& suffix) {
        
        // we only want to focus on two specific recipes
        assert(inputs.size() == 2 || inputs.size() == 4);
        bool using_haplotypes = inputs.size() == 4;
        
        // test streams for I/O
        ifstream infile_vg, infile_xg, infile_gbwt, infile_mapping;
        init_in(infile_vg, inputs.at(0)->get_filenames().front());
        init_in(infile_xg, inputs.at(1)->get_filenames().front());
        if (using_haplotypes) {
            init_in(infile_vg, inputs.at(2)->get_filenames().front());
            init_in(infile_mapping, inputs.at(3)->get_filenames().front());
        }
        string vg_output_name = prefix + "." + suffix;
        string mapping_output_name = prefix + "." + suffix + ".mapping";
        ofstream outfile_vg, outfile_mapping;
        init_out(outfile_vg, vg_output_name);
        if (using_haplotypes) {
            init_out(outfile_mapping, mapping_output_name);
        }
        
        unique_ptr<MutablePathDeletableHandleGraph> graph
            = vg::io::VPKG::load_one<MutablePathDeletableHandleGraph>(infile_vg);
        
        // remove all paths
        vector<path_handle_t> paths;
        paths.reserve(graph->get_path_count());
        graph->for_each_path_handle([&](const path_handle_t& path) {
            paths.push_back(path);
        });
        for (auto path : paths) {
            graph->destroy_path(path);
        }
        
        // prune the graph based on topology
        if (IndexingParameters::pruning_max_node_degree != 0) {
            algorithms::remove_high_degree_nodes(*graph, IndexingParameters::pruning_max_node_degree);
        }
        algorithms::prune_complex_with_head_tail(*graph, IndexingParameters::pruning_walk_length,
                                                 IndexingParameters::pruning_max_edge_count);
        algorithms::prune_short_subgraphs(*graph, IndexingParameters::pruning_min_component_size);
        
        if (!paths.empty() || using_haplotypes) {
            // there were paths/threads we can restore
            
            xg::XG xg_index;
            xg_index.deserialize(infile_xg);
            
            if (!using_haplotypes) {
                // we can bring back edges on embedded paths
                
                // Make an empty GBWT index to pass along
                gbwt::GBWT empty_gbwt;
                PhaseUnfolder unfolder(xg_index, empty_gbwt, xg_index.max_node_id() + 1);
                unfolder.restore_paths(*graph, IndexingParameters::verbose);
            }
            else {
                // we can expand out complex regions using haplotypes
                
                // copy the node mapping so that we have a copy that we can alter without
                // potentially messing up the input for other recipes
                unique_ptr<gbwt::GBWT> gbwt_index = vg::io::VPKG::load_one<gbwt::GBWT>(infile_gbwt);
                PhaseUnfolder unfolder(xg_index, *gbwt_index, xg_index.max_node_id() + 1);
                unfolder.read_mapping(inputs.at(3)->get_filenames().front());
                unfolder.unfold(*graph, IndexingParameters::verbose);
                unfolder.write_mapping(mapping_output_name);
            }
        }
        
        vg::io::save_handle_graph(graph.get(), outfile_vg);
        
        if (using_haplotypes) {
            return vector<string>{vg_output_name, mapping_output_name};
        }
        else {
            return vector<string>(1, vg_output_name);
        }
    };
    
    registry.register_recipe("Pruned VG", {"VG", "XG"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Pruning complex regions of VG to prepare for GCSA indexing." << endl;
        }
        // call the meta-recipe
        return prune_graph(inputs, prefix, suffix);
    });
    
    registry.register_recipe("Haplotype-Pruned VG + NodeMapping", {"VG", "XG", "GBWT", "NodeMapping"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Pruning complex regions of VG to prepare for GCSA indexing with GBWT unfolding." << endl;
        }
        
        // call the meta-recipe
        return prune_graph(inputs, prefix, suffix);
    });
    
    ////////////////////////////////////
    // GCSA + LCP Recipes
    ////////////////////////////////////
    
#ifdef debug_index_registry
    cerr << "registering GCSA recipes" << endl;
#endif
    
    // meta-recipe for GCSA indexing with or without unfolded input
    auto construct_gcsa = [&](const vector<const IndexFile*>& inputs,
                              const string& prefix, const string& suffix) {
        
        if (IndexingParameters::verbose) {
            cerr << "[IndexRegistry]: Constructing GCSA/LCP indexes." << endl;
        }
        
        assert(inputs.size() == 1);
        assert(inputs.front()->get_filenames().size() == 1 ||
               inputs.front()->get_filenames().size() == 2);
        bool unfolded = inputs.front()->get_filenames().size() == 2;
        
        // test streams for I/O
        ifstream infile_vg, infile_mapping;
        init_in(infile_vg, inputs.at(0)->get_filenames().front());
        string mapping_filename;
        if (unfolded) {
            mapping_filename = inputs.at(0)->get_filenames().back();
            init_in(infile_mapping, mapping_filename);
        }
        string gcsa_output_name = prefix + "." + suffix;
        string lcp_output_name = gcsa_output_name + ".lcp";
        ofstream outfile_gcsa;
        ofstream outfile_lcp;
        init_out(outfile_gcsa, gcsa_output_name);
        init_out(outfile_lcp, lcp_output_name);
        
        // configure
        gcsa::TempFile::setDirectory(temp_file::get_dir());
        if (IndexingParameters::verbose) {
            gcsa::Verbosity::set(gcsa::Verbosity::BASIC);
        }
        else {
            gcsa::Verbosity::set(gcsa::Verbosity::SILENT);
        }
        auto params = gcsa::ConstructionParameters();
        params.doubling_steps = IndexingParameters::gcsa_doubling_steps;
        
        // load graph to index
        unique_ptr<HandleGraph> graph = vg::io::VPKG::load_one<HandleGraph>(infile_vg);
        SourceSinkOverlay overlay(graph.get(), IndexingParameters::gcsa_initial_kmer_length);
        size_t kmer_bytes = params.getLimitBytes();
        
        // get the intiial k-mers
        string dbg_name = write_gcsa_kmers_to_tmpfile(overlay, IndexingParameters::gcsa_initial_kmer_length,
                                                      kmer_bytes, overlay.get_id(overlay.get_source_handle()),
                                                      overlay.get_id(overlay.get_sink_handle()));
        
        // construct the indexes (giving empty mapping name is sufficient to make
        // indexing skip the unfolded code path)
        gcsa::InputGraph input_graph(vector<string>(1, dbg_name), true, gcsa::Alphabet(),
                                     mapping_filename);
        gcsa::GCSA gcsa_index(input_graph, params);
        gcsa::LCPArray lcp_array(input_graph, params);
        
        // clean up the k-mers file
        temp_file::remove(dbg_name);
        
        vg::io::VPKG::save(gcsa_index, gcsa_output_name);
        vg::io::VPKG::save(lcp_array, lcp_output_name);
        
        return vector<string>{gcsa_output_name, lcp_output_name};
    };
    
    registry.register_recipe("GCSA + LCP", {"Haplotype-Pruned VG + NodeMapping"},
                            [&](const vector<const IndexFile*>& inputs,
                                const string& prefix, const string& suffix) {
        // execute meta recipe
        return construct_gcsa(inputs, prefix, suffix);
    });
    
    registry.register_recipe("GCSA + LCP", {"Pruned VG"},
                             [&](const vector<const IndexFile*>& inputs,
                                 const string& prefix, const string& suffix) {
        // execute meta recipe
        return construct_gcsa(inputs, prefix, suffix);
    });
    
    return registry;
}

vector<string> VGIndexes::get_default_map_indexes() {
    vector<string> indexes{
        "XG",
        "GCSA + LCP"
    };
    return indexes;
}

vector<string> VGIndexes::get_default_mpmap_indexes() {
    vector<string> indexes{
        "Spliced XG",
        "Spliced Distance",
        "Spliced GCSA + LCP",
        "Haplotype-Transcript GBWT"
    };
    return indexes;
}

vector<string> VGIndexes::get_default_giraffe_indexes() {
    vector<string> indexes{
        "GBWT",
        "GBWTGraph",
        "Distance",
        "Minimizer"
    };
    return indexes;
}

void IndexRegistry::set_prefix(const string& prefix) {
    this->output_prefix = prefix;
}

void IndexRegistry::set_intermediate_file_keeping(bool keep_intermediates) {
    this->keep_intermediates = keep_intermediates;
}

void IndexRegistry::make_indexes(const vector<string>& identifiers) {
    
    // utility function, checks if an index is neither directly requested
    // or provided as input
    auto is_intermediate = [&](const IndexFile* index) {
        if (index->was_provided_directly()) {
            return false;
        }
        return find(identifiers.begin(), identifiers.end(),
                    index->get_identifier()) == identifiers.end();
    };
    // figure out the best plan to make the objectives from the inputs
    auto plan = make_plan(identifiers);
    
    // execute the plan
    for (auto& step : plan) {
        
        auto index = get_index(step.first);
        
        // note: recipes that are simply aliasing a more general file will sometimes
        // ignore the prefix
        string index_prefix;
        if (keep_intermediates || !is_intermediate(index)) {
            // we're saving this file, put it at the output prfix
            index_prefix = output_prefix;
        }
        else {
            // we're not saving this file, make it
            index_prefix = temp_file::get_dir() + "/" + sha1sum(index->get_identifier());
        }
        
        index->execute_recipe(step.second, index_prefix);
    }
    
    // clean up intermediate files
    if (!keep_intermediates) {
        // collect the names of everything we want to keep (i.e. considered non-
        // intermediate by at least one index)
        unordered_set<string> to_keep;
        for (const auto& registered_index : registry) {
            if (!is_intermediate(registered_index.second.get())) {
                for (auto& filename : registered_index.second->get_filenames()) {
                    to_keep.insert(filename);
                }
            }
        }
        
        // delete everything else
        for (const auto& registered_index : registry) {
            for (auto& filename : registered_index.second->get_filenames()) {
                if (!to_keep.count(filename)) {
                    std::remove(filename.c_str());
                }
            }
        }
    }
}

void IndexRegistry::register_index(const string& identifier, const string& suffix) {
    // Add this index to the registry
    if (identifier.empty()) {
        cerr << "error:[IndexRegistry] indexes must have a non-empty identifier" << endl;
        exit(1);
    }
    if (suffix.empty()) {
        cerr << "error:[IndexRegistry] indexes must have a non-empty suffix" << endl;
        exit(1);
    }
    if (registry.count(identifier)) {
        cerr << "error:[IndexRegistry] index registry contains a duplicated identifier: " << identifier << endl;
        exit(1);
    }
    if (registered_suffixes.count(suffix)) {
        cerr << "error:[IndexRegistry] index registry contains a duplicated suffix: " << suffix << endl;
        exit(1);
    }
    registry[identifier] = unique_ptr<IndexFile>(new IndexFile(identifier, suffix));
    registered_suffixes.insert(suffix);
}


void IndexRegistry::provide(const string& identifier, const string& filename) {
    provide(identifier, vector<string>(1, filename));
}

void IndexRegistry::provide(const string& identifier, const vector<string>& filenames) {
    get_index(identifier)->provide(filenames);
}

vector<string> IndexRegistry::completed_indexes() const {
    vector<string> indexes;
    for (const auto& index : registry) {
        if (index.second->is_finished()) {
            indexes.push_back(index.first);
        }
    }
    return indexes;
}

void IndexRegistry::register_recipe(const string& identifier,
                                    const vector<string>& input_identifiers,
                                    const function<vector<string>(const vector<const IndexFile*>&,const string&,const string&)>& exec) {
    vector<const IndexFile*> inputs;
    for (const auto& input_identifier : input_identifiers) {
        inputs.push_back(get_index(input_identifier));
    }
    get_index(identifier)->add_recipe(inputs, exec);
}

IndexFile* IndexRegistry::get_index(const string& identifier) {
    return registry.at(identifier).get();
}

const IndexFile* IndexRegistry::get_index(const string& identifier) const {
    return registry.at(identifier).get();
}

vector<string> IndexRegistry::dependency_order() const {
    
#ifdef debug_index_registry
    cerr << "finding topological order in dependency graph" << endl;
#endif
    
    // assign each index file an index in a vector (arbitrarily)
    unordered_map<string, size_t> graph_idx;
    vector<string> graph_label;
    for (const auto& idx_file : registry) {
        graph_idx[idx_file.first] = graph_label.size();
        graph_label.push_back(idx_file.first);
    }
    
    // build the dependency graph
    vector<vector<size_t>> dependency_graph(graph_label.size());
    for (size_t i = 0; i < dependency_graph.size(); ++i) {
        auto index = get_index(graph_label[i]);
        for (const auto& recipe : index->get_recipes()) {
            for (auto input : recipe.inputs) {
                dependency_graph[graph_idx[input->get_identifier()]].push_back(i);
            }
        }
    }
    
    // deduplicate any edges
    for (auto& adj : dependency_graph) {
        sort(adj.begin(), adj.end());
        adj.resize(unique(adj.begin(), adj.end()) - adj.begin());
    }
    
    // kahn's algorithm to determine a topological order
    vector<size_t> in_degree(dependency_graph.size(), 0);
    for (auto& adj : dependency_graph) {
        for (size_t i : adj) {
            ++in_degree[i];
        }
    }
    
    vector<size_t> stack;
    for (size_t i = 0; i < dependency_graph.size(); ++i) {
        if (in_degree[i] == 0) {
            stack.push_back(i);
        }
    }
    
    vector<size_t> order;
    while (!stack.empty()) {
        size_t i = stack.back();
        stack.pop_back();
        order.push_back(i);
        for (size_t j : dependency_graph[i]) {
            --in_degree[j];
            if (in_degree[j] == 0) {
                stack.push_back(j);
            }
        }
    }
    
    if (order.size() != dependency_graph.size()) {
        cerr << "error:[IndexFile] index dependency graph is not a DAG" << endl;
        exit(1);
    }
    
    // convert to return format
    vector<string> ordered_identifiers(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        ordered_identifiers[i] = graph_label[order[i]];
    }
    
#ifdef debug_index_registry
    for (const auto& identifier : ordered_identifiers) {
        cerr << "\t" << identifier << endl;
    }
#endif
    
    return ordered_identifiers;
}

vector<pair<string, size_t>> IndexRegistry::make_plan(const vector<string>& end_products) const {
    
#ifdef debug_index_registry
    cerr << "generating plan for indexes:" << endl;
    for (const auto& product : end_products) {
        cerr << "\t" << product << endl;
    }
#endif
    
    // get the dependency ordering of the indexes
    vector<string> identifier_order = dependency_order();
    unordered_map<string, size_t> dep_order_of_identifier;
    for (size_t i = 0; i < identifier_order.size(); ++i) {
        dep_order_of_identifier[identifier_order[i]] = i;
    }
    
    // TODO: I'm sure there's a more elegant implementation of this algorithm
    unordered_set<pair<string, size_t>> plan_elements;
    for (const auto& product : end_products) {
#ifdef debug_index_registry
        cerr << "making a plan for end product " << product << endl;
#endif
        
        // records of (identifier, lowest level requester, ordinal index of recipe selected)
        vector<tuple<size_t, size_t, size_t>> plan_path;
        
        // map dependency priority to lowest level priority that requested this and
        // the number of requesters
        map<size_t, pair<size_t, size_t>, greater<size_t>> queue;
        queue[dep_order_of_identifier[product]] = pair<size_t, size_t>(identifier_order.size(), 1);
        
        while (!queue.empty()) {
#ifdef debug_index_registry_path_state
            cerr << "new iteration, path:" << endl;
            for (auto pe : plan_path) {
                cerr << "\t" << identifier_order[get<0>(pe)] << ", requester: " << (get<1>(pe) == identifier_order.size() ? string("PLAN TARGET") : identifier_order[get<1>(pe)]) << ", recipe " << get<2>(pe) << endl;
            }
            cerr << "state of queue:" << endl;
            for (auto q : queue) {
                cerr << "\t" << identifier_order[q.first] << ", requester: " << (q.second.first == identifier_order.size() ? string("PLAN TARGET") : identifier_order[q.second.first]) << ", num requesters " << q.second.second << endl;
            }
#endif
            
            // get the latest file in the dependency order
            // that we have left to build
            auto it = queue.begin();
            plan_path.emplace_back(it->first, it->second.first, 0);
            
#ifdef debug_index_registry
            cerr << "dequeue " << identifier_order[it->first] << " requested from " << (it->second.first == identifier_order.size() ? string("PLAN TARGET") : identifier_order[it->second.first]) << " and " << (it->second.second - 1) << " other indexes" << endl;
#endif
            queue.erase(it);
            
            if (get_index(identifier_order[get<0>(plan_path.back())])->is_finished()) {
                // this index has been provided, we don't need to use a recipe
#ifdef debug_index_registry
                cerr << "file has been provided as input" << endl;
#endif
                continue;
            }
            else if (!get_index(identifier_order[get<0>(plan_path.back())])->get_recipes().empty()) {
                
                // this index can be created by a recipe
                const auto& recipe = get_index(identifier_order[get<0>(plan_path.back())])->get_recipes().front();
#ifdef debug_index_registry
                cerr << "index can be made by a recipe requiring";
                for (auto input : recipe.inputs) {
                    cerr << " " << input->get_identifier();
                }
                cerr << endl;
#endif
                
                for (auto input : recipe.inputs) {
                    size_t dep_order = dep_order_of_identifier[input->get_identifier()];
                    auto f = queue.find(dep_order);
                    if (f == queue.end()) {
                        // no lower-level index has requested this one yet
                        queue[dep_order] = pair<size_t, size_t>(get<0>(plan_path.back()), 1);
                    }
                    else {
                        // record that one more index is requesting this one
                        f->second.second++;
                    }
                    
                }
            }
            else {
                // we've reached a file that needs to be provided but we don't have it,
                // so now we backtrack until hitting something that has remaining
                // lower priority recipes
#ifdef debug_index_registry
                cerr << "file cannot be made from existing inputs" << endl;
#endif
                while (!plan_path.empty() &&
                       get<2>(plan_path.back()) == get_index(identifier_order[get<0>(plan_path.back())])->get_recipes().size()) {
                    // there are no remaining recipes to build the last index in the plan
                    
                    // remove items off the plan path until we get to the index that requested
                    // this one
                    size_t requester = get<1>(plan_path.back());
#ifdef debug_index_registry
                    cerr << "pruning path to previous requester: " << (requester == identifier_order.size() ? "PLAN TARGET" : identifier_order[requester]) << endl;
#endif
                    while (!plan_path.empty() && get<0>(plan_path.back()) != requester) {
                        
                        auto index = get_index(identifier_order[get<0>(plan_path.back())]);
                        if (!index->is_finished() && get<2>(plan_path.back()) < index->get_recipes().size()) {
                            // this index was using a recipe, we need to update its dependencies
                            // that are currently in the queue
                            const auto& recipe = index->get_recipes().at(get<2>(plan_path.back()));
                            for (auto input : recipe.inputs) {
                                size_t input_dep_order = dep_order_of_identifier[input->get_identifier()];
                                auto q = queue.find(input_dep_order);
                                if (q != queue.end()) {
                                    // there is now one fewer index requesting this index as input
                                    --q->second.second;
                                    if (q->second.second == 0) {
                                        // this is the only index that's requesting this queued index,
                                        // so we can remove it from the queue
                                        queue.erase(q);
                                    }
                                }
                            }
                        }
                        
                        plan_path.pop_back();
                    }
                    
                    if (!plan_path.empty()) {
                        // the requester should now use its next highest priority recipe
                        auto index = get_index(identifier_order[get<0>(plan_path.back())]);
                        if (!index->is_finished() && get<2>(plan_path.back()) < index->get_recipes().size()) {
                            // this index was using a recipe, we need to update its dependencies
                            // that are currently in the queue
                            const auto& recipe = index->get_recipes().at(get<2>(plan_path.back()));
                            for (auto input : recipe.inputs) {
                                size_t input_dep_order = dep_order_of_identifier[input->get_identifier()];
                                auto q = queue.find(input_dep_order);
                                if (q != queue.end()) {
                                    // there is now one fewer index requesting this index as input
                                    --q->second.second;
                                    if (q->second.second == 0) {
                                        // this is the only index that's requesting this queued index,
                                        // so we can remove it from the queue
                                        queue.erase(q);
                                    }
                                }
                            }
                        }
                        ++get<2>(plan_path.back());
                    }
                }
                
                if (!plan_path.empty()) {
                    const auto& recipe = get_index(identifier_order[get<0>(plan_path.back())])->get_recipes()[get<2>(plan_path.back())];
                    
#ifdef debug_index_registry
                    cerr << "advancing to recipe " << get<2>(plan_path.back()) << " for index " << identifier_order[get<0>(plan_path.back())] << ", which requires";
                    for (auto input : recipe.inputs) {
                        cerr << " " << input->get_identifier();
                    }
                    cerr << endl;
#endif
                    for (auto input : recipe.inputs) {
                        size_t dep_order = dep_order_of_identifier[input->get_identifier()];
                        auto f = queue.find(dep_order);
                        if (f == queue.end()) {
                            // no lower-level index has requested this one yet
                            queue[dep_order] = pair<size_t, size_t>(get<0>(plan_path.back()), 1);
                        }
                        else {
                            // record that one more index is requesting this one
                            f->second.second++;
                        }
                        
                    }
                }
            }
            
        }
        
#ifdef debug_index_registry
        cerr << "final plan path for index " << product << ":" << endl;
        for (auto path_elem : plan_path) {
            cerr << "\t" << identifier_order[get<0>(path_elem)] << ", from " << (get<1>(path_elem) == identifier_order.size() ? "PLAN START" : identifier_order[get<1>(path_elem)]) << ", recipe " << get<2>(path_elem) << endl;
        }
#endif
        
        if (plan_path.empty()) {
            // we don't have enough of the inputs to create this index
            throw InsufficientInputException(product, *this);
        }
        
        // record the elements of this plan
        for (size_t i = 0; i < plan_path.size(); ++i) {
            plan_elements.emplace(identifier_order[get<0>(plan_path[i])], get<2>(plan_path[i]));
        }
    }
    
    // convert the aggregated plan elements into a forward ordered plan
    vector<pair<string, size_t>> plan(plan_elements.begin(), plan_elements.end());
    sort(plan.begin(), plan.end(), [&](const pair<string, size_t>& a, const pair<string, size_t>& b) {
        return dep_order_of_identifier[a.first] < dep_order_of_identifier[b.first];
    });
#ifdef debug_index_registry
    cerr << "full plan including provided files:" << endl;
    for (auto plan_elem : plan) {
        cerr << "\t" << plan_elem.first << " " << plan_elem.second << endl;
    }
#endif
    
    // and remove the input data from the plan
    plan.resize(remove_if(plan.begin(), plan.end(), [&](const pair<string, size_t>& recipe_choice) {
        return get_index(recipe_choice.first)->is_finished();
    }) - plan.begin());
    return plan;
}

string IndexRegistry::to_dot() const {
    return to_dot(vector<string>());
}

string IndexRegistry::to_dot(const vector<string>& targets) const {
    
    
    stringstream strm;
    strm << "digraph recipegraph {" << endl;
    
    unordered_set<string> plan_targets(targets.begin(), targets.end());
    unordered_set<pair<string, size_t>> plan_elements;
    unordered_set<string> plan_indexes;
    if (!targets.empty()) {
        vector<pair<string, size_t>> plan;
        try {
            plan = make_plan(targets);
        }
        catch (InsufficientInputException ex) {
            strm << "labelloc=\"t\";" << endl;
            strm << "label=\"Insufficient input to create targets\";" << endl;
        }
        for (const auto& plan_elem : plan) {
            plan_elements.insert(plan_elem);
            plan_indexes.insert(plan_elem.first);
        }
    }
    
    unordered_map<string, string> index_to_dot_id;
    size_t index_idx = 0;
    for (const auto& index_file : registry) {
        index_to_dot_id[index_file.first] = "I" + to_string(index_idx);
        ++index_idx;
        strm << index_to_dot_id[index_file.first] << "[label=\"" << index_file.first << "\" shape=box";
        if (index_file.second->is_finished()) {
            strm << " style=\"filled,bold\" fillcolor=lightgray";
        }
        else if (plan_targets.count(index_file.first)) {
            strm << " style=\"filled,bold\" fillcolor=lightblue";
        }
        else if (plan_indexes.count(index_file.first)) {
            strm << " style=bold";
        }
        strm << "];" << endl;
    }
    string unselected_col = targets.empty() ? "black" : "gray33";
    size_t recipe_idx = 0;
    for (const auto& index_file : registry) {
        const auto& recipes = index_file.second->get_recipes();
        for (size_t priority_idx = 0; priority_idx < recipes.size(); ++priority_idx, ++recipe_idx) {
            const auto& recipe = recipes[priority_idx];
            string recipe_dot_id = "R" + to_string(recipe_idx);
            if (plan_elements.count(make_pair(index_file.first, priority_idx))) {
                strm << recipe_dot_id << "[label=\"" << priority_idx << "\" shape=circle style=bold];" << endl;
                strm << recipe_dot_id << " -> " << index_to_dot_id[index_file.first] << "[style=bold];" << endl;
            }
            else {
                strm << recipe_dot_id << "[label=\"" << priority_idx << "\" shape=circle];" << endl;
                strm << recipe_dot_id << " -> " << index_to_dot_id[index_file.first] << " [color=" << unselected_col << "];" << endl;
            }
            for (const auto& input : recipe.inputs) {
                if (plan_elements.count(make_pair(index_file.first, priority_idx))) {
                    strm << index_to_dot_id[input->get_identifier()] << " -> " << recipe_dot_id << "[style=bold];" << endl;
                }
                else {
                    strm << index_to_dot_id[input->get_identifier()] << " -> " << recipe_dot_id << " [color=" << unselected_col << "];" << endl;
                }
            }
        }
    }
    strm << "}" << endl;
    return strm.str();
}

IndexFile::IndexFile(const string& identifier, const string& suffix) : identifier(identifier), suffix(suffix) {
    // nothing more to do
}

bool IndexFile::is_finished() const {
    return !filenames.empty();
}

const string& IndexFile::get_identifier() const {
    return identifier;
}

const vector<string>& IndexFile::get_filenames() const {
    return filenames;
}

const vector<IndexRecipe>& IndexFile::get_recipes() const {
    return recipes;
}

void IndexFile::provide(const vector<string>& filenames) {
    this->filenames = filenames;
    provided_directly = true;
}

bool IndexFile::was_provided_directly() const {
    return provided_directly;
}

void IndexFile::execute_recipe(size_t recipe_priority, const string& prefix) {
    assert(recipe_priority < recipes.size());
    auto& recipe = recipes[recipe_priority];
    for (auto input : recipe.inputs) {
        assert(input->is_finished());
    }
    filenames = recipe.execute(prefix, this->suffix);
}

void IndexFile::add_recipe(const vector<const IndexFile*>& inputs,
                           const function<vector<string>(const vector<const IndexFile*>&,const string&,const string&)>& exec) {
    recipes.emplace_back(inputs, exec);
}

IndexRecipe::IndexRecipe(const vector<const IndexFile*>& inputs,
                         const function<vector<string>(const vector<const IndexFile*>&,const string&,const string&)>& exec) :
    exec(exec), inputs(inputs)
{
    // nothing more to do
}

vector<string> IndexRecipe::execute(const string& prefix, const string& suffix) {
    return exec(inputs, prefix, suffix);
}

InsufficientInputException::InsufficientInputException(const string& target,
                                                       const IndexRegistry& registry) :
    runtime_error("Insufficient input to create " + target), target(target), inputs(registry.completed_indexes())
{
    // nothing else to do
}

const char* InsufficientInputException::what() const throw () {
    stringstream ss;
    ss << "Inputs" << endl;
    for (const auto& input : inputs) {
        ss << "\t" << input << endl;
    }
    ss << "are insufficient to create target index " << target << endl;
    return ss.str().c_str();
}

}

