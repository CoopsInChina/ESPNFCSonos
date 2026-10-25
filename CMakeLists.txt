cmake_minimum_required(VERSION 3.16)
# managed_components/ is committed (fully resolved already — see
# .gitignore's comment) and the component manager is disabled in CI
# (IDF_COMPONENT_MANAGER=0, see .github/workflows/release.yml) to avoid
# it re-querying the live registry on every build. Disabling the manager
# also disables its own auto-detection of managed_components/, so it
# has to be registered explicitly here instead.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(album_nfc)
