add_library(simbehavior_adapters STATIC
  command_bus_impl.cpp
)

target_include_directories(simbehavior_adapters
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_adapters
  PUBLIC
    simbehavior_async_runtime
    simbehavior_domain
)

target_compile_features(simbehavior_adapters PUBLIC cxx_std_17)
target_link_libraries(simbehavior_adapters PRIVATE simbehavior_compile_warnings)
