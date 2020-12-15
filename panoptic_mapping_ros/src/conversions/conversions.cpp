#include "panoptic_mapping_ros/conversions/conversions.h"

namespace panoptic_mapping {

DetectronIDTracker::DetectronLabel detectronLabelFromMsg(
    const panoptic_mapping_msgs::DetectronLabel& msg) {
  DetectronIDTracker::DetectronLabel result;
  result.id = msg.id;
  result.instance_id = msg.instance_id;
  result.is_thing = msg.is_thing;
  result.category_id = msg.category_id;
  result.score = msg.score;
  return result;
}

DetectronIDTracker::DetectronLabels detectronLabelsFromMsg(
    const panoptic_mapping_msgs::DetectronLabels& msg) {
  DetectronIDTracker::DetectronLabels result;
  for (const panoptic_mapping_msgs::DetectronLabel& label : msg.labels) {
    result[label.id] = detectronLabelFromMsg(label);
  }
  return result;
}

}  // namespace panoptic_mapping
