# B4 native regression tests.
#
# They link the real engine static library, so the precise sampler, the volume
# integral and the cubiomes tables under test are exactly the shipped ones.

add_executable(lowyd-volume-unit volume_unit.cpp)
target_link_libraries(lowyd-volume-unit PRIVATE lowydripstonecavefinder-core)

add_executable(lowyd-column-reference column_reference.cpp)
target_link_libraries(lowyd-column-reference PRIVATE lowydripstonecavefinder-core)

add_test(NAME volume_unit COMMAND lowyd-volume-unit)

add_test(NAME column_reference
    COMMAND lowyd-column-reference
            ${CMAKE_CURRENT_SOURCE_DIR}/drowned-original-reference.tsv)
set_tests_properties(column_reference PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
