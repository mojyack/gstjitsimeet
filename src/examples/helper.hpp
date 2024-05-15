#pragma once
#include <gst/gst.h>

auto add_new_element_to_pipeine(GstElement* const pipeline, const char* const element_name) -> GstElement*;
auto run_pipeline(GstElement* pipeline) -> bool;
