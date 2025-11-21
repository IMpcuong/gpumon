#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>

#include <cassert>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

//
// @Note(impcuong): Abbrev-list
//  + CF  := Core Foundation
//  + hw  := hardware
//  + dev := device
//  + txt := texture
//

template <typename... Args>
void println(const Args &...args)
{
  bool end = true;
  ((std::cout << (end ? (end = false, "") : " ") << args), ...) << "\n";
}

struct gpu_spec
{
  std::string name;
  std::optional<std::string> model;
  std::optional<std::string> vendor;
  std::optional<std::string> dev_id;
  std::optional<UInt64> vram;

  friend std::ostream &operator<<(std::ostream &out, const gpu_spec &spec)
  {
    out << "Reporter:\n";
    out << "  + Name: " << spec.name << "\n";
    if (spec.model.has_value())
      out << "  + Model: " << spec.model.value() << "\n";
    if (spec.vendor.has_value())
      out << "  + Vendor: " << spec.vendor.value() << "\n";
    if (spec.dev_id.has_value())
      out << "  + Device ID: " << spec.dev_id.value() << "\n";
    if (spec.vram.has_value())
      out << "  + VRAM Size: " << spec.vram.value() << "\n";
    return out;
  }
};

std::pair<const UInt8 *, bool>
hw_get_qualified_byte_ptr(io_service_t &entry, CFTypeRef &ref,
    const int &min_sz, const bool &need_type_check = false)
{
  const UInt8 *raw_bytes;
  CFDataRef ref_data;
  if (need_type_check)
  {
    // typedef unsigned long CFTypeID;
    CFTypeID ref_id = CFGetTypeID(ref);
    if (ref_id == CFDataGetTypeID())
    {
      ref_data = static_cast<CFDataRef>(ref);
    }
    else
    {
      CFRelease(ref);
      IOObjectRelease(entry);
      return {raw_bytes, false};
    }
  }

  if (min_sz > -1)
  {
    if (!need_type_check)
      ref_data = static_cast<CFDataRef>(ref);

    // typedef long CFIndex;
    CFIndex ref_data_sz = CFDataGetLength(ref_data);
    if (ref_data_sz < min_sz)
    {
      CFRelease(ref_data);
      IOObjectRelease(entry);
      return {raw_bytes, false};
    }
  }

  raw_bytes = CFDataGetBytePtr(ref_data);
  return {raw_bytes, true};
}

// #include <AvailabilityMacros.h> := older API
// #include <Availability.h>       := newer API
#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_VERSION_12_0
  #define IO_MAIN_PORT kIOMainPortDefault
#else
  #define IO_MAIN_PORT kIOMasterPortDefault
#endif

const char *_PCI_DEV = "IOPCIDevice";

int GPU_QUAN = 0;

const std::vector<gpu_spec> hw_collect_gpu_specs()
{
  std::vector<gpu_spec> specs;

  CFMutableDictionaryRef matching_dyn_dict_ref = IOServiceMatching(_PCI_DEV /*name=*/);
  io_iterator_t io_iter;
  kern_return_t kern_rc = IOServiceGetMatchingServices(IO_MAIN_PORT /*mainPort=*/,
      matching_dyn_dict_ref /*matching=*/, &io_iter /*existing=*/);
  println("INFO: Kernel's return-code:", kern_rc, KERN_SUCCESS);
  if (kern_rc != KERN_SUCCESS)
    return specs;

  // typedef io_object_t io_service_t;
  io_service_t io_svc_entry; // uint32_t
  while ((io_svc_entry = IOIteratorNext(io_iter)))
  {
    // typedef const void * CFTypeRef;
    CFTypeRef class_code_ref = IORegistryEntryCreateCFProperty(io_svc_entry /*entry=*/, CFSTR("class-code") /*key=*/,
        kCFAllocatorDefault /*allocator=*/, 0 /*options=*/);
    if (class_code_ref != NULL)
    {
      auto data_state = hw_get_qualified_byte_ptr(io_svc_entry, class_code_ref, 4 /*min_sz=*/);
      const UInt8 *raw_bytes = data_state.first;
      bool allocated = data_state.second;
      if (!allocated)
      {
        CFRelease(class_code_ref);
        IOObjectRelease(io_svc_entry);
        continue;
      }

      UInt32 class_code = (raw_bytes[3] << 24) | (raw_bytes[2] << 16) |
        (raw_bytes[1] << 8) | raw_bytes[0];

      // 0x010000 := mass storage controller's preserved address
      // 0x020000 := network controller's preserved address
      // 0x0C0000 := serial bus (USB, FireWire, etc) controller's preserved address
      // 0x030000 := display controller's preserved address
      // class_code & 0xFF0000 := extracts 8 high-bits
      if ((class_code & 0xFF0000) == 0x030000)
      {
        GPU_QUAN++;
        gpu_spec spec;
        memset(&spec, 0, sizeof(gpu_spec));

        { // Name
          io_name_t io_name;
          IORegistryEntryGetName(io_svc_entry, io_name);
          spec.name = io_name;
        }

        { // Model
          CFTypeRef model_ref = IORegistryEntryCreateCFProperty(io_svc_entry, CFSTR("model"),
              kCFAllocatorDefault, 0);
          if (model_ref)
          {
            auto data_state = hw_get_qualified_byte_ptr(io_svc_entry, model_ref, -1, true /*need_type_check=*/);
            const UInt8 *raw_bytes = data_state.first;
            bool allocated = data_state.second;
            if (allocated)
              spec.model = static_cast<std::string>((const char *)raw_bytes);

            CFRelease(model_ref);
          }
        }

        { // Vendor ID
          CFTypeRef vendor_id_ref = IORegistryEntryCreateCFProperty(io_svc_entry, CFSTR("vendor-id"),
              kCFAllocatorDefault, 0);
          if (vendor_id_ref)
          {
            auto data_state = hw_get_qualified_byte_ptr(io_svc_entry, vendor_id_ref, 2 /*min_sz=*/);
            const UInt8 *raw_bytes = data_state.first;
            bool allocated = data_state.second;
            if (allocated)
            {
              UInt32 vendor_id = raw_bytes[0] | (raw_bytes[1] << 8);
              switch (vendor_id)
              {
                case 0x1002: spec.vendor = "AMD"; break;
                case 0x10de: spec.vendor = "Nvidia"; break;
                case 0x8086: spec.vendor = "Intel"; break;
                case 0x106b: spec.vendor = "Apple"; break;
              }
            }

            CFRelease(vendor_id_ref);
          }
        }

        { // Device ID
          CFTypeRef dev_id_ref = IORegistryEntryCreateCFProperty(io_svc_entry, CFSTR("device-id"),
              kCFAllocatorDefault, 0);
          if (dev_id_ref)
          {
            auto data_state = hw_get_qualified_byte_ptr(io_svc_entry, dev_id_ref, 2 /*min_sz=*/);
            const UInt8 *raw_bytes = data_state.first;
            bool allocated = data_state.second;
            if (allocated)
            {
              UInt32 device_id = raw_bytes[0] | (raw_bytes[1] << 8);
              std::stringstream ss;
              ss << std::hex << device_id;
              spec.dev_id = ss.str();
            }

            CFRelease(dev_id_ref);
          }
        }

        { // VRAM Size
          CFTypeRef vram_ref = IORegistryEntryCreateCFProperty(io_svc_entry, CFSTR("VRAM,totalsize"),
              kCFAllocatorDefault, 0);
          if (vram_ref)
          {
            auto data_state = hw_get_qualified_byte_ptr(io_svc_entry, vram_ref,
                2 /*min_sz=*/, true /*need_type_check=*/);
            const UInt8 *raw_bytes = data_state.first;
            bool allocated = data_state.second;
            if (allocated)
            {
              UInt64 vram_sz = 0;
              const int max_seg_quan = 8;
              const int cur_seg_quan = static_cast<int>(CFDataGetLength((CFDataRef)vram_ref));
              for (int i = 0; i < cur_seg_quan && i < max_seg_quan; i++)
                vram_sz |= static_cast<UInt64>(raw_bytes[i]) << (i * 8);

              spec.vram = vram_sz / (1024 * 1024);
            }

            CFRelease(vram_ref);
          }
        }

        specs.emplace_back(spec);
      }
    }
    CFRelease(class_code_ref);
    IOObjectRelease(io_svc_entry /*object=*/);
  }
  IOObjectRelease(io_iter /*object=*/);

  return specs;
}

using ll = long long;

struct gpu_stat
{
  std::optional<long> vram;
  std::optional<int> txt_quan;
  std::optional<int> dev_usage;
  std::optional<double> core_usage;
  std::optional<ll> vram_free;

  friend std::ostream &operator<<(std::ostream &out, const gpu_stat &stat)
  {
    out << "Reporter:\n";
    if (stat.txt_quan.has_value())
      out << "  + Textures: " << stat.txt_quan.value() << "\n";
    if (stat.vram.has_value())
      out << "  + VRAM (MB): " << stat.vram.value() << "\n";
    if (stat.vram_free.has_value())
      out << "  + VRAM Free (B): " << stat.vram_free.value() << "\n";
    if (stat.dev_usage.has_value())
      out << "  + Device usage (%): " << stat.dev_usage.value() << "\n";
    if (stat.core_usage.has_value())
      out << "  + Core usage (%): " << stat.core_usage.value() << "\n";
    return out;
  }
};

const char *_IO_ACCEL = "IOAccelerator";

const std::vector<gpu_stat> hw_collect_gpu_stats()
{
  std::vector<gpu_stat> stats;

  CFMutableDictionaryRef io_accel_ref = IOServiceMatching(_IO_ACCEL /*name=*/);
  io_iterator_t io_iter;
  kern_return_t kern_rc = IOServiceGetMatchingServices(IO_MAIN_PORT /*mainPort=*/,
      io_accel_ref /*matching=*/, &io_iter /*existing=*/);
  println("INFO: Kernel's return-code:", kern_rc, KERN_SUCCESS);
  if (kern_rc != KERN_SUCCESS)
    return stats;

  // typedef io_object_t io_service_t;
  io_service_t io_svc_entry; // uint32_t
  while ((io_svc_entry = IOIteratorNext(io_iter)))
  {
    CFMutableDictionaryRef io_accel_prop_ref = nullptr;
    // @Note: `ioreg -l -w 0 | grep -i "ioaccelerator"
    kern_rc = IORegistryEntryCreateCFProperties(io_svc_entry /*entry=*/, &io_accel_prop_ref /*properties=*/,
        kCFAllocatorDefault /*allocator=*/, 0 /*options=*/);
    println("INFO: Kernel's return-code:", kern_rc, KERN_SUCCESS);
    if (kern_rc != KERN_SUCCESS)
      return stats;

    if (io_accel_prop_ref != nullptr)
    {
      gpu_stat stat;
      memset(&stat, 0, sizeof(gpu_stat));

      const void *raw_vram_sz = CFDictionaryGetValue(io_accel_prop_ref, CFSTR("VRAM,totalMB"));
      if (raw_vram_sz != nullptr)
      {
        auto gpu_vram_sz = static_cast<CFNumberRef>(raw_vram_sz);
        long vram = 0;
        CFNumberGetValue(gpu_vram_sz, kCFNumberIntType, &vram);
        stat.vram.emplace(vram);
      }

      // @Note: `ioreg -l -w 0 | grep -i "performance" -A 30`
      const void *raw_perf_stat = CFDictionaryGetValue(io_accel_prop_ref /*theDict=*/, CFSTR("PerformanceStatistics") /*key=*/);
      auto gpu_perf_stat = static_cast<CFDictionaryRef>(raw_perf_stat);
      if (gpu_perf_stat)
      {
        int usage = 0;

        const void *raw_txt_quan = CFDictionaryGetValue(gpu_perf_stat, CFSTR("textureCount"));
        if (raw_txt_quan != nullptr)
        {
          auto gpu_util_percentage = static_cast<CFNumberRef>(raw_txt_quan);
          CFNumberGetValue(gpu_util_percentage, kCFNumberIntType, &usage);
          stat.txt_quan.emplace(usage);
        }

        const void *raw_util_percentage = CFDictionaryGetValue(gpu_perf_stat, CFSTR("Device Utilization % at cur p-state"));
        if (raw_util_percentage != nullptr)
        {
          auto gpu_util_percentage = static_cast<CFNumberRef>(raw_util_percentage);
          CFNumberGetValue(gpu_util_percentage, kCFNumberIntType, &usage);
          stat.dev_usage.emplace(usage);
        }

        const void *raw_core_percentage= CFDictionaryGetValue(gpu_perf_stat, CFSTR("GPU Core Utilization"));
        if (raw_core_percentage != nullptr)
        {
          auto gpu_core_percentage = static_cast<CFNumberRef>(raw_core_percentage);
          CFNumberGetValue(gpu_core_percentage, kCFNumberDoubleType, &usage);
          stat.core_usage.emplace(usage);
        }
      }

      // @Fixme: Wrong implementation...
      const void *raw_entries_stat = CFDictionaryGetValue(io_accel_prop_ref /*theDict=*/, CFSTR("IOCompatibilityProperties") /*key=*/);
      auto gpu_entries_stat = static_cast<CFDictionaryRef>(raw_perf_stat);
      if (gpu_entries_stat)
      {
        const void *raw_vram_free = CFDictionaryGetValue(gpu_perf_stat, CFSTR("PerformanceStatistics.vramFreeBytes"));
        if (raw_vram_free != nullptr)
        {
          auto gpu_vram_free = static_cast<CFNumberRef>(raw_vram_free);
          long vram_free = 0;
          CFNumberGetValue(gpu_vram_free, kCFNumberIntType, &vram_free);
          stat.vram_free.emplace(vram_free);
        }
      }
      // End @Fixme

      stats.emplace_back(stat);

      CFRelease(io_accel_prop_ref);
    }
    IOObjectRelease(io_svc_entry /*object=*/);
  }
  IOObjectRelease(io_iter /*object=*/);

  return stats;
}

struct cpu_spec
{
  std::string name;
  std::string arch;
  int family;
  int physical_cores = 0;
  int logical_cores = 0;
  // std::array<int, 4> caches; // hw.l1dcachesize, hw.l1icachesize, hw.l2cachesize, hw.l3cachesize

  friend std::ostream &operator<<(std::ostream &out, const cpu_spec &spec)
  {
    out << "Reporter:\n";
    out << "  + Name: " << spec.name << "\n";
    out << "  + Architecture: " << spec.arch << "\n";
    out << "  + Family: " << spec.family << "\n";
    out << "  + Physical cores: " << spec.physical_cores << "\n";
    out << "  + Logical cores: " << spec.logical_cores << "\n";
    return out;
  }
};

const cpu_spec * hw_collect_cpu_spec()
{
  cpu_spec *spec = new cpu_spec();

  const char *name_query = "machdep.cpu.brand_string";
  size_t len = 0;
  sysctlbyname(name_query, NULL, &len, NULL, 0);
  spec->name.resize(len);
  sysctlbyname(name_query, spec->name.data(), &len, NULL, 0);

  const char *arch_query = "hw.machine";
  sysctlbyname(arch_query, NULL, &len, NULL, 0);
  spec->arch.resize(len);
  sysctlbyname(arch_query, spec->arch.data(), &len, NULL, 0);

  const char *family_query = "hw.cpufamily";
  len = sizeof(spec->family);
  sysctlbyname(family_query, &spec->family, &len, NULL, 0);

  const char *pcore_query = "hw.physicalcpu";
  len = sizeof(spec->physical_cores);
  sysctlbyname(pcore_query, &spec->physical_cores, &len, NULL, 0);

  const char *lcore_query = "hw.logicalcpu";
  len = sizeof(spec->logical_cores);
  sysctlbyname(lcore_query, &spec->logical_cores, &len, NULL, 0);

  return spec;
}

int main()
{
  println("INFO: CPU");
  auto cpu_spec = *hw_collect_cpu_spec();
  std::cout << cpu_spec << "\n";

  println("INFO: GPU Quantity =", GPU_QUAN);
  auto gpu_specs = hw_collect_gpu_specs();
  std::ranges::for_each(gpu_specs, [](const auto &spec) { std::cout << spec << "\n"; });

  println("INFO: GPU Stat");
  auto gpu_stats = hw_collect_gpu_stats();
  std::ranges::for_each(gpu_stats, [](const auto &stat) { std::cout << stat << "\n"; });
}
