#include <Grappa.hpp>
#include <GlobalAllocator.hpp>
#include <Delegate.hpp>
#include <PerformanceTools.hpp>
#include <Statistics.hpp>
#include <Collective.hpp>

#define _GRAPPA 1
#include "../../graph500/generator/make_graph.h"

#include "../../graph500/generator/utils.h"
#include "../prng.h"
#include "common.h"

#include "timer.h"
#include "options.h"
#include "graph.hpp"
#include "verify.hpp"

#include <stack>
#include <vector>

using namespace Grappa;

/////////////////////////////////
// Declarations
double make_bfs_tree(GlobalAddress<Graph<VertexP>> g_in, GlobalAddress<int64_t> _bfs_tree, int64_t root);
long cc_benchmark(GlobalAddress<Graph<>> g);

/////////////////////////////////
// Globals

static double generation_time;
static double construction_time;
static double bfs_time[NBFS_max];
static int64_t bfs_nedge[NBFS_max];

DEFINE_int64(scale, 3, "Graph500 scale (graph will have ~2^scale vertices)");
DEFINE_int64(edgefactor, 1, "Approximate number of edges in graph will be 2*2^(scale)*edgefactor");
DEFINE_int64(nbfs, 8, "Number of BFS traversals to do");

DEFINE_string(bench, "bfs", "Specify what graph benchmark to execute.");

DEFINE_bool(verify, true, "Do verification. Note: `--noverify` is equivalent to `--verify=(false|no|0)`");

DEFINE_double(beamer_alpha, 20.0, "Beamer BFS parameter for switching to bottom-up.");
DEFINE_double(beamer_beta, 20.0, "Beamer BFS parameter for switching back to top-down.");


//#define TRACE 0 //outputs traversal trace if high


//-----[ISOMORPHIC PATH ALGORITHM DEFINITION]-----//

/* Recursive call definition for isomorphic path search
 * Given a root and pattern sequence, will recursively call until the pattern is empty, or there are no more
 * vertices to traverse.
 * Searches for patterns with the designated color pattern and returns the number of paths with the specified
 * pattern coloring starting from the specified node
 * Parameters:
 *    GlobalAddress<Graph<VertexP>> g - a graph representation
 *    int64_t node - the node to process in the traversal
 *    std::stack<int64_t> pattern - a stack of the remaining colors that need to be matched in the path
 *    std::visited_notes<int64_t> - a list of nodes that have already been visited in the path search traversal
 */
int find_iso_paths(GlobalAddress<Graph<VertexP>> g, int64_t node, std::stack<int64_t> &pattern, std::vector<int64_t> &visited_nodes) {

  //if the pattern size is zero, something went wrong so abort
  if (pattern.size() == 0) {
    return 0;
  }
  int64_t color = pattern.top(); //get the next color in the pattern sequence
  
  //get the color of the current vertex being traversed
  VertexP vp = delegate::read(g->vs+node);
  int64_t vcolor = (int64_t) vp.vertex_data;

  //get the number of adjacent vertices for this node
  int64_t nadj = vp.nadj;

  //#ifdef TRACE
  LOG(INFO) << "Traversed node: " << node << " with color " << (int64_t) vcolor << " patc = " << color << " vtxc = " << vcolor << " pattern depth: " << pattern.size();
  //#endif

  //check if the vertex color matches the next color in the pattern
  if (color == vcolor) {
    //if the length of the pattern to match is 1, and it matches, we're done
    if (pattern.size() == 1) {
      LOG(INFO) << "[PATH FOUND] Path ending at node: " << node;
      return 1; //end of path, return found 1 path
    }  
    else { //recursive case
      int paths = 0;
      
      //iterate over all adjacent nodes
      for (int64_t i = 0; i < nadj; i++) {
	//add the current node to the list of visited nodes
	visited_nodes.push_back(node);

	//get the next node in the adjacency array
	int64_t next_node = delegate::call(g->vs+node, [i](VertexP *v) {return v->local_adj[i];});
	
	//if not visited already or current node, traverse
	if(std::find(visited_nodes.begin(), visited_nodes.end(), next_node) == visited_nodes.end()) {
	  pattern.pop();
	  paths += find_iso_paths(g, next_node, pattern, visited_nodes);
	  pattern.push(color);
	}
	
	//remove the current node from list of visited nodes
	visited_nodes.pop_back();
      }
      return paths;
    }
  } else { //color does not match so return 0 paths
    return 0;
  }

  //failsafe
  return 0;
}


//applies delegate call to write colors to nodes
//TODO: make this a forall_localized call
void set_color(GlobalAddress<Graph<VertexP>> g) {
  int64_t num_vtx = g->nv;
  for (int64_t n = 0; n < num_vtx; n++) {
    delegate::call(g->vs+n, [](VertexP* v) { v->vertex_data = (void *) 1; });
  }
}

//sets the color to the node number parity
void set_color2(GlobalAddress<Graph<VertexP>> g) {
  int64_t num_vtx = g->nv;
  for (int64_t n = 0; n < num_vtx; n++) {
    int64_t color = n % 2;
    delegate::call(g->vs+n, [color](VertexP* v) { v->vertex_data = (void *) color; });
  }
}

//generates a stack representation of the pattern from a vector
std::stack<int64_t> generate_pattern(std::vector<int64_t> pattern) {
  std::stack<int64_t> p;
  for (int64_t i = pattern.size() - 1; i >= 0; i--) {
    p.push(pattern[i]);
  }
  return p;
}

//grappa version of the isomorphic path implementation
int grappa_iso_paths(tuple_graph &tg, GlobalAddress<Graph<>> generic_graph, std::vector<int64_t> vpattern) {
  //set the vertices to have color 1
  auto g = Graph<>::transform_vertices<VertexP>(generic_graph, [](VertexP & v) { v.parent(1); });
  set_color2(g);
  //set_color2(g);
  Graph<VertexP>::dump(g);
  
  //forall_local
  int64_t grappa_paths = 0;
  int seq_paths = 0;
  int paths = 0;

  //holder for path results
  LOG(INFO) << "Allocating " << g->nv << " result slots...\n";
  GlobalAddress<int64_t> results = global_alloc<int64_t>(g->nv);

  //allocate space for putting the pattern in global memory
  //TODO: see if there's just a way to pass with the lambda call
  GlobalAddress<int64_t> global_pattern = global_alloc<int64_t>(vpattern.size());

  //write the pattern to global memory
  for (int i = 0; i < vpattern.size(); i++) {
    delegate::write(global_pattern+i, vpattern[i]);
  }

  int64_t psize = vpattern.size();

  //GRAPPA isopaths call
  forall_localized(g->vs, g->nv, [g, results, global_pattern, psize] (int64_t i, VertexP &v) {
      //generate a node copy of the visited nodes list
      std::vector<int64_t> visited_nodes;
      
      //generate a node copy of the pattern
      std::stack<int64_t> pattern;

      //generate a local pattern in stack form
      for (int64_t i = psize - 1; i >= 0; i--) {
	int64_t pcolor = delegate::read(global_pattern+i);
	pattern.push(pcolor);
      }

      //run the path search on the node
      #ifdef TRACE
      LOG(INFO) << "[GRAPPA] Starting path search at node: " << i; 
      #endif
      int num_paths = find_iso_paths(g, i , pattern, visited_nodes);
      #ifdef TRACE
      LOG(INFO) << "[GRAPPA] Number of paths from node " << i << ": " << num_paths;
      #endif
      delegate::write(results+i, num_paths);
    });

  //collect the result path count
  for (int j = 0; j < g->nv; j++) {
    int64_t num_paths = delegate::read(results+j);
    #ifdef TRACE
    LOG(INFO) << "From node " << j << " counted " << num_paths << " routes...";
    #endif
    grappa_paths += num_paths;
  }

  LOG(INFO) << "[Grappa] Total number of isomorphic paths: " << grappa_paths;
  return grappa_paths;
}

//reference implementation of the isomorphic path implementation
//purely sequential version
int ref_iso_paths(tuple_graph &tg, GlobalAddress<Graph<>> generic_graph, std::vector<int64_t> vpattern) {
  auto g = Graph<>::transform_vertices<VertexP>(generic_graph, [](VertexP & v) { v.parent(1); });
  //set_color(g);
  set_color2(g);
  Graph<VertexP>::dump(g);
  
  //generate a node copy of the pattern
  std::stack<int64_t> pattern;
  pattern = generate_pattern(vpattern);

  int paths = 0;
  int seq_paths = 0;

  //iterate over all vertices
  for (int64_t n = 0; n < g->nv; n++) {
    std::vector<int64_t> visited_nodes;
    #ifdef TRACE
    LOG(INFO) << "[SEQ] Starting path search at node: " << n;
    #endif
    paths = find_iso_paths(g, n, pattern, visited_nodes);
    #ifdef TRACE
    LOG(INFO) << "[SEQ] Number of paths from node: " << n << " = " << paths;
    #endif
    seq_paths += paths;
  }
  LOG(INFO) << "[Sequential] Total number of isomorphic paths: " << seq_paths;
  return seq_paths;
}

//-----[PRIMARY USER MAIN]-----//
/* Performs graph construction and setup.
 * Calls the grappa implementation and single core implementation and compares results.
 */
void user_main(void * ignore) {
  double t, start_time;
  start_time = walltime();
  
  int64_t nvtx_scale = ((int64_t)1)<<FLAGS_scale;
  int64_t desired_nedge = nvtx_scale * FLAGS_edgefactor;
  
  LOG(INFO) << "scale = " << FLAGS_scale << ", NV = " << nvtx_scale << ", NE = " << desired_nedge;
  
  // make raw graph edge tuples
  tuple_graph tg;
  tg.edges = global_alloc<packed_edge>(desired_nedge);
  
  t = walltime();
  make_graph( FLAGS_scale, desired_nedge, userseed, userseed, &tg.nedge, &tg.edges );
  generation_time = walltime() - t;
  LOG(INFO) << "graph_generation: " << generation_time;
  
  t = walltime();
  auto g = Graph<>::create(tg);
  construction_time = walltime() - t;
  LOG(INFO) << "construction_time: " << construction_time;
  
  int grappa_paths = 0;
  int ref_paths = 0;

  //define a search path pattern
  std::vector<int64_t> path_pattern {1,1,1};

  //run the isomorphics paths routine
  grappa_paths = grappa_iso_paths(tg, g, path_pattern);

  //run the sequential reference version of isomorphic paths routine
  ref_paths = ref_iso_paths(tg, g, path_pattern);

  LOG(INFO) << "//-----[ISOMORPHIC PATHS EXECUTION REPORT-----//";
  LOG(INFO) << "[Grappa] Isomorphic paths: " << grappa_paths;
  LOG(INFO) << "[Sequential] Isomorphic paths: " << ref_paths;

  //dump the graph if it's small enough
  if (FLAGS_scale < 5) {
    LOG(INFO) << "//-----[GRAPH EDGE LIST DUMP]-----//";
    Graph<>::dump(g);
  } 

  //clean up
  g->destroy();
  global_free(tg.edges);
    
  LOG(INFO) << "total_runtime: " << walltime() - start_time;
}

int main(int argc, char** argv) {
  Grappa_init(&argc, &argv);
  Grappa_activate();

  Grappa_run_user_main(&user_main, (void*)NULL);
  
  Grappa_finish(0);
  return 0;
}
