//
// Project configuration header. This is a generated header; do not modify
// it directly. Instead, modify the config.h.in version and run CMake again.
//
#ifndef CLUCONFIG_H
#define CLUCONFIG_H

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif  // _USE_MATH_DEFINES
#ifndef NOMINMAX
#define NOMINMAX
#endif  // NOMINMAX

#ifdef _MSC_VER
// Avoid dubious security warnings for plenty of legitimate code
#ifndef _SCL_SECURE_NO_WARNINGS
#define _SCL_SECURE_NO_WARNINGS
#endif  // _SCL_SECURE_NO_WARNINGS
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif  // _CRT_SECURE_NO_WARNINGS
//#define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 1
#endif  // _MSC_VER

// clang-format off
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION @CLU_TARGET_OPENCL_VERSION@
// clang-format on

#endif  // CLUCONFIG_H
