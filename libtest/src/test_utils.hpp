#include <catch2/catch.hpp>

#include <memory>
#include <utils.h>
#include <array>


TEST_CASE("Should beable to load vector from config", "[utils]") {

    SECTION("Should get values with equal data set size") {
        const char* inputDataSet = "k1=11,22,33";
        std::array<float, 3> currentValues = {1, 2, 3};
        std::array<float, 3>  newValues = getConfigArray("k1", "k1", inputDataSet, currentValues);
        REQUIRE(newValues[0] == Approx(11.0));
        REQUIRE(newValues[1] == Approx(22.0));
        REQUIRE(newValues[2] == Approx(33.0));
    }
    SECTION("Should get old data with un equal dataset (to much)") {
        const char* inputDataSet = "k2=34,35,36,37";
        std::array<float, 3> currentValues = {1, 2, 3};
        std::array<float, 3> newValues = getConfigArray("k1", "k1", inputDataSet, currentValues);
        REQUIRE(newValues[0] == Approx(34.0));
        REQUIRE(newValues[1] == Approx(35.0));
        REQUIRE(newValues[2] == Approx(36.0));
    }
    SECTION("Should get old data with un equal dataset (to little)") {
        const char* inputDataSet = "k2=34,35";
        std::array<float, 3> currentValues = {1, 2, 3};
        auto newValues = getConfigArray("k1", "k1", inputDataSet, currentValues);
        REQUIRE(newValues[0] == Approx(1.0));
        REQUIRE(newValues[1] == Approx(2.0));
        REQUIRE(newValues[2] == Approx(3.0));
    }

    SECTION("Should beable to split string") {
        splitString<float, 4>("1.2,5,8,6,-12,6.79", ',', [&](const std::string & value) {
            return std::stof(value);
        }
                             );
    }



}

