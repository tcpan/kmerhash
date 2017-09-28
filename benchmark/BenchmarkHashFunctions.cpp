#include <vector>
#include <algorithm>  // std::generate
#include <cstdlib>    // std::rand
#include <sstream>
#include <exception>

#include "kmerhash/hash.hpp"
#include "utils/benchmark_utils.hpp"

#include "tclap/CmdLine.h"


// comparison of some hash functions.

#ifdef VTUNE_ANALYSIS
#define MEASURE_DISABLED 0
#define MEASURE_FARM 1

#define MEASURE_MURMURSSE 10
#define MEASURE_MURMURAVX 11
#define MEASURE_CRC32C 12

static int measure_mode = MEASURE_DISABLED;

#include <ittnotify.h>

#endif


template <size_t N>
struct DataStruct {
    unsigned char data[N];
};


template <typename H, size_t N>
void benchmark_hash(H const & hasher, DataStruct<N> const * data, unsigned int * hashes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    hashes[i] = hasher(data[i]);
  }
}

template <typename H, size_t N>
void benchmark_hash_batch(H const & hasher, DataStruct<N> const * data, unsigned int * hashes, size_t count) {
  hasher.hash(data, count, hashes);
}


template <size_t N>
void benchmarks(size_t count, unsigned char* in, unsigned int* out) {
  BL_BENCH_INIT(benchmark);

  DataStruct<N>* data = reinterpret_cast<DataStruct<N>*>(in);

  // ============ flat_hash_map  not compiling.
  // doesn't compile.  it's using initializer lists extensively, and the templated_iterator is having trouble constructing from initializer list.
  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::identity<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "iden", count);



  BL_BENCH_START(benchmark);
  {
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_FARM)
      __itt_resume();
#endif
    ::fsc::hash::farm<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_FARM)
      __itt_pause();
#endif
  }
  BL_BENCH_END(benchmark, "farm", count);


  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::murmur<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "murmur", count);

  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::farm32<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "farm32", count);


  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::murmur32<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "murmur32", count);

#if defined(__SSE4_1__)
  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::murmur3sse32<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "murmur32sse1", count);

  BL_BENCH_START(benchmark);
  {
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_MURMURSSE)
      __itt_resume();
#endif
    ::fsc::hash::murmur3sse32<DataStruct<N> > h;
     benchmark_hash_batch(h, data, out, count);
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_MURMURSSE)
      __itt_pause();
#endif
  }
  BL_BENCH_END(benchmark, "murmur32sse4", count);
#endif

#if defined(__AVX2__)
  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::murmur3avx32<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "murmur32avx1", count);

  BL_BENCH_START(benchmark);
  {

#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_MURMURAVX)
      __itt_resume();
#endif
    ::fsc::hash::murmur3avx32<DataStruct<N> > h;
     benchmark_hash_batch(h, data, out, count);
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_MURMURAVX)
      __itt_pause();
#endif
  }
  BL_BENCH_END(benchmark, "murmur32avx8", count);
#endif

#if defined(__SSE4_2__)
  BL_BENCH_START(benchmark);
  {
    ::fsc::hash::crc32c<DataStruct<N> > h;
     benchmark_hash(h, data, out, count);
  }
  BL_BENCH_END(benchmark, "CRC32C1", count);

  BL_BENCH_START(benchmark);
  {
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_CRC32C)
      __itt_resume();
#endif
    ::fsc::hash::crc32c<DataStruct<N> > h;
     benchmark_hash_batch(h, data, out, count);
#ifdef VTUNE_ANALYSIS
  if (measure_mode == MEASURE_CRC32C)
      __itt_pause();
#endif
  }
  BL_BENCH_END(benchmark, "CRC32Cbatch", count);
#endif


  std::stringstream ss;
  ss << "hash " << count << " " << size_t(N) << "-byte elements";

  BL_BENCH_REPORT_NAMED(benchmark, ss.str().c_str());

}


int main(int argc, char** argv) {

#ifdef VTUNE_ANALYSIS
      __itt_pause();
#endif

      size_t count = 100000000;
      size_t el_size = 0;

      try {

        // Define the command line object, and insert a message
        // that describes the program. The "Command description message"
        // is printed last in the help text. The second argument is the
        // delimiter (usually space) and the last one is the version number.
        // The CmdLine object parses the argv array based on the Arg objects
        // that it contains.
        TCLAP::CmdLine cmd("Benchmark hash function", ' ', "0.1");

        // Define a value argument and add it to the command line.
        // A value arg defines a flag and a type of value that it expects,
        // such as "-n Bishop".

        TCLAP::ValueArg<size_t> countArg("c","count","number of elements to hash", false, count, "size_t", cmd);
        TCLAP::ValueArg<size_t> elSizeArg("e","el_size","size of elements in bytes. 0 to run all", false, el_size, "size_t", cmd);

    #ifdef VTUNE_ANALYSIS
        std::vector<std::string> measure_modes;
        measure_modes.push_back("farm");
        measure_modes.push_back("murmur_sse");
        measure_modes.push_back("murmur_avx");
        measure_modes.push_back("crc32c");
        measure_modes.push_back("disabled");
        TCLAP::ValuesConstraint<std::string> measureModeVals( measure_modes );
        TCLAP::ValueArg<std::string> measureModeArg("","measured_op","hash function to measure (default disabled)",false,"disabled",&measureModeVals, cmd);
    #endif

        // Parse the argv array.
        cmd.parse( argc, argv );

        count = countArg.getValue();
        el_size = elSizeArg.getValue();
        std::cout << "Executing for " << el_size << " element size. " << std::endl;

    #ifdef VTUNE_ANALYSIS
        // set the default for query to filename, and reparse
        std::string measure_mode_str = measureModeArg.getValue();
        std::cout << "Measuring " << measure_mode_str << std::endl;

        if (measure_mode_str == "farm") {
          measure_mode = MEASURE_FARM;
        } else if (measure_mode_str == "murmur_sse") {
          measure_mode = MEASURE_MURMURSSE;
        } else if (measure_mode_str == "murmur_avx") {
          measure_mode = MEASURE_MURMURAVX;
        } else if (measure_mode_str == "crc32c") {
          measure_mode = MEASURE_CRC32C;
        } else {
          measure_mode = MEASURE_DISABLED;
        }
    #endif


        // Do what you intend.

      } catch (TCLAP::ArgException &e)  // catch any exceptions
      {
        std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
        exit(-1);
      }






  unsigned char* data = (unsigned char*) malloc(count * 256);

  if (data == nullptr) {
    throw std::invalid_argument("count is too large, or 0, and cannot allocation memory");
  }

  unsigned int* hashes = (unsigned int*) malloc(count * sizeof(unsigned int));

  if (hashes == nullptr) {
    throw std::invalid_argument("count is too large, or 0, and cannot allocation memory");
  }


  if ((el_size == 0) || (el_size ==   1)) benchmarks<  1>(count, data, hashes);
  if ((el_size == 0) || (el_size ==   2)) benchmarks<  2>(count, data, hashes);
  if ((el_size == 0) || (el_size ==   4)) benchmarks<  4>(count, data, hashes);
  if ((el_size == 0) || (el_size ==   8)) benchmarks<  8>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  16)) benchmarks< 16>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  32)) benchmarks< 32>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  64)) benchmarks< 64>(count, data, hashes);
  if ((el_size == 0) || (el_size == 128)) benchmarks<128>(count, data, hashes);
  if ((el_size == 0) || (el_size == 256)) benchmarks<256>(count, data, hashes);

  if ((el_size == 0) || (el_size ==   3)) benchmarks<  3>(count, data, hashes);
  if ((el_size == 0) || (el_size ==   5)) benchmarks<  5>(count, data, hashes);
  if ((el_size == 0) || (el_size ==   7)) benchmarks<  7>(count, data, hashes);
  if ((el_size == 0) || (el_size ==   9)) benchmarks<  9>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  15)) benchmarks< 15>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  17)) benchmarks< 17>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  31)) benchmarks< 31>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  33)) benchmarks< 33>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  63)) benchmarks< 63>(count, data, hashes);
  if ((el_size == 0) || (el_size ==  65)) benchmarks< 65>(count, data, hashes);
  if ((el_size == 0) || (el_size == 127)) benchmarks<127>(count, data, hashes);
  if ((el_size == 0) || (el_size == 129)) benchmarks<129>(count, data, hashes);
  if ((el_size == 0) || (el_size == 255)) benchmarks<255>(count, data, hashes);

  free(data);
  free(hashes);
#ifdef VTUNE_ANALYSIS
      __itt_resume();
#endif

}

