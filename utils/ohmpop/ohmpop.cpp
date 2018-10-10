//
// author Kazys Stepanas
//
#include "OhmPopConfig.h"

#include <glm/glm.hpp>

#include <slamio/SlamCloudLoader.h>

#include <ohm/ClearanceProcess.h>
#include <ohm/Mapper.h>
#include <ohm/OhmGpu.h>
#include <ohm/GpuMap.h>
#include <ohm/OccupancyMap.h>
#include <ohm/MapSerialise.h>
#include <ohm/Voxel.h>
#include <ohm/OccupancyType.h>
#include <ohm/OccupancyUtil.h>
#include <ohmutil/OhmUtil.h>
#include <ohmutil/PlyMesh.h>
#include <ohmutil/ProgressMonitor.h>
#include <ohmutil/SafeIO.h>
#include <ohmutil/ScopedTimeDisplay.h>

#include <ohmutil/Options.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <thread>

#define COLLECT_STATS 0
#define COLLECT_STATS_IGNORE_FIRST 1

namespace
{
  int quit = 0;

  void onSignal(int arg)
  {
    if (arg == SIGINT || arg == SIGTERM)
    {
      ++quit;
    }
  }


  // Min/max P: 0.1192 0.971
  // Min/max V: -2.00003 3.51103
  struct Options
  {
    std::string cloud_file;
    std::string trajectory_file;
    std::string output_base_name;
    glm::u8vec3 region_voxel_dim;
    uint64_t point_limit = 0;
    int64_t preload_count = 0;
    double start_time = 0;
    double time_limit = 0;
    double resolution = 0.25;
    double progressive_mapping_slice = 0.0;
    double mapping_interval = 0.2;
    float prob_hit = 0.9f;
    float prob_miss = 0.49f;
    float prob_thresh = 0.5f;
    float clearance = 0.0f;
    glm::vec2 prob_range = glm::vec2(0, 0);
    unsigned batch_size = 2048;
    bool post_population_mapping = true;
    bool serialise = true;
    bool save_info = false;
    bool quiet = false;

    void print(std::ostream **out, const ohm::OccupancyMap &map) const;
  };


  void Options::print(std::ostream **out, const ohm::OccupancyMap &map) const
  {
    while (*out)
    {
      **out << "Cloud: " << cloud_file;
      if (!trajectory_file.empty())
      {
        **out << " + " << trajectory_file << '\n';
      }
      else
      {
        **out << " (no trajectory)\n";
      }
      if (preload_count)
      {
        **out << "Preload: ";
        if (preload_count < 0)
        {
          **out << "all";
        }
        else
        {
          **out << preload_count;
        }
        **out << '\n';
      }

      if (point_limit)
      {
        **out << "Maximum point: " << point_limit << '\n';
      }

      if (start_time)
      {
        **out << "Process from timestamp: " << start_time << '\n';
      }

      if (time_limit)
      {
        **out << "Process to timestamp: " << time_limit << '\n';
      }

      std::string mem_size_string;
      util::makeMemoryDisplayString(mem_size_string, ohm::OccupancyMap::nodeMemoryPerRegion(region_voxel_dim));
      **out << "Map resolution: " << resolution << '\n';
      glm::i16vec3 region_dim = region_voxel_dim;
      region_dim.x = (region_dim.x) ? region_dim.x : OHM_DEFAULT_CHUNK_DIM_X;
      region_dim.y = (region_dim.y) ? region_dim.y : OHM_DEFAULT_CHUNK_DIM_Y;
      region_dim.z = (region_dim.z) ? region_dim.z : OHM_DEFAULT_CHUNK_DIM_Z;
      **out << "Map region dimensions: " << region_dim << '\n';
      **out << "Map region memory: " << mem_size_string << '\n';
      **out << "Hit probability: " << prob_hit << '\n';
      **out << "Miss probability: " << prob_miss << '\n';
      **out << "Probability range: [" << map.minNodeProbability() << ' ' << map.maxNodeProbability() << "]\n";
      **out << "Ray batch size: " << batch_size << '\n';
      **out << "Clearance mapping: ";
      if (clearance > 0)
      {
        **out << clearance << "m range\n";
      }
      else
      {
        **out << "disabled\n";
      }

      **out << "Mapping mode: ";
      if (progressive_mapping_slice)
      {
        **out << "progressive time slice " << progressive_mapping_slice << "s\n";
        **out << "Mapping interval: " << mapping_interval << "s\n";
        **out << "Post population mapping: " << (post_population_mapping ? "on" : "off") << '\n';
      }
      else
      {
        **out << "post" << '\n';
      }

      **out << std::flush;

      ++out;
    }
  }

  class SaveMapProgress : public ohm::SerialiseProgress
  {
  public:
    SaveMapProgress(ProgressMonitor &monitor)
      : monitor_(monitor)
    {}

    bool quit() const override { return ::quit > 1; }

    void setTargetProgress(unsigned target) override { monitor_.beginProgress(ProgressMonitor::Info(target)); }
    void incrementProgress(unsigned inc = 1) override { monitor_.incrementProgressBy(inc); }

  private:
    ProgressMonitor &monitor_;
  };


  class MapperThread
  {
  public:
    MapperThread(ohm::OccupancyMap &map, const Options &opt);
    ~MapperThread();

    void start();
    void join(bool wait_for_completion = true);

  private:
    void run();

    ohm::Mapper mapper_;
    std::thread *thread_ = nullptr;
    double time_slice_sec_ = 0.001;
    double interval_sec_ = 0.0;
    std::atomic_bool allow_completion_;
    std::atomic_bool quit_request_;
  };

  MapperThread::MapperThread(ohm::OccupancyMap &map, const Options &opt)
    : time_slice_sec_(opt.progressive_mapping_slice)
    , interval_sec_(opt.mapping_interval)
    , allow_completion_(true)
    , quit_request_(false)
  {
    mapper_.setMap(&map);
    if (opt.clearance > 0)
    {
      mapper_.addProcess(new ohm::ClearanceProcess(opt.clearance, ohm::kQfGpuEvaluate));
    }
  }


  MapperThread::~MapperThread()
  {
    join(false);
    delete thread_;
  }


  void MapperThread::start()
  {
    if (!thread_)
    {
      thread_ = new std::thread(std::bind(&MapperThread::run, this));
    }
  }


  void MapperThread::join(bool wait_for_completion)
  {
    if (thread_)
    {
      allow_completion_ = wait_for_completion;
      quit_request_ = true;
      thread_->join();
      delete thread_;
      thread_ = nullptr;
    }
  }


  void MapperThread::run()
  {
    using Clock = std::chrono::high_resolution_clock;
    while (!quit_request_)
    {
      const auto loop_start = Clock::now();
      if (time_slice_sec_ > 0)
      {
        mapper_.update(time_slice_sec_);
      }
      if (interval_sec_ > 0)
      {
        std::this_thread::sleep_until(loop_start + std::chrono::duration<double>(interval_sec_));
      }
    }

    if (allow_completion_)
    {
      mapper_.update(0);
    }
  }
}


int populateMap(const Options &opt)
{
  ohmutil::ScopedTimeDisplay time_display("Execution time");
  if (opt.quiet)
  {
    time_display.disable();
  }

  std::cout << "Loading points from " << opt.cloud_file << " with trajectory " << opt.trajectory_file << std::endl;

  SlamCloudLoader loader;
  if (!loader.open(opt.cloud_file.c_str(), opt.trajectory_file.c_str()))
  {
    fprintf(stderr, "Error loading cloud %s with trajectory %s \n", opt.cloud_file.c_str(), opt.trajectory_file.c_str());
    return -2;
  }

  using Clock = std::chrono::high_resolution_clock;
  ohm::OccupancyMap map(opt.resolution, opt.region_voxel_dim);
  ohm::GpuMap gpu_map(&map, true, opt.batch_size);
  //MapperThread mapper(map, opt);
  ohm::Mapper mapper(&map);
  std::vector<double> sample_timestamps;
  std::vector<glm::dvec3> origin_sample_pairs;
  glm::dvec3 origin, sample;
  // glm::vec3 voxel, ext(opt.resolution);
  double timestamp;
  uint64_t point_count = 0;
  // Update map visualisation every N samples.
  const size_t ray_batch_size = opt.batch_size;
  double timebase = -1;
  double first_timestamp = -1;
  double last_timestamp = -1;
  double first_batch_timestamp = -1;
  double next_mapper_update = opt.mapping_interval;
  std::atomic<uint64_t> elapsed_ms(0);
  Clock::time_point start_time, end_time;
  ProgressMonitor prog(10);

  if (!gpu_map.gpuOk())
  {
    std::cerr << "Failed to initialise GpuMap programs." << std::endl;
    return -3;
  }

  map.setHitProbability(opt.prob_hit);
  map.setOccupancyThresholdProbability(opt.prob_thresh);
  map.setMissProbability(opt.prob_miss);
  if (opt.prob_range[0])
  {
    map.setMinNodeProbability(opt.prob_range[0]);
  }
  if (opt.prob_range[1])
  {
    map.setMaxNodeProbability(opt.prob_range[1]);
  }
  // map.setSaturateAtMinValue(opt.saturateMin);
  // map.setSaturateAtMaxValue(opt.saturateMax);

  // Prevent ready saturation to free.
  // map.setClampingThresMin(0.01);
  // printf("min: %g\n", map.getClampingThresMinLog());

  if (opt.clearance > 0)
  {
    mapper.addProcess(new ohm::ClearanceProcess(opt.clearance, ohm::kQfGpuEvaluate));
  }

  std::ostream *streams[] = { &std::cout, nullptr, nullptr };
  std::ofstream info_stream;
  if (opt.save_info)
  {
    streams[1] = &info_stream;
    std::string output_file = opt.output_base_name + ".txt";
    std::ofstream out(output_file.c_str());
    info_stream.open(output_file.c_str());
  }

  opt.print(streams, map);

  if (opt.preload_count)
  {
    int64_t preload_count = opt.preload_count;
    if (preload_count < 0 && opt.point_limit)
    {
      preload_count = opt.point_limit;
    }

    std::cout << "Preloading points";

    start_time = Clock::now();
    if (preload_count < 0)
    {
      std::cout << std::endl;
      loader.preload();
    }
    else
    {
      std::cout << " " << preload_count << std::endl;
      loader.preload(preload_count);
    }
    end_time = Clock::now();
    const double preload_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() * 1e-3;
    std::cout << "Preload completed over " << preload_time << " seconds." << std::endl;
  }

  start_time = Clock::now();
  std::cout << "Populating map" << std::endl;

  prog.setDisplayFunction([&elapsed_ms, &opt](const ProgressMonitor::Progress &prog) {
    if (!opt.quiet)
    {
      const uint64_t elapsed_ms_local = elapsed_ms;
      const uint64_t sec = elapsed_ms_local / 1000u;
      const unsigned ms = unsigned(elapsed_ms_local - sec * 1000);

      std::ostringstream out;
      out.imbue(std::locale(""));
      out << '\r';

      if (prog.info.info && prog.info.info[0])
      {
        out << prog.info.info << " : ";
      }

      out << sec << '.' << std::setfill('0') << std::setw(3) << ms << "s : ";

      out << std::setfill(' ') << std::setw(12) << prog.progress;
      if (prog.info.total)
      {
        out << " / " << std::setfill(' ') << std::setw(12) << prog.info.total;
      }
      out << "    ";
      std::cout << out.str() << std::flush;
    }
  });
  prog.beginProgress(ProgressMonitor::Info(
    (point_count && timebase == 0) ? std::min<uint64_t>(point_count, loader.numberOfPoints()) : loader.numberOfPoints()));
  prog.startThread();

#if COLLECT_STATS
  struct TimeStats
  {
    uint64_t totalNs = 0;
    uint64_t maxNs = 0;
    uint64_t updateCount = 0;

    void add(const Clock::duration &elapsed)
    {
      const uint64_t timeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
      maxNs = std::max(timeNs, maxNs);
      totalNs += timeNs;
      ++updateCount;
    }

    void print() const
    {
      std::ostringstream str;
      str << "\n*************************************" << std::endl;
      const std::chrono::nanoseconds avgTNs(totalNs / updateCount);
      str << "Average integration time: " << avgTNs << std::endl;
      const std::chrono::nanoseconds maxTNs(maxNs);
      str << "Max integration time: " << maxTNs << std::endl;
      str << "*************************************" << std::endl;
      std::cout << str.str() << std::flush;
    }
  };

  TimeStats stats;
#endif // COLLECT_STATS

  //------------------------------------
  // Population loop.
  //------------------------------------
  //mapper.start();
  origin = glm::vec3(0, 0, 0);
  while ((point_count < opt.point_limit || opt.point_limit == 0) &&
    (last_timestamp - timebase < opt.time_limit || opt.time_limit == 0) &&
    loader.nextPoint(sample, &origin, &timestamp))
  {
    if (timebase < 0)
    {
      timebase = timestamp;
    }

    if (timestamp - timebase < opt.start_time)
    {
      continue;
    }

    if (last_timestamp < 0)
    {
      last_timestamp = timestamp;
    }

    if (first_timestamp < 0)
    {
      first_timestamp = timestamp;
    }

    ++point_count;
    sample_timestamps.push_back(timestamp);
    origin_sample_pairs.push_back(origin);
    origin_sample_pairs.push_back(sample);

    if (first_batch_timestamp < 0)
    {
      first_batch_timestamp = timestamp;
    }

    if (point_count % ray_batch_size == 0 || quit)
    {
#if COLLECT_STATS
      const auto then = Clock::now();
#endif // COLLECT_STATS
      gpu_map.integrateRays(origin_sample_pairs.data(), unsigned(origin_sample_pairs.size()));
#if COLLECT_STATS
      const auto integrateTime = Clock::now() - then;
#if COLLECT_STATS_IGNORE_FIRST
      if (firstBatchTimestamp != timestamp)
#endif // COLLECT_STATS_IGNORE_FIRST
      {
        stats.add(integrateTime);
      }
      const unsigned kLongUpdateThresholdMs = 100u;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(integrateTime).count() > kLongUpdateThresholdMs)
      {
        std::ostringstream str;
        str << '\n' << sampleTimestamps.front() - firstTimestamp << " (" << sampleTimestamps.front() << "): long update " << integrateTime << std::endl;
        std::cout << str.str() << std::flush;
  }
#endif // COLLECT_STATS
      sample_timestamps.clear();
      origin_sample_pairs.clear();

      const double elapsed_time = timestamp - last_timestamp;
      first_batch_timestamp = -1;

      prog.incrementProgressBy(ray_batch_size);
      last_timestamp = timestamp;
      // Store into elapsedMs atomic.
      elapsed_ms = uint64_t((last_timestamp - timebase) * 1e3);

      if (opt.progressive_mapping_slice > 0)
      {
        if (opt.mapping_interval >= 0)
        {
          next_mapper_update -= elapsed_time;
        }
        if (next_mapper_update <= 0)
        {
          next_mapper_update += opt.mapping_interval;
          //const auto mapper_start = Clock::now();
          mapper.update(opt.progressive_mapping_slice);
          //const auto mapper_end = Clock::now();
          //std::ostringstream msg;
          //msg << "\nMapper: " << (mapper_end - mapper_start) << '\n';
          //std::cout << msg.str();
        }
      }

      if (opt.point_limit && point_count >= opt.point_limit ||
          opt.time_limit && last_timestamp - timebase >= opt.time_limit || quit)
      {
        break;
      }
    }
  }

  // Make sure we have no more rays.
  if (!origin_sample_pairs.empty())
  {
#if COLLECT_STATS
    const auto then = Clock::now();
#endif // COLLECT_STATS
    gpu_map.integrateRays(origin_sample_pairs.data(), unsigned(origin_sample_pairs.size()));
#if COLLECT_STATS
    const auto integrateTime = Clock::now() - then;
    stats.add(integrateTime);
#endif // COLLECT_STATS
    sample_timestamps.clear();
    origin_sample_pairs.clear();
  }

  prog.endProgress();
  prog.pause();

  const auto mapper_start = Clock::now();
  if (opt.post_population_mapping && !quit)
  {
    std::cout << "\nFinalising" << std::endl;
    mapper.update(0.0);
  }
  //mapper.join(!quit && opt.postPopulationMapping);
  end_time = Clock::now();

#if COLLECT_STATS
  stats.print();
#endif // COLLECT_STATS

  if (!opt.quiet)
  {
    std::cout << std::endl;
  }

  // Sync the map.
  if (!opt.quiet)
  {
    std::cout << "syncing map" << std::endl;
  }
  gpu_map.syncOccupancy();

  const double processing_time_sec =
    std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() * 1e-3;

  std::ostream **out = streams;
  while (*out)
  {
    const double time_range = last_timestamp - first_timestamp;
    **out << "Point count: " << point_count << '\n';
    **out << "Data time: " << time_range << '\n';
    **out << "Population completed in " << mapper_start - start_time << std::endl;
    **out << "Post mapper completed in " << end_time - mapper_start << std::endl;
    **out << "Total processing time: " << end_time - start_time << '\n';
    **out << "Efficiency: " << ((time_range) ? processing_time_sec / time_range : 0.0) << '\n';
    **out << "Points/sec: " << ((processing_time_sec > 0) ? point_count / processing_time_sec : 0.0) << '\n';
    **out << "Memory (approx): " << map.calculateApproximateMemory() / (1024.0 * 1024.0) << " MiB\n";
    **out << std::flush;
    ++out;
  }

  if (opt.serialise)
  {
    if (quit < 2)
    {
      std::string output_file = opt.output_base_name + ".ohm";
      std::cout << "Saving map to " << output_file.c_str() << std::endl;
      SaveMapProgress save_progress(prog);
      prog.unpause();
      int err = ohm::save(output_file.c_str(), map, &save_progress);
      prog.endProgress();
      if (!opt.quiet)
      {
        std::cout << std::endl;
      }

      if (err)
      {
        fprintf(stderr, "Failed to save map: %d\n", err);
      }
    }

    // Save a cloud representation.
    std::cout << "Converting to point cloud." << std::endl;
    PlyMesh ply;
    glm::vec3 v;
    const auto map_end_iter = map.end();
    const size_t region_count = map.regionCount();
    glm::i16vec3 last_region = map.begin().key().regionKey();
    point_count = 0;

    prog.beginProgress(ProgressMonitor::Info(region_count));

    if (opt.serialise)
    {
      for (auto iter = map.begin(); iter != map_end_iter && quit < 2; ++iter)
      {
        const ohm::OccupancyNodeConst node = *iter;
        if (last_region != iter.key().regionKey())
        {
          prog.incrementProgress();
          last_region = iter.key().regionKey();
        }
        if (node.isOccupied())
        {
          v = map.voxelCentreLocal(node.key());
          ply.addVertex(v);
          ++point_count;
        }
      }

      prog.endProgress();
      prog.pause();
      if (!opt.quiet)
      {
        std::cout << "\nExported " << point_count << " point(s)" << std::endl;
      }

      if (quit < 2)
      {
        std::string output_file = opt.output_base_name + ".ply";
        std::cout << "Saving point cloud to " << output_file.c_str() << std::endl;
        ply.save(output_file.c_str(), true);
      }
    }
  }

  prog.joinThread();

  return 0;
}


namespace
{
  void printOptions(std::ostream **out, const Options &opt, const ohm::OccupancyMap &map)
  {
    while (*out)
    {
      **out << "Cloud: " << opt.cloud_file;
      if (!opt.trajectory_file.empty())
      {
        **out << " + " << opt.trajectory_file << '\n';
      }
      else
      {
        **out << " (no trajectory)\n";
      }
      if (opt.preload_count)
      {
        **out << "Preload: ";
        if (opt.preload_count < 0)
        {
          **out << "all";
        }
        else
        {
          **out << opt.preload_count;
        }
        **out << '\n';
      }

      if (opt.point_limit)
      {
        **out << "Maximum point: " << opt.point_limit << '\n';
      }

      if (opt.start_time)
      {
        **out << "Process from timestamp: " << opt.start_time << '\n';
      }

      if (opt.time_limit)
      {
        **out << "Process to timestamp: " << opt.time_limit << '\n';
      }

      std::string mem_size_string;
      util::makeMemoryDisplayString(mem_size_string, ohm::OccupancyMap::nodeMemoryPerRegion(opt.region_voxel_dim));
      **out << "Map resolution: " << opt.resolution << '\n';
      glm::i16vec3 region_dim = opt.region_voxel_dim;
      region_dim.x = (region_dim.x) ? region_dim.x : OHM_DEFAULT_CHUNK_DIM_X;
      region_dim.y = (region_dim.y) ? region_dim.y : OHM_DEFAULT_CHUNK_DIM_Y;
      region_dim.z = (region_dim.z) ? region_dim.z : OHM_DEFAULT_CHUNK_DIM_Z;
      **out << "Map region dimensions: " << region_dim << '\n';
      **out << "Map region memory: " << mem_size_string << '\n';
      **out << "Hit probability: " << opt.prob_hit << " (" << ohm::probabilityToValue(opt.prob_hit) << ")\n";
      **out << "Miss probability: " << opt.prob_miss << " (" << ohm::probabilityToValue(opt.prob_miss) << ")\n";
      **out << "Occupancy threshold: " << opt.prob_thresh << " (" << ohm::probabilityToValue(opt.prob_thresh) << ")\n";
      **out << "Probability range: [" << map.minNodeProbability() << ' ' << map.maxNodeProbability() << "]\n";
      **out << "Ray batch size: " << opt.batch_size << '\n';

      ++out;
    }
  }
}

int parseOptions(Options &opt, int argc, char *argv[])
{
  cxxopts::Options opt_parse(argv[0],
                            "Generate an occupancy map from a LAS/LAZ based point cloud and accompanying "
                            "trajectory file using GPU. The trajectory marks the scanner trajectory with timestamps "
                            "loosely corresponding to cloud point timestamps. Trajectory points are "
                            "interpolated for each cloud point based on corresponding times in the "
                            "trajectory.");
  opt_parse.positional_help("<cloud.laz> <_traj.txt> [output-base]");

  try
  {
    // Build GPU options set.
    std::vector<int> gpu_options_types(ohm::gpuArgsInfo(nullptr, nullptr, 0));
    std::vector<const char *> gpu_options(gpu_options_types.size() * 2);
    ohm::gpuArgsInfo(gpu_options.data(), gpu_options_types.data(), unsigned(gpu_options_types.size()));

    // clang-format off
    opt_parse.add_options()
      ("b,batch-size", "The number of points to process in each batch. Controls debug display.", optVal(opt.batch_size))
      ("help", "Show help.")
      ("i,cloud", "The input cloud (las/laz) to load.", cxxopts::value(opt.cloud_file))
      ("o,output","Output base name", optVal(opt.output_base_name))
      ("p,point-limit", "Limit the number of points loaded.", optVal(opt.point_limit))
      ("preload", "Preload this number of points before starting processing. Zero for all. May be used for separating processing and loading time.", optVal(opt.preload_count)->default_value("0"))
      ("q,quiet", "Run in quiet mode. Suppresses progress messages.", optVal(opt.quiet))
      ("s,start-time", "Only process points time stamped later than the specified time.", optVal(opt.start_time))
      ("save-info", "Save timing information to text based on the output file name.", optVal(opt.save_info))
      ("serialise", "Serialise the results? This option is intended for skipping saving during performance analysis.", optVal(opt.serialise))
      ("t,time-limit", "Limit the elapsed time in the LIDAR data to process (seconds). Measured relative to the first data sample.", optVal(opt.time_limit))
      ("trajectory", "The trajectory (text) file to load.", cxxopts::value(opt.trajectory_file))
      ;

    opt_parse.add_options("Map")
      ("clamp", "Set probability clamping to the given min/max.", optVal(opt.prob_range))
      ("d,dim", "Set the voxel dimensions of each region in the map. Range for each is [0, 255).", optVal(opt.region_voxel_dim))
      ("h,hit", "The occupancy probability due to a hit. Must be >= 0.5.", optVal(opt.prob_hit))
      ("m,miss", "The occupancy probability due to a miss. Must be < 0.5.", optVal(opt.prob_miss))
      ("r,resolution", "The voxel resolution of the generated map.", optVal(opt.resolution))
      ("threshold", "Sets the occupancy threshold assigned when exporting the map to a cloud.", optVal(opt.prob_thresh)->implicit_value(optStr(opt.prob_thresh)))
      ;

    opt_parse.add_options("Mapping")
      ("clearance", "Calculate clearance values for the map using this as the maximum search range. Zero to disable.", optVal(opt.clearance))
      ("progressive", "Time slice allowed for progressive mapping processes. Zero to disable and update after population.", optVal(opt.progressive_mapping_slice))
      ("progressive-interval", "Interval for progressive mapping. Time is based on input data time.", cxxopts::value(opt.mapping_interval)->default_value(optStr(opt.mapping_interval)))
      ("post-mapping", "Allow mapping thread to complete after population?", optVal(opt.post_population_mapping))
      ;

    // clang-format on

    if (!gpu_options.empty())
    {
      auto adder = opt_parse.add_options("GPU");
      for (size_t i = 0; i < gpu_options_types.size(); ++i)
      {
        adder(gpu_options[(i << 1) + 0], gpu_options[(i << 1) + 1],
              gpu_options_types[i] == 0 ? ::cxxopts::value<bool>() : ::cxxopts::value<std::string>());
      }
    }


    opt_parse.parse_positional({ "cloud", "trajectory", "output" });

    cxxopts::ParseResult parsed = opt_parse.parse(argc, argv);

    if (parsed.count("help") || parsed.arguments().empty())
    {
      // show usage.
      std::cout << opt_parse.help({ "", "Map", "Mapping", "GPU" }) << std::endl;
      return 1;
    }

    if (opt.cloud_file.empty())
    {
      std::cerr << "Missing input cloud" << std::endl;
      return -1;
    }
    if (opt.trajectory_file.empty())
    {
      std::cerr << "Missing trajectory file" << std::endl;
      return -1;
    }
  }
  catch (const cxxopts::OptionException &e)
  {
    std::cerr << "Argument error\n" << e.what() << std::endl;
    return -1;
  }

  return 0;
}

int main(int argc, char *argv[])
{
  Options opt;

  std::cout.imbue(std::locale(""));

  int res = parseOptions(opt, argc, argv);

  if (res)
  {
    return res;
  }

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  // Generate output name based on input if not specified.
  if (opt.output_base_name.empty())
  {
    const auto extension_start = opt.cloud_file.find_last_of(".");
    if (extension_start != std::string::npos)
    {
      opt.output_base_name = opt.cloud_file.substr(0, extension_start);
    }
    else
    {
      opt.output_base_name = opt.cloud_file;
    }
  }

  res = ohm::configureGpuFromArgs(argc, argv);
  if (res)
  {
    return res;
  }

  res = populateMap(opt);
  return res;
}
