cmake_minimum_required(VERSION 3.16)
project(auto_viz)

enable_testing()

# Set up language settings
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Find Halide
find_package(Halide REQUIRED)

# Generator
add_executable(auto_viz.generator auto_viz_demo_generator.cpp)
target_link_libraries(auto_viz.generator PRIVATE Halide::Generator)

set(variants naive lessnaive complex)
set(filters "")
foreach (v IN LISTS variants)
    add_halide_library(auto_viz_${v}_up FROM auto_viz.generator
                       GENERATOR auto_viz_demo
                       FEATURES trace_all
                       PARAMS schedule_type=${v} upsample=true)
    add_halide_library(auto_viz_${v}_down FROM auto_viz.generator
                       GENERATOR auto_viz_demo
                       FEATURES trace_all
                       PARAMS schedule_type=${v} upsample=false)

    add_test(NAME auto_viz_${v}_up COMMAND resample rgb_small.png out_${v}_up.png -s ${v} -f 4.0)
    add_test(NAME auto_viz_${v}_down COMMAND resample rgb_small.png out_${v}_up.png -s ${v} -f 0.5)
    set_tests_properties(auto_viz_${v}_up auto_viz_${v}_down
                         PROPERTIES
                         LABELS auto_viz
                         PASS_REGULAR_EXPRESSION "Success!"
                         SKIP_REGULAR_EXPRESSION "\\[SKIP\\]")

    list(APPEND filters auto_viz_${v}_up auto_viz_${v}_down)
endforeach ()

# Main executable
add_executable(resample auto_viz_demo.cpp)
target_link_libraries(resample PRIVATE Halide::ImageIO ${filters})

# Test that the app actually works!
set(image ${CMAKE_CURRENT_LIST_DIR}/../images/rgb_small.png)
if (EXISTS ${image})
    configure_file(${image} rgb_small.png COPYONLY)
else ()
    set_tests_properties(${filters} PROPERTIES DISABLED TRUE)
endif ()
