
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <random>
#include <typeinfo>
#include <ctime>
#include <iomanip> // put_time

#include <diy/master.hpp>
#include <diy/reduce.hpp>
#include <diy/partners/merge.hpp>
#include <diy/decomposition.hpp>
#include <diy/assigner.hpp>
#include <diy/mpi.hpp>
#include <diy/serialization.hpp>
#include <diy/partners/broadcast.hpp>
#include <diy/reduce-operations.hpp>



#include "config.hpp"
#include "GenericBlock.hpp"
#include "Reduce.hpp"
#include "Tools.hpp"
#include "CalcConfig.hpp"
#include "Serialisation.hpp"

#include "YODA/ReaderYODA.h"
#include "YODA/WriterYODA.h"
#include "YODA/AnalysisObject.h"

#include "Pythia8/Pythia.h"
#include "Pythia8Plugins/HepMC2.h"
#include "Pythia8Plugins/LHAMadgraph.h" // For MadGraph5 
#include "Rivet/AnalysisHandler.hh"
#undef foreach // This line prevents a clash of definitions of rivet's legacy foreach with that of DIY


#include <chrono> // to time running time, since c++11

#include "HepMC/IO_GenEvent.h"




using namespace std;
using namespace Pythia8;
#include "opts.h"

using namespace std;


typedef diy::DiscreteBounds Bounds;
typedef diy::RegularGridLink RCLink;

typedef GenericBlock<Bounds, PointConfig, AnalysisObjects> Block;
typedef ConfigBlockAdder<Bounds, RCLink, Block> AddBlock;


void print_block(Block* b,                             // local block
		const diy::Master::ProxyWithLink& cp, // communication proxy
		bool verbose)                         // user-defined additional arguments
{
	fmt::print(stderr, "\n[{}] Pythia configuration\n", cp.gid());
	for (auto s : b->state.conf) {
		fmt::print(stderr, "\t[{}]: {}\n", cp.gid(), s);
	}
	fmt::print(stderr, "\n[{}] Rivet analyses:\n", cp.gid());
	for (auto s : b->state.analyses) {
		fmt::print(stderr, "\t[{}]: {} ", cp.gid(), s);
	}
	fmt::print(stderr, "[{}] detector configuration: {}\n", cp.gid(), b->state.detector_conf);
	fmt::print(stderr, "[{}] output: {}\n", cp.gid(), b->state.f_out);
	if(b->state.use_mg5){
		fmt::print(stderr, "MadGraph5:\n");
		for (auto s : b->state.mg5_conf) {
			fmt::print(stderr, "\t[{}]: {}\n", cp.gid(), s);
		}
	} else {
		fmt::print(stderr, "[{}] MadGraph5 not Used\n", cp.gid());
	}
	fmt::print(stderr, "\n");
}


void process_block(Block* b, diy::Master::ProxyWithLink const& cp,  bool verbose)
{
	auto start = std::chrono::system_clock::now();
	std::time_t tttt = std::time(nullptr);

	fmt::print(stderr, "[{}] Start to process {} \n", cp.gid(), 
			std::put_time(std::localtime(&tttt), "%c %Z") );
	// This make rivet only report ERRORs
	// TODO: can we have a global flag to steer verbosity of all moving parts?
	if (!verbose) Rivet::Log::setLevel("Rivet", Rivet::Log::ERROR);

	// Explicit desctruction and recreation of pythia --- this is important when running multiple
	// physics configs!!! https://stackoverflow.com/questions/1124634/call-destructor-and-then-constructor-resetting-an-object
	(&b->pythia)->~Pythia();
	new (&b->pythia) Pythia();
	// Minimise pythia's output

	if (verbose) b->pythia.readString("Print:quiet = off");
	else b->pythia.readString("Print:quiet = on");




	if(b->state.use_mg5) {
		// Add MadGraph
		if(!b->mg5) {
			try{
				b->mg5 = new LHAupMadgraph(&(b->pythia), true, basepath(b->state.f_out)+"/amcatnlorun", "mg5_aMC");
			} catch(const std::bad_alloc& e) {
				fmt::print(stderr, "{} cannot iniatiate LHAupMadgraph\n", cp.gid());
				return;
			}
			for(auto s: b->state.mg5_conf) b->mg5->readString(s);
			// setup seed and number of events....
			b->mg5->readString(" set nevents " + std::to_string(b->state.num_events));
			b->mg5->readString(" set iseed " + std::to_string(b->state.seed+cp.gid()));
		} else {
			// If this is already, assuming configruations and such are already read...
			fmt::print(stderr, "{} LHAupMadgraph already there!\n", cp.gid());
		}
		try {
			b->pythia.setLHAupPtr(b->mg5);
		} catch (const std::bad_alloc& e){
			fmt::print(stderr, "{} cannot setup LHA MadGraph...\n", cp.gid());
		}
	} else {
		// Configure pythia with a vector of strings
		for (auto s  : b->state.conf) b->pythia.readString(s);

		// Py8 random seed for this block read from point config
		b->pythia.readString("Random:setSeed = on");
		b->pythia.readString("Random:seed = " + std::to_string(b->state.seed+cp.gid()));
		b->pythia.readString("Main:numberOfEvents = " + std::to_string(b->state.num_events));
	}


	// All configurations done, initialise Pythia
	// b->pythia.initPtrs(); // TODO --- is this really necessary here?

	b->pythia.init();

	// b->pythia.settings.listChanged();

	// Delete the AnalysisHandlerPtr to ensure there is no memory
	if (b->ah)
	{
		delete b->ah;
	}
	b->ah = new Rivet::AnalysisHandler;

	// Add all anlyses to rivet
	// TODO: we may want to feed the "ignore beams" switch as well
	for (auto a : b->state.analyses) b->ah->addAnalysis(a);

	// Terminate the program if analysis is not available
	if(b->ah->analyses().empty()) {
		throw std::range_error::range_error("no analysis provided, I don't run!");
	}


	// Update Rivet simulation
	if (b->state.detector_conf != "") {
		b->ah->setSimulationFile(b->state.detector_conf.c_str());
	}

	// The event loop
	// int nAbort = b->pythia.mode("Main:timesAllowErrors");
	int nAbort = (int) b->state.num_events * 0.005; // 0.005 is a random fraction
	nAbort = nAbort > 10? nAbort: 10;

	int iAbort = 0;
	if (verbose) fmt::print(stderr, "[{}] generating {} events\n", cp.gid(),  b->state.num_events);
    for (unsigned int iEvent = 0; iEvent < b->state.num_events; ++iEvent) {
		if (!b->pythia.next()) {
			if (++iAbort < nAbort) continue;
			break;
		}
		HepMC::GenEvent* hepmcevt = new HepMC::GenEvent();
		b->ToHepMC.fill_next_event( b->pythia, hepmcevt );

		try {b->ah->analyze( *hepmcevt ) ;} catch (const std::exception& e)
		{
			if (verbose) fmt::print(stderr, "[{}] exception in analyze: {}\n", cp.gid(), e.what());
		}
		delete hepmcevt;
		if (iEvent%1000 == 0 && cp.gid()==0) fmt::print(stderr, "[{}]  {}/{} \n", cp.gid(),  iEvent, b->state.num_events);;
	}
	if (verbose) fmt::print(stderr, "[{}] finished generating {} events\n", cp.gid(),  b->state.num_events);

	// Event loop is done, set xsection correctly and normalise histos
	b->ah->setCrossSection(b->pythia.info.sigmaGen() * 1.0E9);
	b->ah->finalize();

	// Push histos into block
	b->data = b->ah->getData();

	// Debug write out --- uncomment to write each block's YODA file
	//b->ah->writeData(std::to_string((1+npc)*(b->state.seed+cp.gid()))+".yoda");


	// This is a bit annoying --- we need to unscale Histo1D and Histo2D beforge the reduction
	// TODO: Figure out whether this is really necessary
	/***
	for (auto ao : b->data) {
		if (ao && ao->hasAnnotation("ScaledBy"))
		{
			double sc = std::stod(ao->annotation("ScaledBy"));
			if (ao->type()=="Histo1D")
			{
				dynamic_cast<YODA::Histo1D&>(*ao).scaleW(1./sc);
				dynamic_cast<YODA::Histo1D&>(*ao).addAnnotation("OriginalScaledBy", 1./sc);
			}
			else if (ao->type()=="Histo2D")
			{
				dynamic_cast<YODA::Histo2D&>(*ao).scaleW(1./sc);
				dynamic_cast<YODA::Histo2D&>(*ao).addAnnotation("OriginalScaledBy", 1./sc);
			}
		}
	}
	***/

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> diff = end - start;
	fmt::print(stderr, "[{}] Finishes processing, Takes {} minutes\n", cp.gid(), diff.count()/60);
}


void write_yoda(Block* b, diy::Master::ProxyWithLink const& cp, int nConfigs, bool verbose)
{
	if (verbose) fmt::print(stderr, "[{}] sees write_yoda \n", cp.gid());
	if (cp.gid() > nConfigs - 1 ) return;
/***
	for (auto ao : b->buffer) {
		if (ao && ao->hasAnnotation("OriginalScaledBy"))
		{
			double sc = std::stod(ao->annotation("OriginalScaledBy"));
			if (ao->type()=="Histo1D")
			{
				dynamic_cast<YODA::Histo1D&>(*ao).scaleW(1./sc);
			}
			else if (ao->type()=="Histo2D")
			{
				dynamic_cast<YODA::Histo2D&>(*ao).scaleW(1./sc);
			}
		}
	}
***/
	if (verbose) fmt::print(stderr, "[{}] -- writing to file {}  \n", cp.gid(), b->state.f_out);
	YODA::WriterYODA::write(b->state.f_out, b->buffer);
}

void clear_buffer(Block* b, diy::Master::ProxyWithLink const& cp, bool verbose)
{
	if (verbose) fmt::print(stderr, "[{}] -- clear buffer  \n", cp.gid());
	b->buffer.clear();
}

// --- main program ---//
int main(int argc, char* argv[])
{
	auto start = std::chrono::system_clock::now();
	diy::mpi::environment env(argc, argv);
	diy::mpi::communicator world;

	int threads = 1;
    int runnum = -1;
	int nEvents=1000;
	size_t seed=1234;
	int evts_per_block_in = -1;
	vector<std::string> analyses;
	std::string out_file="diy.yoda";
	std::string pfile="runPythia.cmd";
	std::string dfile="detector_config.yoda";
	std::string indir="";
	std::string mfile = "mg5.cmd";
	int dim(2); // 2-d blocks, 3-d do not work for now! 

	// get command line arguments
	using namespace opts;
	Options ops(argc, argv);
	bool                      verbose     = ops >> Present('v',
			"verbose",
			"verbose output");
	ops >> Option('t', "thread",    threads,   "Number of threads");
	ops >> Option('n', "nevents",   nEvents,   "Number of events to generate in total");
	ops >> Option('a', "analysis",  analyses,  "Rivet analyses --- can be given multiple times");
	ops >> Option('o', "output",    out_file,  "Output filename.");
	ops >> Option('s', "seed",      seed,      "The Base seed --- this is incremented for each block.");
	ops >> Option('p', "pfile",     pfile,     "Parameter config file for testing __or__ input file name to look for in directory.");
	ops >> Option('d', "dfile",     dfile,     "detector config file for testing __or__ input file name to look for in directory.");
	ops >> Option('i', "indir",     indir,     "Input directory with hierarchical pfiles");
    ops >> Option('r', "runnum",    runnum,    "Use only runs ending runnum");
	ops >> Option('m', "mfile",     mfile,     "MadGraph5 config file for testing __or__ input file name to look for in directory.");
	ops >> Option(     "evtsPerBlock",     evts_per_block_in,     "Events per block");
	// ops >> Option(       "dim",     dim,     "dimension");
	if (ops >> Present('h', "help", "Show help"))
	{
		std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
		std::cout << ops;
		return 1;
	}

	int nConfigs = 0;

	if( world.rank()==0 ) {
		bool f_ok;
		// Program logic: check whether a single parameter file has been given or
		// a directory.
		std::vector<std::string> physConfig;
		physConfig.clear();
		f_ok = readConfig(pfile, physConfig, verbose);
		if (!f_ok) {
			// Use glob to look for files in directory
			std::string pattern = indir + "/*/" + pfile;
			if (runnum >=0) {
             	pattern =  indir + "/*" + std::to_string(runnum) + "/" + pfile;
			}
			for (auto f : glob(pattern)) {
				physConfig.clear();
				bool this_ok = readConfig(f, physConfig, verbose);
				if (this_ok) {
					nConfigs ++; 
				}
			}
		}
		else {
			nConfigs = 1;
		}
	}

	MPI_Bcast(&nConfigs,   1, MPI_INT, 0, world);

	// number of events per block is inferred from total number of events and events per block
	// Taking advantage of DIY being able to map any number of blacks to available CPU resources
	const int MINIMUM_NUMBER_EVENTS = 2000; // each block at least generates 2000 events
	int evts_per_block = (evts_per_block_in > 1)? evts_per_block_in: MINIMUM_NUMBER_EVENTS;
	size_t num_universes = ceil(nEvents/evts_per_block);
	num_universes = num_universes < 1?1:num_universes;



	// ----- starting here is a lot of standard boilerplate code for this kind of
	//       application.
	int mem_blocks  = -1;  // all blocks in memory, if value here then that is how many are in memory


	// diy initialization
	diy::FileStorage storage("./DIY.XXXXXX"); // used for blocks moved out of core

	// set global data bounds
	Bounds domain;
	for(auto i = 0; i < dim -1; i++){
		domain.min[i] = 0;
		// Used to identify number of configurations.
		domain.max[i] = nConfigs; 
	}
	// number of blocks used to produce the events of the same physics and configuration
	domain.min[dim-1] = 0; domain.max[dim-1] = num_universes;   

	////// choice of contiguous or round robin assigner
	size_t tot_blocks = nConfigs*num_universes;
	diy::ContiguousAssigner   assigner(world.size(), tot_blocks);

	if( world.rank()==0 ) {
		std::time_t tttt = std::time(nullptr);
		fmt::print(stderr, "\n*** This is diy running Pythia8 ***\n");
		fmt::print(stderr, "\n    Local Time:              {}\n", std::put_time(std::localtime(&tttt), "%c %Z"));
		fmt::print(stderr, "\n    Universes:               {}\n", num_universes);
		fmt::print(stderr, "\n    Physics configurations:  {}\n", nConfigs);
		fmt::print(stderr, "\n    Number of events: {}\n", nEvents);
		fmt::print(stderr, "\n    Total number of events:  {}\n", nEvents*nConfigs);
		fmt::print(stderr, "\n    World size:  {}\n", world.size());
		fmt::print(stderr, "\n    Events Per Bloack:  {}\n", evts_per_block);
		fmt::print(stderr, "\n    Total blocks:  {}\n", tot_blocks);
		for (auto s : analyses) {
			fmt::print(stderr, "\n	Analyses: {} \n", s);
		}

		// fmt::print(stderr, "\n    MadGraph configurations:  {}\n", mg5Configs.size());
		fmt::print(stderr, "***********************************\n");
	}


	// whether faces are shared in each dimension; uninitialized values default to false
	diy::RegularDecomposer<Bounds>::BoolVector          share_face(dim);
	// whether boundary conditions are periodic in each dimension; uninitialized values default to false
	diy::RegularDecomposer<Bounds>::BoolVector          wrap(dim);
	// number of ghost cells per side in each dimension; uninitialized values default to 0
	diy::RegularDecomposer<Bounds>::CoordinateVector    ghosts(dim);
	// number of blocks in each dimension; 0 means no constraint; uninitialized values default to 0
	diy::RegularDecomposer<Bounds>::DivisionsVector     divs(dim);
	divs[dim - 1] = num_universes;                      // one universe per block in last dimension

	//// decompose the domain into blocks
	diy::RegularDecomposer<Bounds> decomposer(
			dim, 
			domain, 
			tot_blocks,
			share_face, wrap, ghosts, divs
			);



	diy::Master master(world, threads, mem_blocks, &Block::create, &Block::destroy,
			&storage, &Block::save, &Block::load);
	AddBlock create(master);
	decomposer.decompose(world.rank(), assigner, create); // Note: only decompose once!


	// setup configurations for each scan
	master.foreach( [&](Block* b, const diy::Master::ProxyWithLink& cp)
			{ b->init_data(cp, nConfigs, evts_per_block, seed, indir, pfile, analyses, out_file, dfile, mfile, verbose); });


	// run MadGraph to generate gridpack
	master.foreach( [&](Block* b, const diy::Master::ProxyWithLink& cp)
			{ b->gen_mg5_gridpack(cp, nConfigs, verbose); });

	// make sure blocks are initialised!
	world.barrier();

	// Let's decompose the problem
	int k = 2;       // the radix of the k-ary reduction tree

	// First create partners for merge over regular block grid.
	// These will be used to get divisions vector and kvalues vector for default merge reduction.
	// We won't actually do the merge with these partners. Instead, we'll use the divisions vector as is
	// and will modify the kvalues vector to only merge in final dimension.
	bool contiguous = true;
	diy::RegularMergePartners  orig_partners(
			decomposer,         // domain decomposition
			k,                  // radix of k-ary reduction
			contiguous);        // contiguous = true: distance doubling
	diy::RegularMergePartners::KVSVector orig_kvs = orig_partners.kvs(); // original vector of [dim, k value] for each round
	diy::RegularMergePartners::KVSVector my_kvs;                         // subset of orig_kvs for only rounds in the final dimension
	divs = orig_partners.divisions();                                    // number of blocks in each dimension

	// modify orig_kvs to get my_kvs
	for (auto i = 0; i < orig_kvs.size(); i++)
		if (orig_kvs[i].dim == dim - 1)                        // keep last dim only
			my_kvs.push_back(orig_kvs[i]);

	// debug: print orig_kvs and my_kvs
	/***
	  fmt::print(stderr, "orig_kvs: ");
	  for (auto i = 0; i < orig_kvs.size(); i++)
	  fmt::print(stderr, "[ dim={}, k={} ]", orig_kvs[i].dim, orig_kvs[i].size);
	  fmt::print(stderr, "\n");
	  fmt::print(stderr, "my_kvs: ");
	  for (auto i = 0; i < my_kvs.size(); i++)
	  fmt::print(stderr, "[ dim={}, k={} ]", my_kvs[i].dim, my_kvs[i].size);
	  fmt::print(stderr, "\n\n");
	 ****/

	// Now, do the actual reduction in the last dimension using explicit divs and my_kvs vectors
	diy::RegularMergePartners  my_merge_partner(
			divs,                // explicit divisions vector
			my_kvs,              // explicit vector of rounds
			contiguous);         // contiguous = true: distance doubling

	diy::RegularBroadcastPartners my_bc_partner(
			divs,                // explicit divisions vector
			my_kvs,              // explicit vector of rounds
			contiguous);         // contiguous = true: distance doubling


	diy::RegularMergePartners  partners(decomposer,  // domain decomposition
			k,           // radix of k-ary reduction
			true); // contiguous = true: distance doubling



	// Broadcast the runconfig to the other blocks
	diy::reduce(master, assigner, my_bc_partner, &bc_pointconfig<Block>);


	if (verbose) master.foreach([verbose](Block* b, const diy::Master::ProxyWithLink& cp)
			{print_block(b, cp, verbose); });

	master.foreach([world, verbose](Block* b, const diy::Master::ProxyWithLink& cp)
			{process_block(b, cp, verbose); });

	// make sure all data are written before clear buffers
	world.barrier();

	diy::reduce(
			master,              // Master object
			assigner,            // Assigner object
			my_merge_partner,            // RegularMergePartners object
			&reduceData<Block>);

	//////This is where the write out happens --- the output file name is part of the blocks config
	master.foreach([nConfigs, verbose](Block* b, const diy::Master::ProxyWithLink& cp)
			{ write_yoda(b, cp, nConfigs, verbose); });

	// make sure all data are written before clear buffers
	world.barrier();

	// Wipe the buffer to prevent double counting etc.
	master.foreach([world, verbose](Block* b, const diy::Master::ProxyWithLink& cp)
			{ clear_buffer(b, cp, verbose); });

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> diff = end - start;
	if(world.rank() == 0) {
		std::time_t tttt = std::time(nullptr);
		fmt::print(stderr, "FINISHED on Local Time: {} \n",
				std::put_time(std::localtime(&tttt), "%c %Z") );
		fmt::print(stderr, "{} FINISHED, Takes {} minutes\n", argv[0], diff.count()/60);
	}
	return 0;
}
