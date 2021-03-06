// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#include "CalculateSegmentKeys.h"

#include "KeyList.h"

#include <ohmutil/LineWalk.h>

namespace ohm
{
size_t calculateSegmentKeys(KeyList &keys, const OccupancyMap &map, const glm::dvec3 &start_point,
                            const glm::dvec3 &end_point, bool include_end_point)
{
  const glm::dvec3 start_point_local = glm::dvec3(start_point - map.origin());
  const glm::dvec3 end_point_local = glm::dvec3(end_point - map.origin());

  keys.clear();
  return ohm::walkSegmentKeys<Key>([&keys](const Key &key) { keys.add(key); }, start_point_local, end_point_local,
                                   include_end_point, WalkKeyAdaptor(map));
}
}  // namespace ohm
