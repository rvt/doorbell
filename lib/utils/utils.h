#pragma once

#include <stdint.h>
#include <vector>
#include <array>
#include <cstring>
#include <string>
#include <optparser.hpp>
#include <iostream>
#include <algorithm>

/**
 * Split a string into an array
 */
template <typename T, std::size_t SIZE>


/**
 * Map input range to output range
 */
inline float fmap(float x, float in_min, float in_max, float out_min, float out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
};

/**
 * Ensure that a value is between and including a minimum and a maximum value
 */
template <typename T>
inline T between(const T& n, const T& lower, const T& upper) {
    return std::max(lower, std::min(n, upper));
}

inline float percentmap(float value, float out_max) {
    return value * out_max / 100.f;
}