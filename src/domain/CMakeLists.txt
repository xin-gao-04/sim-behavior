add_library(simbehavior_domain STATIC
  entity/entity_context_impl.cpp
  group/group_context_impl.cpp
  world/world_snapshot_impl.cpp
  world/world_snapshot_provider_impl.cpp
)

target_include_directories(simbehavior_domain
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_domain
  PUBLIC
    # 无外部依赖：domain 层应尽量保持纯业务逻辑
  PRIVATE
    corekit
)

target_compile_features(simbehavior_domain PUBLIC cxx_std_17)
target_link_libraries(simbehavior_domain PRIVATE simbehavior_compile_warnings)
