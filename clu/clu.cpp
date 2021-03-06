//
// @author Kazys Stepanas
//
// Copyright (c) CSIRO 2016
//
#include "clu.h"

#include <mutex>
#include <sstream>

#include "cluConstraint.h"

#include <array>
#include <iostream>
#include <memory>

namespace
{
struct PrimaryContex
{
  cl::Context context;
  cl::Device device;
};

// Note: clang-tidy warns of potentially uncaught excepsions when the cl objects are static. Use a struct pointer to
// avoid the issue.
std::unique_ptr<PrimaryContex> g_context;
std::mutex g_lock;

struct InfoItem
{
  int id;
  std::string label;
};


void split(const std::string &str, char delim, std::vector<std::string> &tokens)
{
  size_t prev = 0;
  size_t pos = 0;
  do
  {
    pos = str.find(delim, prev);

    if (pos != std::string::npos)
    {
      tokens.push_back(str.substr(prev, pos - prev));
      prev = pos + 1;
    }
    else
    {
      tokens.push_back(str.substr(prev, str.length()));
    }
  } while (pos != std::string::npos);
}


std::string stripQuotes(const std::string &str)
{
  if (str.length() < 2)
  {
    return str;
  }

  if (str[0] == str[str.length() - 1])
  {
    if (str[0] == '"' || str[0] == '\'')  // fix syntax hilighting in VSCode :"
    {
      return str.substr(1, str.length() - 2);
    }
  }

  return str;
}
}  // namespace

namespace clu
{
enum ArgParse
{
  kApOk,
  kApMissingValue,
  kApParseFailure
};

cl::Platform createPlatform(cl_device_type type, const PlatformConstraint *constraints, unsigned constraint_count)
{
  std::vector<cl::Platform> platforms;
  std::vector<cl::Device> devices;
  cl::Platform::get(&platforms);

  for (cl::Platform &platform : platforms)
  {
    // Check constraints.
    bool constraints_ok = true;
    for (unsigned i = 0; i < constraint_count; ++i)
    {
      if (!(*constraints)(platform))
      {
        constraints_ok = false;
        break;
      }
    }

    if (!constraints_ok)
    {
      // A constraint has failed.
      continue;
    }

    if (type != CL_DEVICE_TYPE_ALL)
    {
      // Check device types.
      devices.clear();
      platform.getDevices(type, &devices);
      if (!devices.empty())
      {
        // Found a suitable platform.
        return platform;
      }
    }
    else
    {
      return platform;
    }
  }

  return cl::Platform();
}


cl::Platform createPlatform(cl_device_type type, const std::vector<PlatformConstraint> &constraints)
{
  return createPlatform(type, constraints.data(), unsigned(constraints.size()));
}


bool filterPlatforms(std::vector<cl::Platform> &platforms, cl_device_type /*type*/,
                     const PlatformConstraint *constraints, unsigned constraint_count)
{
  for (auto iter = platforms.begin(); iter != platforms.end();)
  {
    bool constraints_ok = true;
    for (unsigned i = 0; i < constraint_count; ++i)
    {
      if (!(*constraints)(*iter))
      {
        constraints_ok = false;
        break;
      }
    }

    if (constraints_ok)
    {
      ++iter;
    }
    else
    {
      iter = platforms.erase(iter);
    }
  }

  return !platforms.empty();
}


bool filterDevices(const cl::Platform &platform, std::vector<cl::Device> &devices, const DeviceConstraint *constraints,
                   unsigned constraint_count)
{
  for (auto iter = devices.begin(); iter != devices.end();)
  {
    bool constraints_ok = true;
    for (unsigned i = 0; i < constraint_count; ++i)
    {
      if (!(*constraints)(platform, *iter))
      {
        constraints_ok = false;
        break;
      }
    }

    if (constraints_ok)
    {
      ++iter;
    }
    else
    {
      iter = devices.erase(iter);
    }
  }

  return !devices.empty();
}


unsigned listDevices(std::vector<cl::Device> &devices, const cl::Context &context)
{
  cl_int device_count = 0;
  devices.clear();
  cl_int clerr = clGetContextInfo(context(), CL_CONTEXT_NUM_DEVICES, sizeof(device_count), &device_count, nullptr);

  if (device_count == 0 || clerr != CL_SUCCESS)
  {
    return 0;
  }

  auto *device_ids = static_cast<cl_device_id *>(alloca(sizeof(cl_device_id) * device_count));
  clerr = clGetContextInfo(context(), CL_CONTEXT_DEVICES, sizeof(cl_device_id) * device_count, device_ids, nullptr);
  if (clerr != CL_SUCCESS)
  {
    return 0;
  }

  devices.resize(device_count);
  for (cl_int i = 0; i < device_count; ++i)
  {
    devices[i] = cl::Device(device_ids[i]);
  }

  return device_count;
}


cl_device_id getFirstDevice(const cl::Context &context, cl_int *err)
{
  cl_int device_count = 0;
  cl_int clerr = clGetContextInfo(context(), CL_CONTEXT_NUM_DEVICES, sizeof(device_count), &device_count, nullptr);

  if (device_count == 0 || clerr != CL_SUCCESS)
  {
    if (err)
    {
      *err = clerr;
    }
    return nullptr;
  }

  auto *device_ids = static_cast<cl_device_id *>(alloca(sizeof(cl_device_id) * device_count));
  clerr = clGetContextInfo(context(), CL_CONTEXT_DEVICES, sizeof(cl_device_id) * device_count, device_ids, nullptr);
  if (err)
  {
    *err = clerr;
  }

  if (clerr != CL_SUCCESS)
  {
    return nullptr;
  }

  return device_ids[0];
}

#if 0
  cl::Context createContext(const cl::Platform &platform, cl_device_type type, const DeviceConstraint *constraints, unsigned constraintCount)
  {
    std::vector<cl::Device> devices;
    platform.getDevices(type, &devices);

    if (!filterDevices(platform, devices, constraints, constraintCount))
    {
      return cl::Context();
    }

    // Use the preferred platform and create a context
    cl_context_properties cprops[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)(platform)(), 0 };
    cl::Context context = cl::Context(devices, cprops);
    return context;
  }


  cl::Context createContext(const cl::Platform &platform, cl_device_type type, const std::vector<DeviceConstraint> &constraints)
  {
    return createContext(platform, type, constraints.data(), unsigned(constraints.size()));
  }
#endif  // #

cl::Context createContext(cl::Device *device_out, cl_device_type type, const PlatformConstraint *platform_constraint,
                          unsigned platform_constraint_count, const DeviceConstraint *device_constraints,
                          unsigned device_constraint_count)
{
  std::vector<cl::Platform> platforms;
  std::vector<cl::Device> devices;
  cl::Platform::get(&platforms);
  filterPlatforms(platforms, type, platform_constraint, platform_constraint_count);

  for (cl::Platform &platform : platforms)
  {
    devices.clear();

    // // Version check: 1.2 is the minimum supported or we can segfault.
    // cl_uint version_major = 0u;
    // cl_uint version_minor = 0u;
    // platformVersion(platform(), &version_major, &version_minor);

    // const cl_uint min_version_major = 1;
    // const cl_uint min_version_minor = 2;
    // if (version_major < min_version_major || version_major == min_version_major && version_minor <
    // min_version_minor)
    // {
    //   continue;
    // }

    platform.getDevices(type, &devices);
    filterDevices(platform, devices, device_constraints, device_constraint_count);
    if (!devices.empty())
    {
      // Select a single device.
      std::array<cl_context_properties, 3> cprops = { CL_CONTEXT_PLATFORM, cl_context_properties((platform)()), 0 };
      cl::Context context = cl::Context(devices[0], cprops.data());

      if (context())
      {
        if (device_out)
        {
          *device_out = devices[0];
        }
        return context;
      }
    }
  }

  return cl::Context();
}


bool setPrimaryContext(const cl::Context &context, const cl::Device &device)
{
  if (!context())
  {
    return false;
  }

  std::unique_lock<std::mutex> guard(g_lock);
  if (!g_context)
  {
    g_context = std::make_unique<PrimaryContex>();
  }
  g_context->context = context;
  g_context->device = device;
  return true;
}


void clearPrimaryContext()
{
  std::unique_lock<std::mutex> guard(g_lock);
  if (g_context)
  {
    g_context = std::make_unique<PrimaryContex>();
    g_context->context = cl::Context();
    g_context->device = cl::Device();
  }
}


bool initPrimaryContext(cl_device_type type, const PlatformConstraint *platform_constraints,
                        unsigned platform_constraint_count, const DeviceConstraint *device_constraints,
                        unsigned device_constraint_count)
{
  cl::Device default_device;
  cl::Context context = createContext(&default_device, type, platform_constraints, platform_constraint_count,
                                      device_constraints, device_constraint_count);
  return setPrimaryContext(context, default_device);
}


bool initPrimaryContext(cl_device_type type, const std::vector<PlatformConstraint> &platform_constraint,
                        const std::vector<DeviceConstraint> &device_constraints)
{
  return initPrimaryContext(type, platform_constraint.data(), unsigned(platform_constraint.size()),
                            device_constraints.data(), unsigned(device_constraints.size()));
}


bool getPrimaryContext(cl::Context &context, cl::Device &device)
{
  std::unique_lock<std::mutex> guard(g_lock);
  if (g_context)
  {
    context = g_context->context;
    device = g_context->device;
  }
  else
  {
    context = cl::Context();
    device = cl::Device();
  }

  return context() != nullptr;
}


ArgParse argValue(std::string &val, const std::string &arg, std::list<std::string>::const_iterator &iter,
                  const std::list<std::string>::const_iterator &end)
{
  std::string::size_type eqpos = arg.find('=');
  auto next = iter;

  ++next;
  // Check for form
  if (eqpos == std::string::npos)
  {
    // Not of the form "--arg=value". Read next argument.
    if (next != end)
    {
      val = *next;
      if (val.find("--") != 0)
      {
        // Good value. Consume the next argument.
        iter = next;
        return kApOk;
      }

      // Argument missing value.
      return kApMissingValue;
    }
    // Not enough additional arguments.
    return kApMissingValue;
  }

  // Of the form "--arg=value"
  val = arg.substr(eqpos + 1);
  return (!val.empty()) ? kApOk : kApMissingValue;
}


void constraintsFromCommandLine(int argc, const char **argv, cl_device_type &type,
                                std::vector<PlatformConstraint> &platform_constraints,
                                std::vector<DeviceConstraint> &device_constraints, const char *arg_prefix)
{
  std::list<std::string> args;
  for (int i = 0; i < argc; ++i)
  {
    args.emplace_back(argv[i]);
  }

  return constraintsFromArgs(args, type, platform_constraints, device_constraints, arg_prefix);
}


void constraintsFromArgs(const std::list<std::string> &args, cl_device_type &type,
                         std::vector<PlatformConstraint> &platform_constraints,
                         std::vector<DeviceConstraint> &device_constraints, const char *arg_prefix)
{
  std::string arg;
  std::string val;
  std::string prefix = (arg_prefix) ? std::string("--") + std::string(arg_prefix) : "--";
  std::vector<std::string> tokens;
  ArgParse parse_result = kApOk;

  if (!type)
  {
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    type = CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR;
  }

  for (auto iter = args.begin(); iter != args.end(); ++iter)
  {
    arg = *iter;
    if (arg.find(prefix) == 0)
    {
      arg.erase(0, prefix.length());
      if (arg.find("accel") == 0)
      {
        parse_result = argValue(val, arg, iter, args.end());
        if (parse_result == kApOk)
        {
          tokens.clear();
          split(val, ',', tokens);
          type = 0;
          for (const std::string &type_str : tokens)
          {
            if (type_str == "any")
            {
              type |= CL_DEVICE_TYPE_ALL;  // NOLINT(hicpp-signed-bitwise)
            }
            else if (type_str == "accel")
            {
              type |= CL_DEVICE_TYPE_ACCELERATOR;  // NOLINT(hicpp-signed-bitwise)
            }
            else if (type_str == "cpu")
            {
              type |= CL_DEVICE_TYPE_CPU;  // NOLINT(hicpp-signed-bitwise)
            }
            else if (type_str == "gpu")
            {
              type |= CL_DEVICE_TYPE_GPU;  // NOLINT(hicpp-signed-bitwise)
            }
            else
            {
              parse_result = kApParseFailure;
            }
          }
        }
      }
      else if (arg.find("clver") == 0)
      {
        parse_result = argValue(val, arg, iter, args.end());
        if (parse_result == kApOk)
        {
          int major = 0;
          int minor = 0;
          // Expecting: major[.minor]
          tokens.clear();
          split(val, '.', tokens);
          if (!tokens.empty() && tokens.size() <= 2)
          {
            std::istringstream str(tokens[0]);
            str >> major;
            if (str.fail())
            {
              parse_result = kApParseFailure;
            }
            if (tokens.size() > 1)
            {
              str = std::istringstream(tokens[1]);
              str >> minor;
              if (str.fail())
              {
                parse_result = kApParseFailure;
              }
            }

            if (parse_result == kApOk)
            {
              // Add a device version constraint.
              device_constraints.push_back(deviceVersionMin(major, minor));
            }
          }
          else
          {
            parse_result = kApParseFailure;
          }
        }
      }
      else if (arg.find("device") == 0)
      {
        parse_result = argValue(val, arg, iter, args.end());
        if (parse_result == kApOk)
        {
          val = stripQuotes(val);
          // Add a device constraint.
          device_constraints.push_back(deviceNameLike(val.c_str(), true));
        }
      }
      else if (arg.find("platform") == 0)
      {
        parse_result = argValue(val, arg, iter, args.end());
        if (parse_result == kApOk)
        {
          val = stripQuotes(val);
          // Add a device constraint.
          platform_constraints.push_back(platformNameLike(val.c_str(), true));
        }
      }
      else if (arg.find("vendor") == 0)
      {
        parse_result = argValue(val, arg, iter, args.end());
        if (parse_result == kApOk)
        {
          val = stripQuotes(val);
          // Add a device constraint.
          // platformConstraints.push_back(platformVendorLike(val.c_str(), true));
          device_constraints.push_back(deviceVendorLike(val.c_str(), true));
        }
      }

      if (parse_result != kApOk)
      {
        switch (parse_result)
        {
        case kApParseFailure:
          std::cerr << "Failed parsing argument '" << arg << "' value '" << val << "'" << std::endl;
          break;
        case kApMissingValue:
          std::cerr << "Argument '" << arg << "' missing value" << std::endl;
          break;
        default:
          std::cerr << "Error processing argument '" << arg << "'" << std::endl;
          break;
        }
      }
    }
  }
}


bool parseVersion(const char *version_string, cl_uint *version_major, cl_uint *version_minor)
{
  cl_uint high_version = 0;
  cl_uint low_version = 0;
  int index = 0;

  while (version_string[index] && !isdigit(version_string[index]))
  {
    ++index;
  }

  if (!version_string[index])
  {
    return false;
  }

  bool have_major = false;
  bool have_minor = false;
  const cl_uint base_shift = 10u;
  while (version_string[index] && isdigit(version_string[index]))
  {
    have_major = true;
    high_version *= base_shift;
    high_version += version_string[index] - '0';
    ++index;
  }

  // Verify and skip the '.'
  if (!version_string[index] || !version_string[++index])
  {
    return false;
  }

  while (version_string[index] && isdigit(version_string[index]))
  {
    have_minor = true;
    low_version *= base_shift;
    low_version += version_string[index] - '0';
    ++index;
  }

  if (version_major)
  {
    *version_major = high_version;
  }

  if (version_minor)
  {
    *version_minor = low_version;
  }

  return have_major && have_minor;
}


void platformVersion(cl_platform_id platform, cl_uint *version_major, cl_uint *version_minor)
{
  ::size_t size = 0;
  clGetPlatformInfo(platform, CL_PLATFORM_VERSION, 0, nullptr, &size);
  cl::string version_info;
  version_info.resize(size + 1);
  clGetPlatformInfo(platform, CL_PLATFORM_VERSION, size, &version_info[0], &size);
  parseVersion(version_info.c_str(), version_major, version_minor);
}


const char *errorCodeString(cl_int error)
{
#define CL_ERR_CASE(CODE) \
  case CODE:              \
    return #CODE;
#define CL_ERR_CASE2(CODE, msg) \
  case CODE:                    \
    return msg;

  switch (error)
  {
    CL_ERR_CASE(CL_SUCCESS)
    CL_ERR_CASE(CL_DEVICE_NOT_FOUND)
    CL_ERR_CASE(CL_DEVICE_NOT_AVAILABLE)
    CL_ERR_CASE(CL_COMPILER_NOT_AVAILABLE)
    CL_ERR_CASE(CL_MEM_OBJECT_ALLOCATION_FAILURE)
    CL_ERR_CASE(CL_OUT_OF_RESOURCES)
    CL_ERR_CASE(CL_OUT_OF_HOST_MEMORY)
    CL_ERR_CASE(CL_PROFILING_INFO_NOT_AVAILABLE)
    CL_ERR_CASE(CL_MEM_COPY_OVERLAP)
    CL_ERR_CASE(CL_IMAGE_FORMAT_MISMATCH)
    CL_ERR_CASE(CL_IMAGE_FORMAT_NOT_SUPPORTED)
    CL_ERR_CASE(CL_BUILD_PROGRAM_FAILURE)
    CL_ERR_CASE(CL_MAP_FAILURE)
    CL_ERR_CASE(CL_MISALIGNED_SUB_BUFFER_OFFSET)
    CL_ERR_CASE(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST)
    CL_ERR_CASE(CL_COMPILE_PROGRAM_FAILURE)
    CL_ERR_CASE(CL_LINKER_NOT_AVAILABLE)
    CL_ERR_CASE(CL_LINK_PROGRAM_FAILURE)
    CL_ERR_CASE(CL_DEVICE_PARTITION_FAILED)
    CL_ERR_CASE(CL_KERNEL_ARG_INFO_NOT_AVAILABLE)
    CL_ERR_CASE(CL_INVALID_VALUE)
    CL_ERR_CASE(CL_INVALID_DEVICE_TYPE)
    CL_ERR_CASE(CL_INVALID_PLATFORM)
    CL_ERR_CASE(CL_INVALID_DEVICE)
    CL_ERR_CASE(CL_INVALID_CONTEXT)
    CL_ERR_CASE(CL_INVALID_QUEUE_PROPERTIES)
    CL_ERR_CASE(CL_INVALID_COMMAND_QUEUE)
    CL_ERR_CASE(CL_INVALID_HOST_PTR)
    CL_ERR_CASE(CL_INVALID_MEM_OBJECT)
    CL_ERR_CASE(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)
    CL_ERR_CASE(CL_INVALID_IMAGE_SIZE)
    CL_ERR_CASE(CL_INVALID_SAMPLER)
    CL_ERR_CASE(CL_INVALID_BINARY)
    CL_ERR_CASE(CL_INVALID_BUILD_OPTIONS)
    CL_ERR_CASE(CL_INVALID_PROGRAM)
    CL_ERR_CASE(CL_INVALID_PROGRAM_EXECUTABLE)
    CL_ERR_CASE(CL_INVALID_KERNEL_NAME)
    CL_ERR_CASE(CL_INVALID_KERNEL_DEFINITION)
    CL_ERR_CASE(CL_INVALID_KERNEL)
    CL_ERR_CASE(CL_INVALID_ARG_INDEX)
    CL_ERR_CASE(CL_INVALID_ARG_VALUE)
    CL_ERR_CASE(CL_INVALID_ARG_SIZE)
    CL_ERR_CASE(CL_INVALID_KERNEL_ARGS)
    CL_ERR_CASE(CL_INVALID_WORK_DIMENSION)
    CL_ERR_CASE(CL_INVALID_WORK_GROUP_SIZE)
    CL_ERR_CASE(CL_INVALID_WORK_ITEM_SIZE)
    CL_ERR_CASE(CL_INVALID_GLOBAL_OFFSET)
    CL_ERR_CASE(CL_INVALID_EVENT_WAIT_LIST)
    CL_ERR_CASE(CL_INVALID_EVENT)
    CL_ERR_CASE(CL_INVALID_OPERATION)
    CL_ERR_CASE(CL_INVALID_GL_OBJECT)
    CL_ERR_CASE(CL_INVALID_BUFFER_SIZE)
    CL_ERR_CASE(CL_INVALID_MIP_LEVEL)
    CL_ERR_CASE(CL_INVALID_GLOBAL_WORK_SIZE)
    CL_ERR_CASE(CL_INVALID_PROPERTY)
    CL_ERR_CASE(CL_INVALID_IMAGE_DESCRIPTOR)
    CL_ERR_CASE(CL_INVALID_COMPILER_OPTIONS)
    CL_ERR_CASE(CL_INVALID_LINKER_OPTIONS)
    CL_ERR_CASE(CL_INVALID_DEVICE_PARTITION_COUNT)

#if CL_VERSION_2_0
    CL_ERR_CASE(CL_INVALID_PIPE_SIZE)
    CL_ERR_CASE(CL_INVALID_DEVICE_QUEUE)
#endif  // CL_VERSION_2_0

    // extension errors
#ifdef CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR
    CL_ERR_CASE(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)
#endif  // CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR

#ifdef CL_PLATFORM_NOT_FOUND_KHR
    CL_ERR_CASE(CL_PLATFORM_NOT_FOUND_KHR)
#endif  // CL_PLATFORM_NOT_FOUND_KHR

#ifdef CL_INVALID_D3D10_DEVICE_KHR
    CL_ERR_CASE(CL_INVALID_D3D10_DEVICE_KHR)
#endif  // CL_INVALID_D3D10_DEVICE_KHR

#ifdef CL_INVALID_D3D10_RESOURCE_KHR
    CL_ERR_CASE(CL_INVALID_D3D10_RESOURCE_KHR)
#endif  // CL_INVALID_D3D10_RESOURCE_KHR

#ifdef CL_D3D10_RESOURCE_ALREADY_ACQUIRED_KHR
    CL_ERR_CASE(CL_D3D10_RESOURCE_ALREADY_ACQUIRED_KHR)
#endif  // CL_D3D10_RESOURCE_ALREADY_ACQUIRED_KHR

#ifdef CL_D3D10_RESOURCE_NOT_ACQUIRED_KHR
    CL_ERR_CASE(CL_D3D10_RESOURCE_NOT_ACQUIRED_KHR)
#endif  // CL_D3D10_RESOURCE_NOT_ACQUIRED_KHR

  default:
    return "Unknown OpenCL error";
  }

#undef CL_ERR_CASE
#undef CL_ERR_CASE2
}


void printPlatformInfo(std::ostream &out, const cl::Platform &platform, const char *prefix, const char *endl)
{
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
  static const std::array<InfoItem, 3> items =  //
    {
      InfoItem{ CL_PLATFORM_NAME, "Name" },        //
      InfoItem{ CL_PLATFORM_VERSION, "Version" },  //
      InfoItem{ CL_PLATFORM_VENDOR, "Vendor" }     //
      //{ CL_PLATFORM_PROFILE, "Profile" },
      //{ CL_PLATFORM_EXTENSIONS, "Extensions" },
    };
  std::string info_str;
  std::string info_str2;
  if (platform())
  {
    bool first = true;
    for (const InfoItem &item : items)
    {
      platform.getInfo(cl_platform_info(item.id), &info_str);
      if (!first)
      {
        out << endl;
      }
      // Use c_str() otherwise we get extra '\0' characters.
      out << prefix << item.label << ": " << info_str;
      first = false;
    }
  }
}


void printDeviceInfo(std::ostream &out, const cl::Device &device, const char *prefix, const char *endl)
{
  static const std::array<InfoItem, 2> items =  //
    {
      InfoItem{ CL_DEVICE_NAME, "Name" },       //
      InfoItem{ CL_DEVICE_VERSION, "Version" }  //
      //{ CL_PLATFORM_PROFILE, "Profile" },
      //{ CL_PLATFORM_EXTENSIONS, "Extensions" },
    };
  std::string info_str;
  if (device())
  {
    bool first = true;
    for (const InfoItem &item : items)
    {
      device.getInfo(cl_device_info(item.id), &info_str);
      if (!first)
      {
        out << endl;
      }
      // Use c_str() otherwise we get extra '\0' characters.
      out << prefix << item.label << ": " << info_str;
      first = false;
    }
  }
}
}  // namespace clu
