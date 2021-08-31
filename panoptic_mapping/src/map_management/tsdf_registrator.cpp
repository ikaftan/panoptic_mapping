#include "panoptic_mapping/map_management/tsdf_registrator.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <voxblox/integrator/merge_integration.h>
#include <voxblox/mesh/mesh_integrator.h>

#include "panoptic_mapping/map/submap.h"
#include "panoptic_mapping/map/submap_collection.h"

namespace panoptic_mapping {

void TsdfRegistrator::Config::checkParams() const {
  checkParamNE(error_threshold, 0.f, "error_threshold");
  checkParamGE(min_voxel_weight, 0.f, "min_voxel_weight");
  checkParamGE(match_rejection_points, 0, "match_rejection_points");
  checkParamGE(match_rejection_percentage, 0.f, "match_rejection_percentage");
  checkParamGE(match_acceptance_points, 0, "match_acceptance_points");
  checkParamGE(match_acceptance_percentage, 0.f, "match_acceptance_percentage");
  if (normalize_by_voxel_weight) {
    checkParamGT(normalization_max_weight, 0.f, "normalization_max_weight");
  }
}

void TsdfRegistrator::Config::setupParamsAndPrinting() {
  setupParam("verbosity", &verbosity);
  setupParam("min_voxel_weight", &min_voxel_weight);
  setupParam("error_threshold", &error_threshold);
  setupParam("match_rejection_points", &match_rejection_points);
  setupParam("match_rejection_percentage", &match_rejection_percentage);
  setupParam("match_acceptance_points", &match_acceptance_points);
  setupParam("match_acceptance_percentage", &match_acceptance_percentage);
  setupParam("normalize_by_voxel_weight", &normalize_by_voxel_weight);
  setupParam("normalization_max_weight", &normalization_max_weight);
}

TsdfRegistrator::TsdfRegistrator(const Config& config)
    : config_(config.checkValid()) {
  LOG_IF(INFO, config_.verbosity >= 1) << "\n" << config_.toString();
}

void TsdfRegistrator::checkSubmapCollectionForChange(
    SubmapCollection* submaps) const {
  auto t_start = std::chrono::high_resolution_clock::now();
  std::stringstream info;

  // Check all inactive maps for alignment with the currently active ones.
  for (Submap& submap : *submaps) {
    if (submap.isActive() || submap.getLabel() == PanopticLabel::kFreeSpace ||
        submap.getIsoSurfacePoints().empty()) {
      continue;
    }

    // Check overlapping submaps for conflicts or matches.
    for (const Submap& other : *submaps) {
      if (!other.isActive() ||
          !submap.getBoundingVolume().intersects(other.getBoundingVolume())) {
        continue;
      }
      // TEST(schmluk): For the moment exclude free space for thin structures.
      // if (other.getLabel() == PanopticLabel::kFreeSpace &&
      //     submap.getConfig().voxel_size < other.getConfig().voxel_size * 0.5)
      //     {
      //   continue;
      // }

      bool submaps_match;
      if (submapsConflict(submap, other, &submaps_match)) {
        // No conflicts allowed.
        if (submap.getChangeState() != ChangeState::kAbsent) {
          info << "\nSubmap " << submap.getID() << " (" << submap.getName()
               << ") conflicts with submap " << other.getID() << " ("
               << other.getName() << ").";
          submap.setChangeState(ChangeState::kAbsent);
        }
        break;
      } else if (submap.getClassID() == other.getClassID() && submaps_match) {
        // Semantically and geometrically match.
        submap.setChangeState(ChangeState::kPersistent);
      }
    }
  }
  auto t_end = std::chrono::high_resolution_clock::now();

  LOG_IF(INFO, config_.verbosity >= 2)
      << "Performed change detection in "
      << std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start)
             .count()
      << (config_.verbosity < 3 || info.str().empty() ? "ms."
                                                      : "ms:" + info.str());
}

bool TsdfRegistrator::submapsConflict(const Submap& reference,
                                      const Submap& other,
                                      bool* submaps_match) const {
  // Reference is the finished submap (with Iso-surfce-points) that is
  // compared to the active submap other.
  Transformation T_O_R = other.getT_S_M() * reference.getT_M_S();
  const float rejection_count =
      config_.normalize_by_voxel_weight
          ? std::numeric_limits<float>::max()
          : std::max(static_cast<float>(config_.match_rejection_points),
                     config_.match_rejection_percentage *
                         reference.getIsoSurfacePoints().size());
  const float rejection_distance =
      config_.error_threshold > 0.f
          ? config_.error_threshold
          : config_.error_threshold * -other.getTsdfLayer().voxel_size();
  float conflicting_points = 0.f;
  float matched_points = 0.f;
  float total_weight = 0.f;
  voxblox::Interpolator<TsdfVoxel> interpolator(&(other.getTsdfLayer()));

  // Check for disagreement.
  float distance, weight;
  for (const auto& point : reference.getIsoSurfacePoints()) {
    if (getDistanceAndWeightAtPoint(&distance, &weight, point, T_O_R,
                                    interpolator)) {
      // Compute the weight to be used for counting.
      if (config_.normalize_by_voxel_weight) {
        weight = computeCombinedWeight(weight, point.weight);
        total_weight += weight;
      } else {
        weight = 1.f;
      }

      // Count.
      if (other.getLabel() == PanopticLabel::kFreeSpace) {
        if (distance >= rejection_distance) {
          conflicting_points += weight;
        }
      } else {
        // Check for class belonging.
        if (other.hasClassLayer()) {
          if (!classVoxelBelongsToSubmap(
                  *other.getClassLayer().getVoxelPtrByCoordinates(
                      point.position))) {
            distance = other.getConfig().truncation_distance;
          }
        }
        if (distance <= -rejection_distance) {
          conflicting_points += weight;
        } else if (distance <= rejection_distance) {
          matched_points += weight;
        }
      }

      if (conflicting_points > rejection_count) {
        // If the rejection count is known and reached submaps conflict.
        if (submaps_match) {
          *submaps_match = false;
        }
        std::cout << "Rejection count reached early." << std::endl;
        return true;
      }
    }
  }
  // Evaluate the result.
  if (config_.normalize_by_voxel_weight) {
    const float rejection_weight =
        std::max(static_cast<float>(config_.match_rejection_points) /
                     reference.getIsoSurfacePoints().size(),
                 config_.match_rejection_percentage) *
        total_weight;
    if (conflicting_points > rejection_weight) {
      if (submaps_match) {
        *submaps_match = false;
      }
      std::cout << "Conflict: " << conflicting_points << " > "
                << rejection_weight << std::endl;
      return true;
    } else if (submaps_match) {
      const float acceptance_weight =
          std::max(static_cast<float>(config_.match_acceptance_points) /
                       reference.getIsoSurfacePoints().size(),
                   config_.match_acceptance_percentage) *
          total_weight;
      if (matched_points > acceptance_weight) {
        std::cout << "Match: " << matched_points << " > " << acceptance_weight
                  << std::endl;
        *submaps_match = true;
      } else {
        *submaps_match = false;
      }
    }
  } else {
    if (submaps_match) {
      const float acceptance_count =
          std::max(static_cast<float>(config_.match_acceptance_points),
                   config_.match_acceptance_percentage *
                       reference.getIsoSurfacePoints().size());
      *submaps_match = matched_points > acceptance_count;
    }
  }
  return false;
}  // namespace panoptic_mapping

bool TsdfRegistrator::getDistanceAndWeightAtPoint(
    float* distance, float* weight, const IsoSurfacePoint& point,
    const Transformation& T_P_S,
    const voxblox::Interpolator<TsdfVoxel>& interpolator) const {
  // Check minimum input point weight.
  if (point.weight < config_.min_voxel_weight) {
    return false;
  }

  // Try to interpolate the voxel in the  map.
  // NOTE(Schmluk): This also interpolates color etc, but otherwise the
  // interpolation lookup has to be done twice. Getting only what we want is
  // also in voxblox::interpolator but private, atm not performance critical.
  TsdfVoxel voxel;
  const Point position = T_P_S * point.position;
  if (!interpolator.getVoxel(position, &voxel, true)) {
    return false;
  }
  if (voxel.weight < config_.min_voxel_weight) {
    return false;
  }
  *distance = voxel.distance;
  *weight = voxel.weight;
  return true;
}

float TsdfRegistrator::computeCombinedWeight(float w1, float w2) const {
  if (w1 <= 0.f || w2 <= 0.f) {
    return 0.f;
  } else if (w1 >= config_.normalization_max_weight &&
             w2 >= config_.normalization_max_weight) {
    return 1.f;
  } else {
    return std::sqrt(std::min(w1 / config_.normalization_max_weight, 1.f) *
                     std::min(w2 / config_.normalization_max_weight, 1.f));
  }
}

void TsdfRegistrator::mergeMatchingSubmaps(SubmapCollection* submaps) {
  // Merge all submaps of identical Instance ID into one.
  // TODO(schmluk): This is a preliminary function for prototyping, update
  // this.
  submaps->updateInstanceToSubmapIDTable();
  int merged_maps = 0;
  for (const auto& instance_submaps : submaps->getInstanceToSubmapIDTable()) {
    const auto& ids = instance_submaps.second;
    Submap* target;
    for (auto it = ids.begin(); it != ids.end(); ++it) {
      if (it == ids.begin()) {
        target = submaps->getSubmapPtr(*it);
        continue;
      }
      // Merging.
      merged_maps++;
      voxblox::mergeLayerAintoLayerB(submaps->getSubmap(*it).getTsdfLayer(),
                                     target->getTsdfLayerPtr().get());
      submaps->removeSubmap(*it);
    }
    // Set the updated flags of the changed layer.
    voxblox::BlockIndexList block_list;
    target->getTsdfLayer().getAllAllocatedBlocks(&block_list);
    for (auto& index : block_list) {
      target->getTsdfLayerPtr()->getBlockByIndex(index).setUpdatedAll();
    }
  }
  LOG_IF(INFO, config_.verbosity >= 2)
      << "Merged " << merged_maps << " submaps.";
}

}  // namespace panoptic_mapping
