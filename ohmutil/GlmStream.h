// Copyright (c) 2018
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#ifndef OHMUTIL_GLMSTREAM_H
#define OHMUTIL_GLMSTREAM_H

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <iostream>

namespace ohm
{
const int kMaxVectorElements = 32;
}  // namespace ohm

template <typename T>
inline void parseVector(std::istream &in, T *v, int elements)
{
  // Expecting:
  //  <value>,<value>[,...]

  char ch;
  bool brackets = false;
  if (in.peek() == '(')
  {
    brackets = true;
    in >> ch;
  }
  for (int i = 0; i < elements; ++i)
  {
    if (i != 0)
    {
      in >> ch;
      if (ch != ',')
      {
        in.setstate(std::ios::badbit | std::ios::failbit);
      }
    }
    in >> v[i];
  }
  if (brackets)
  {
    in >> ch;
    if (ch != ')')
    {
      in.setstate(std::ios::badbit | std::ios::failbit);
    }
  }
}


inline void parseVector(std::istream &in, char *v, int elements)
{
  std::array<int, ohm::kMaxVectorElements> vv;
  if (elements > int(vv.size()))
  {
    in.setstate(std::ios::badbit);
  }
  parseVector(in, vv.data(), elements);
  for (int i = 0; i < elements; ++i)
  {
    v[i] = char(vv[i]);
  }
}


inline void parseVector(std::istream &in, unsigned char *v, int elements)
{
  std::array<int, ohm::kMaxVectorElements> vv;
  if (elements > int(vv.size()))
  {
    in.setstate(std::ios::badbit);
  }
  parseVector(in, vv.data(), elements);
  for (int i = 0; i < elements; ++i)
  {
    v[i] = char(vv[i]);
  }
}

// From GLM 0.9.9.1, numbering starting including a patch number.
// So 0.9.8 was version 98, 0.9.9 was 99 and 0.9.9.1 was 991
// This assumes version 1.0 will be 1000
#if GLM_VERSION < 99
using GlmQualifier = glm::precision;
#else   // GLM_VERSION
using GlmQualifier = glm::qualifier;
#endif  // GLM_VERSION


template <typename T, GlmQualifier Q>
inline std::istream &operator>>(std::istream &in, glm::tvec2<T, Q> &value)
{
  parseVector(in, glm::value_ptr(value), 2);
  return in;
}


template <typename T, GlmQualifier Q>
inline std::istream &operator>>(std::istream &in, glm::tvec3<T, Q> &value)
{
  parseVector(in, glm::value_ptr(value), 3);
  return in;
}


template <typename T, GlmQualifier Q>
inline std::istream &operator>>(std::istream &in, glm::tvec4<T, Q> &value)
{
  parseVector(in, glm::value_ptr(value), 4);
  return in;
}

template <typename T, GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec2<T, Q> &v)
{
  out << '(' << v.x << ',' << v.y << ')';
  return out;
}

template <typename T, GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec3<T, Q> &v)
{
  out << '(' << v.x << ',' << v.y << ',' << v.z << ')';
  return out;
}

template <typename T, GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec4<T, Q> &v)
{
  out << '(' << v.w << ',' << v.x << ',' << v.y << ',' << v.z << ')';
  return out;
}

template <GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec3<char, Q> &v)
{
  out << '(' << int(v.x) << ',' << int(v.y) << ',' << int(v.z) << ')';
  return out;
}

template <GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec3<unsigned char, Q> &v)
{
  out << '(' << int(v.x) << ',' << int(v.y) << ',' << int(v.z) << ')';
  return out;
}

template <GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec4<char, Q> &v)
{
  out << '(' << int(v.w) << ',' << int(v.x) << ',' << int(v.y) << ',' << int(v.z) << ')';
  return out;
}

template <GlmQualifier Q>
inline std::ostream &operator<<(std::ostream &out, const glm::tvec4<unsigned char, Q> &v)
{
  out << '(' << int(v.w) << ',' << int(v.x) << ',' << int(v.y) << ',' << int(v.z) << ')';
  return out;
}

#endif  // OHMUTIL_GLMSTREAM_H
