vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ankitlinkesh/ThunderBolt-HT
    REF 642294f22857f7db806f669cc3cff03ac772aead
    SHA512 ba0e8e9ad1f2971d2f615ec83bbcae1911b3e5df01b33b3a6978a307dc4e5f8dbdd989217eaf3c689ee38e88fb9ecedec23b8ca03d5b288bec78007d7d84ec1f
    HEAD_REF main
)

# thunderbolt/ is the standalone-buildable unit (S4) - engine/ and
# game_benchmarks/ at the repo root are the simulation that exercises the
# runtime, not the runtime itself, and are not part of this package.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/thunderbolt"
    OPTIONS
        -DTHUNDERBOLT_BUILD_TESTS=OFF
        -DTHUNDERBOLT_BUILD_EXAMPLES=OFF
        -DTHUNDERBOLT_BUILD_BENCHMARKS=OFF
        -DTHUNDERBOLT_INSTALL=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME Thunderbolt CONFIG_PATH lib/cmake/Thunderbolt)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
