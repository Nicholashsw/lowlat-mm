cmake_minimum_required(VERSION 3.16)
project(lowlat_mm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Sensible warning level. Add -Wpedantic in CI if desired.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()

# --- Engine library ---------------------------------------------------------
# Compiled into a single static lib consumed by both the runner and the
# test binary. Headers live alongside the sources.
add_library(lowlat_mm_core STATIC
    core/Events.cpp
    orderbook/OrderBook.cpp
    market_data/CsvFeedReader.cpp
    execution/OMS.cpp
    risk/RiskManager.cpp
    simulator/ExchangeSimulator.cpp
    strategy/MarketMaker.cpp
    backtest/BacktestDriver.cpp
)
target_include_directories(lowlat_mm_core PUBLIC ${CMAKE_SOURCE_DIR})

# --- Runner -----------------------------------------------------------------
add_executable(run_backtest apps/run_backtest.cpp)
target_link_libraries(run_backtest PRIVATE lowlat_mm_core)

# --- Tests ------------------------------------------------------------------
enable_testing()
add_executable(unit_tests
    tests/test_main.cpp
    tests/test_orderbook.cpp
    tests/test_oms.cpp
    tests/test_risk.cpp
    tests/test_simulator.cpp
    tests/test_strategy.cpp
)
target_include_directories(unit_tests PRIVATE third_party)
target_link_libraries(unit_tests PRIVATE lowlat_mm_core)
add_test(NAME unit_tests COMMAND unit_tests)
