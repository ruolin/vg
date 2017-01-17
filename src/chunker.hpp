#ifndef CHUNKER_H
#define CHUNKER_H

#include <iostream>
#include <map>
#include <chrono>
#include <ctime>
#include "lru_cache.h"
#include "vg.hpp"
#include "xg.hpp"
#include "json2pb.h"
#include "region.hpp"
#include "index.hpp"

namespace vg {

using namespace std;


/** Chunk up a graph along a path, using a given number of
 * context expansion steps to fill out the chunks.  Most of the 
 * work done by exising xg functions. For gams, the rocksdb
 * index is also required. 
 */
class PathChunker {

public:
   
    // xg index used for all path splitting and subgraphing operations
    xg::XG* xg;
    // number of gams to write at once
    size_t gam_buffer_size = 1000;

    PathChunker(xg::XG* xg = NULL);
    ~PathChunker();

    /** Extract subgraph corresponding to given path region into its 
     * own vg graph, and send it to out_stream.  Returns the starting position
     * of the returned chunk in the path (region.seq).  This can differ from
     * region.start if the latter doesn't fall on the tip of a node. 
     *
     * NOTE: we follow convention of Region coordinates being 1-based 
     * inclusive.  The returned coordinate is 0-based, however */
    int64_t extract_subgraph(const Region& region, int context, VG& subgraph);

    /** Extract all alignments that touch a node in a subgraph and write them 
     * to an output stream using the rocksdb index (and this->gam_buffer_size) */
    int64_t extract_gam_for_subgraph(VG& subgrpah, Index& index, ostream* out_stream);
};


}

#endif
