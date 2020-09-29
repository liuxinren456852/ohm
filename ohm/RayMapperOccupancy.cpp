//
// Author: Kazys Stepanas
// Copyright (c) CSIRO 2020
//
#include "RayMapperOccupancy.h"

#include "OccupancyMap.h"
#include "MapLayer.h"
#include "MapLayout.h"
#include "VoxelMean.h"
#include "VoxelOccupancy.h"

#include <ohmutil/LineWalk.h>

using namespace ohm;

RayMapperOccupancy::RayMapperOccupancy(OccupancyMap *map)
  : map_(map)
  , occupancy_layer_(map_->layout().occupancyLayer())
  , mean_layer_(map_->layout().meanLayer())
{
  occupancy_dim_ = map_->layout().layer(occupancy_layer_).dimensions(map_->regionVoxelDimensions());
  // Validate the mean layer size.
  if (mean_layer_ >= 0)
  {
    if (map_->layout().layer(mean_layer_).voxelByteSize() != sizeof(VoxelMean))
    {
      // Won't be using voxel mean. Wrong voxel size.
      mean_layer_ = -1;
    }
    else if (map_->layout().layer(mean_layer_).dimensions(map_->regionVoxelDimensions()) != occupancy_dim_)
    {
      // Won't be using voxel mean. Layer dimensions don't match occupancy layer.
      mean_layer_ = -1;
    }
  }
  valid_ = true;
}


RayMapperOccupancy::~RayMapperOccupancy() = default;


size_t RayMapperOccupancy::integrateRays(const glm::dvec3 *rays, size_t element_count, unsigned ray_update_flags)
{
  KeyList keys;
  MapChunk *last_chunk = nullptr;
  bool stop_adjustments = false;

  const RayFilterFunction ray_filter = map_->rayFilter();
  const bool use_filter = bool(ray_filter);
  const auto occupancy_layer = occupancy_layer_;
  const auto mean_layer = mean_layer_;
  const auto occupancy_dim = occupancy_dim_;
  const auto occupancy_threshold_value = map_->occupancyThresholdValue();
  const auto map_origin = map_->origin();
  const auto miss_value = map_->missValue();
  const auto hit_value = map_->hitValue();
  const auto resolution = map_->resolution();
  const auto voxel_min = map_->minVoxelValue();
  const auto voxel_max = map_->maxVoxelValue();
  const auto saturation_min = map_->saturateAtMinValue() ? voxel_min : std::numeric_limits<float>::lowest();
  const auto saturation_max = map_->saturateAtMaxValue() ? voxel_max : std::numeric_limits<float>::max();
  // Touch the map to flag changes.
  const auto touch_stamp = map_->touch();

  const auto visit_func = [&](const Key &key)  //
  {                                            //
    // The update logic here is a little unclear as it tries to avoid outright branches.
    // The intended logic is described as follows:
    // 1. Select direct write or additive adjustment.
    //    - Make a direct, non-additive adjustment if one of the following conditions are met:
    //      - stop_adjustments is true
    //      - the voxel is uncertain
    //      - (ray_update_flags & kRfClearOnly) and not is_occupied - we only want to adjust occupied voxels.
    //      - voxel is saturated
    //    - Otherwise add to present value.
    // 2. Select the value adjustment
    //    - current_value if one of the following conditions are met:
    //      - stop_adjustments is true (no longer making adjustments)
    //      - (ray_update_flags & kRfClearOnly) and not is_occupied (only looking to affect occupied voxels)
    //    - miss_value otherwise
    // 3. Calculate new value
    // 4. Apply saturation logic: only min saturation relevant
    //    -
    MapChunk *chunk =
      (last_chunk && key.regionKey() == last_chunk->region.coord) ? last_chunk : map_->region(key.regionKey(), true);
    last_chunk = chunk;
    const unsigned voxel_index = ::voxelIndex(key, occupancy_dim);
    float *occupancy_value = reinterpret_cast<float *>(chunk->voxel_maps[occupancy_layer]) + voxel_index;
    const float initial_value = *occupancy_value;
    const bool is_occupied = (initial_value != unobservedOccupancyValue() && initial_value > occupancy_threshold_value);
    occupancyAdjustMiss(occupancy_value, initial_value, miss_value, unobservedOccupancyValue(), voxel_min,
                        saturation_min, saturation_max, stop_adjustments);
    chunk->updateFirstValid(voxel_index);

    stop_adjustments = stop_adjustments || ((ray_update_flags & kRfStopOnFirstOccupied) && is_occupied);
    chunk->dirty_stamp = touch_stamp;
    // Update the touched_stamps with relaxed memory ordering. The important thing is to have an update,
    // not so much the sequencing. We really don't want to synchronise here.
    chunk->touched_stamps[occupancy_layer].store(touch_stamp, std::memory_order_relaxed);
  };

  glm::dvec3 start, end;
  unsigned filter_flags;
  for (size_t i = 0; i < element_count; i += 2)
  {
    filter_flags = 0;
    start = rays[i];
    end = rays[i + 1];

    if (use_filter)
    {
      if (!ray_filter(&start, &end, &filter_flags))
      {
        // Bad ray.
        continue;
      }
    }

    const bool include_sample_in_ray =
      (filter_flags & kRffClippedEnd) || (ray_update_flags & kRfEndPointAsFree) || (ray_update_flags & kRfClearOnly);

    if (!(ray_update_flags & kRfExcludeRay))
    {
      // Calculate line key for the last voxel if the end point has been clipped
      const glm::dvec3 start_point_local = glm::dvec3(start - map_origin);
      const glm::dvec3 end_point_local = glm::dvec3(end - map_origin);

      stop_adjustments = false;
      ohm::walkSegmentKeys<Key>(visit_func, start_point_local, end_point_local, include_sample_in_ray,
                                WalkKeyAdaptor(*map_));
    }

    if (!stop_adjustments && !include_sample_in_ray && !(ray_update_flags & (kRfClearOnly | kRfExcludeSample)) &&
        !(ray_update_flags & kRfExcludeSample))
    {
      // Like the miss logic, we have similar obfuscation here to avoid branching. It's a little simpler though,
      // because we do have a branch above, which will filter some of the conditions catered for in miss integration.
      const ohm::Key key = map_->voxelKey(end);
      MapChunk *chunk =
        (last_chunk && key.regionKey() == last_chunk->region.coord) ? last_chunk : map_->region(key.regionKey(), true);
      last_chunk = chunk;
      const unsigned voxel_index = ::voxelIndex(key, occupancy_dim);

      float *occupancy_value = reinterpret_cast<float *>(chunk->voxel_maps[occupancy_layer]) + voxel_index;
      const float initial_value = *occupancy_value;
      occupancyAdjustHit(occupancy_value, initial_value, hit_value, unobservedOccupancyValue(), voxel_max,
                         saturation_min, saturation_max, stop_adjustments);

      // update voxel mean if present.
      if (mean_layer >= 0)
      {
        VoxelMean *voxel_mean = reinterpret_cast<VoxelMean *>(chunk->voxel_maps[mean_layer]) + voxel_index;
        voxel_mean->coord =
          subVoxelUpdate(voxel_mean->coord, voxel_mean->count, end - map_->voxelCentreGlobal(key), resolution);
        ++voxel_mean->count;
        chunk->touched_stamps[mean_layer].store(touch_stamp, std::memory_order_relaxed);
      }

      chunk->updateFirstValid(voxel_index);

      chunk->dirty_stamp = touch_stamp;
      // Update the touched_stamps with relaxed memory ordering. The important thing is to have an update,
      // not so much the sequencing. We really don't want to synchronise here.
      chunk->touched_stamps[occupancy_layer].store(touch_stamp, std::memory_order_relaxed);
    }
  }

  return element_count / 2;
}