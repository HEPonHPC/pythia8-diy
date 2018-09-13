#ifndef DIYGEN_GenericBlock_HPP
#define DIYGEN_GenericBlock_HPP

#include "Pythia8/Pythia.h"
#include "Pythia8Plugins/HepMC2.h"

#include "Rivet/AnalysisHandler.hh"
#include "Pythia8Plugins/LHAMadgraph.h" // MadGraph
#undef foreach // This line prevents a clash of definitions of rivet's legacy foreach with that of DIY

#include "HepMC/IO_GenEvent.h"
#include "Tools.hpp"
#include <math.h>

using namespace std;
using namespace Pythia8;

// --------------- generic block definition ----------------
// B = Bounds definition
// S = State definition
// D = dataa definition

template <typename B, typename S, typename D>
struct GenericBlock
{
  typedef B bounds_type;
  typedef S state_type; 
  typedef D data_type;
  typedef GenericBlock<B,S,D> block_type;

  GenericBlock(bounds_type const& b):bounds(b),ah(NULL) { }

  // ---------------------------------------
  // standard functions for block processing
  static void* create() { return new block_type; }
  static void destroy(void* b) { delete static_cast<block_type*>(b); }

  static void save(const void* b, diy::BinaryBuffer& bb)
  {
    block_type const* bp = static_cast<block_type const*>(b);
    diy::save(bb, bp->bounds);
    diy::save(bb, bp->state);
    diy::save(bb, bp->data);
  }
  static void load(void* b, diy::BinaryBuffer& bb) 
  {
    block_type* bp = static_cast<block_type*>(b);
    diy::load(bb, bp->bounds);
    diy::load(bb, bp->state);
    diy::load(bb, bp->data);
  }

  // -------
  // this is the protocol for using the generic reduction function
  // this should be reduced to a minimum number of options

  // get access to the reduce buffer and the block state that is filled by processing
  data_type const& reduceData() const { return data; }
  data_type& reduceBuffer() { return buffer; }
  // a = a + b    (might want to permit other combining function to be passed in here)
  static void reduce(data_type& a, data_type const& b)
  {
    std::transform(a.cbegin(),a.cend(), b.cbegin(),
		   a.begin(), std::plus<typename data_type::value_type>());
  }
  // add "other" into my block data
  void reduce(data_type const& other) { reduce(data,other); }
  // add my block data into "other"
  void altreduce(data_type& other) const { reduce(other,data); }
  // add "other" into my buffer
  void reduce_buffer(data_type const& other) { reduce(buffer,other); }
  // add my buffer data into "other"
  void altreduce_buffer(data_type& other) const { altreduce(other,buffer); }

  std::string dir_name(int dir_idx)
  {
	  int mag;
	  if(dir_idx < 1) mag = 0;
	  else if(dir_idx == 1) mag = 1;
	  else {
		  mag = ceil(log10(dir_idx));
		  if(dir_idx % 10 == 0) mag += 1;
	  }

	  std::string out("000000");
	  std::cout << " is with magnitude of " << mag << " --> ";
	  if(mag >= 6) {
		  return std::to_string(dir_idx);
	  } else {
		  return out.substr(0, 6-mag)+std::to_string(dir_idx);
	  }
  }


  void init_data(const diy::Master::ProxyWithLink& cp,
		  int nConfigs,
		  int nEvents,
		  int seed,
		  std::string& indir,
		  std::string& pythia_conf, 
		  std::vector<std::string>& analyses,
		  std::string& f_out,
		  std::string& detector_conf, 
		  std::string& mg5_conf, 
		  bool verbose
		  )
  {
	  if (cp.gid() > nConfigs - 1) return;

	  string dir;
	  if(nConfigs > 1) {
		int dim = bounds.min.size();
		int config_idx = bounds.min[1];
		string dir(indir +"/"+ dir_name(config_idx));
	  } else {
		 dir = indir; 
	  }
	  create_state(dir, nEvents, seed, pythia_conf, analyses, f_out, detector_conf, mg5_conf, verbose);
  }

  void create_state(
		  std::string& indir,
		  int nEvents,
		  int seed,
		  std::string& pythia_conf, 
		  std::vector<std::string>& analyses,
		  std::string& f_out,
		  std::string& detector_conf, 
		  std::string& mg5_conf, 
		  bool verbose
		  )
  {
	  bool f_ok;

	  std::vector<std::string> physConfig;
	  physConfig.clear();
	  f_ok = readConfig(indir+"/"+pythia_conf, physConfig,  verbose);
	  if(!f_ok) throw(std::system_error(ENOENT, std::iostream_category(), indir+"/"+pythia_conf));	


	  std::vector<std::string> mg5Config;
	  mg5Config.clear();
	  bool use_mg5 = readConfig(indir+"/"+mg5_conf, mg5Config,  verbose);

	  state = {1, nEvents, seed, 1, physConfig, analyses, indir+"/"+f_out, detector_conf, mg5Config, use_mg5};
  }


  // -----------
  // block data
  bounds_type bounds;
  state_type state;
  data_type data;
  data_type buffer;

  Pythia pythia; // The generator
  // Shorthand for the event record in pythia.
  Event& event = pythia.event;
  int nEvents;
  Rivet::AnalysisHandler *ah;
  HepMC::Pythia8ToHepMC ToHepMC;
  std::vector<std::string> physConfig; // This is the pythia steering card, line by line

  LHAupMadgraph* mg5; // NLO generator

	private:
  GenericBlock() { }
};


#endif
