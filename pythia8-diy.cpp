
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <random>
#include <typeinfo>

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
#include "Rivet/AnalysisHandler.hh"
#undef foreach // This line prevents a clash of definitions of rivet's legacy foreach with that of DIY

#include "HepMC/IO_GenEvent.h"




using namespace std;
using namespace Pythia8;
#include "opts.h"

using namespace std;


typedef diy::DiscreteBounds Bounds;
typedef diy::RegularGridLink RCLink;

typedef GenericBlock<Bounds, PointConfig, AnalysisObjects> Block;
typedef ConfigBlockAdder2<Bounds, RCLink, Block, PointConfig> AddBlock2;


void print_block(Block* b,                             // local block
                 const diy::Master::ProxyWithLink& cp, // communication proxy
                 bool verbose)                         // user-defined additional arguments
{
   for (auto s : b->state.conf) {
      fmt::print(stderr, "[{}]: {} ", cp.gid(), s);
   }
   fmt::print(stderr, " {} ", b->state.seed);
   fmt::print(stderr, "\n");
   for (auto s : b->state.analyses) {
      fmt::print(stderr, "[{}]: {} ", cp.gid(), s);
   }
   fmt::print(stderr, "\n");
}

// A pilot event generation to filter out bad parameter points
void pilot_block(Block* b, diy::Master::ProxyWithLink const& cp, int rank, std::vector<std::string> physConfig)
{
  // This make rivet only report ERRORs
  // TODO: can we have a global flag to steer verbosity of all moving parts?

  // Minimise pythia's output
  b->pythia.readString("Print:quiet = on");

  // Configure pythia with a vector of strings
  for (auto s  : physConfig) b->pythia.readString(s);

  // Py8 random seed for this block read from point config
  b->pythia.readString("Random:setSeed = on");
  b->pythia.readString("Random:seed = " + std::to_string(b->state.seed));

  // All configurations done, initialise Pythia
  b->pythia.init();

  int success = 0;
  int nTrials = 0;
  int nAbort = 100;
  int iAbort = 0;
  // The event loop
  for (int iEvent = 0; iEvent < 100; ++iEvent) {
     nTrials++;
    if (!b->pythia.next()) {
     fmt::print(stderr, "[{}] iAbort: {}\n", cp.gid(), iAbort);
      if (++iAbort < nAbort) continue;
      break;
    }
    success++;
  }

  fmt::print(stderr, "[{}] trials: {} success: {}\n", cp.gid(), nTrials, success);
  // Push histos into block
  //b->data = b->ah.getData();

}


//void process_block(Block* b, diy::Master::ProxyWithLink const& cp, int rank, std::vector<std::string> physConfig, std::vector<std::string> analyses, bool verbose)
void process_block(Block* b, diy::Master::ProxyWithLink const& cp, int rank,  bool verbose)
{
  // This make rivet only report ERRORs
  // TODO: can we have a global flag to steer verbosity of all moving parts?
  if (!verbose) Rivet::Log::setLevel("Rivet", Rivet::Log::ERROR);

  // Minimise pythia's output
  if (verbose) b->pythia.readString("Print:quiet = off");
  else b->pythia.readString("Print:quiet = on");

  // Configure pythia with a vector of strings
  for (auto s  : b->state.conf) b->pythia.readString(s);

  // Py8 random seed for this block read from point config
  b->pythia.readString("Random:setSeed = on");
  b->pythia.readString("Random:seed = " + std::to_string(b->state.seed+cp.gid()));

  // All configurations done, initialise Pythia
  b->pythia.init();

  // Add all anlyses to rivet
  // TODO: we may want to feed the "ignore beams" switch as well
  for (auto a : b->state.analyses) b->ah.addAnalysis(a);

  // The event loop
  int nAbort = 5;
  int iAbort = 0;
  if (verbose) fmt::print(stderr, "[{}] generating {} events\n", cp.gid(),  b->state.num_events);
  for (int iEvent = 0; iEvent < b->state.num_events; ++iEvent) {
    if (!b->pythia.next()) {
      if (++iAbort < nAbort) continue;
      break;
    }
    HepMC::GenEvent* hepmcevt = new HepMC::GenEvent();
    b->ToHepMC.fill_next_event( b->pythia, hepmcevt );

    try {b->ah.analyze( *hepmcevt ) ;} catch (const std::exception& e)
    {
      if (verbose) fmt::print(stderr, "[{}] exception in analyze: {}\n", cp.gid(), e.what());
    }
    delete hepmcevt;
    if (verbose && iEvent%100 == 0) fmt::print(stderr, "[{}]  {}/{} \n", cp.gid(),  iEvent, b->state.num_events);;
  }

  // Event loop is done, set xsection correctly and normalise histos
  b->ah.setCrossSection(b->pythia.info.sigmaGen() * 1.0E9);
  b->ah.finalize();

  // Push histos into block
  b->data = b->ah.getData();

  // Write out so we can sanity check with yodamerge

  // This is a bit annoying --- we need to unscale Histo1D and Histo2D beforge the reduction
  // TODO: Figure out whether this is really necessary
  for (auto ao : b->data) {
    if (ao->hasAnnotation("ScaledBy"))
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
}


void write_yoda(Block* b, diy::Master::ProxyWithLink const& cp, int rank, bool verbose)
{
  if (rank==0 && cp.gid()==0) {
    for (auto ao : b->buffer) {
      if (ao->hasAnnotation("OriginalScaledBy"))
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
    if (verbose) fmt::print(stderr, "[{}] -- writing to file {}  \n", cp.gid(), b->state.f_out);
    YODA::WriterYODA::write(b->state.f_out, b->buffer);
  }
}


void set_pc(Block* b,                             // local block
                 const diy::Master::ProxyWithLink& cp, // communication proxy
                 PointConfig pc)                         // user-defined additional arguments
{
  if (cp.gid() == 0) b->state = pc;
}

// --- main program ---//
int main(int argc, char* argv[])
{
    diy::mpi::environment env(argc, argv);
    diy::mpi::communicator world;

    size_t nBlocks = 0;
    int threads = 1;
    int nEvents=1000;
    size_t seed=1234;
    vector<std::string> analyses;
    std::string out_file="diy.yoda";
    std::string pfile="runPythia.cmd";
    std::string indir="";
    // get command line arguments
    using namespace opts;
    Options ops(argc, argv);
    bool                      verbose     = ops >> Present('v',
                                                           "verbose",
                                                           "verbose output");
    ops >> Option('t', "thread",    threads,   "Number of threads");
    ops >> Option('n', "nevents",   nEvents,   "Number of events to generate in total");
    ops >> Option('b', "nblocks",   nBlocks,   "Number of blocks");
    ops >> Option('a', "analysis",  analyses,  "Rivet analyses --- can be given multiple times");
    ops >> Option('o', "output",    out_file,  "Output filename.");
    ops >> Option('s', "seed",      seed,      "The Base seed --- this is incremented for each block.");
    ops >> Option('p', "pfile",     pfile,     "Parameter config file for testing __or__ input file name to look for in directory.");
    ops >> Option('i', "indir",     indir,     "Input directory with hierarchical pfiles");
    if (ops >> Present('h', "help", "Show help"))
    {
        std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
        std::cout << ops;
        return 1;
    }
    
    std::vector<std::string> physConfig;
    std::vector<std::vector<std::string> > physConfigs;
    std::vector<std::string> out_files;
    bool f_ok;
    //if( world.rank()==0 ) { # TODO figure out how to make this work
       // Programm logic: check whether a single parameter file has been given or
       // a directory.
       f_ok = readConfig(pfile, physConfig, verbose);
       if (!f_ok) {
          // Use glob to look for files in directory
          for (auto f : glob(indir + "/*/" + pfile)) {
             physConfig.clear();
             bool this_ok = readConfig(f, physConfig, verbose);
             if (this_ok) {
                physConfigs.push_back(physConfig);
                out_files.push_back(f+".yoda");
             }
          }
       } 
       else {
          physConfigs.push_back(physConfig);
          out_files.push_back(out_file);
          std::cerr << out_files.back() << " ##### \n";
       }
    //}



    int mem_blocks  = -1;  // all blocks in memory, if value here then that is how many are in memory
    int dim(1);
    
    size_t blocks;
    if (nBlocks==0) blocks= world.size() * threads;
    else blocks=nBlocks;

    // ----- starting here is a lot of standard boilerplate code for this kind of
    //       application.

    // diy initialization
    diy::FileStorage storage("./DIY.XXXXXX"); // used for blocks moved out of core
    diy::Master master(world, threads, mem_blocks, &Block::create, &Block::destroy,
                     &storage, &Block::save, &Block::load);
    Bounds domain;
    for (int i = 0; i < dim; ++i) {
      domain.min[i] = 0;
      domain.max[i] = blocks-1;
    }

    ////// choice of contiguous or round robin assigner
    diy::ContiguousAssigner   assigner(world.size(), blocks);
    //// decompose the domain into blocks
    //// This is a DIY regular way to assign neighbors. You can do this manually.
    diy::RegularDecomposer<Bounds> decomposer(dim, domain, blocks);

    int k = 2;       // the radix of the k-ary reduction tree
    diy::RegularBroadcastPartners comm(decomposer, k, true);

    diy::RegularMergePartners  partners(decomposer,  // domain decomposition
                                        k,           // radix of k-ary reduction
                                        true); // contiguous = true: distance doubling


    PointConfig pc;
    for (size_t ipc=0;ipc<physConfigs.size();++ipc) {
       if (world.rank()==0) {
          pc = mkRunConfig(blocks, nEvents, seed, physConfigs[ipc], analyses, out_files[ipc]);
       }

       AddBlock2 create(master, pc);
       // This probably only works if the number of blocks is the same for each pc
       if (ipc==0) decomposer.decompose(world.rank(), assigner, create); // Note: only decompose once!

       // We need to tell the first block about the new configuration --- TODO: more elegant way to access the first block?
       master.foreach([world, pc](Block* b, const diy::Master::ProxyWithLink& cp)
                        {set_pc(b, cp, pc); });

       // Broadcast the runconfig to the other blocks
       diy::reduce(master, assigner, comm, &bc_pointconfig<Block>);

       if (verbose) master.foreach([world](Block* b, const diy::Master::ProxyWithLink& cp)
                        {print_block(b, cp, world.rank()); });

       master.foreach([world, physConfig, analyses, verbose](Block* b, const diy::Master::ProxyWithLink& cp)
                        {process_block(b, cp, world.rank(), verbose); });



       diy::reduce(master,              // Master object
                   assigner,            // Assigner object
                   partners,            // RegularMergePartners object
                   &reduceData<Block>);

       //////This is where the write out happens --- the output file name is part of the blocks config
       master.foreach([world, verbose](Block* b, const diy::Master::ProxyWithLink& cp)
                      { write_yoda(b, cp, world.rank(), verbose); });

       //// Trial run
       ////master.foreach([world, physConfig](Block* b, const diy::Master::ProxyWithLink& cp)
       ///{pilot_block(b, cp, world.rank(), physConfig); });
          //master.foreach([world,out_files, ipc](Block* b, const diy::Master::ProxyWithLink& cp)
                         //{ write_yoda(b, cp, out_files[ipc], world.rank()); });
   }


    return 0;
}
