#include <IOKit/IOKitLib.h>

#include <cassert>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

//
// @Note(impcuong): Abbrev-list
//  + CF := Core Foundation
//  + hw := hardware
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
  std::optional<int> vram;

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

const char *_PCI_DEV = "IOPCIDevice";

const UInt8 *hw_get_qualified_byte_ptr(io_service_t &entry, CFTypeRef &ref,
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
      return raw_bytes;
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
      return raw_bytes;
    }
  }

  raw_bytes = CFDataGetBytePtr(ref_data);
  return raw_bytes;
}

int GPU_QUAN = 0;

std::vector<gpu_spec> hw_retrieve_gpu_specs()
{
  std::vector<gpu_spec> specs;

  CFMutableDictionaryRef matching_dyn_dict = IOServiceMatching(_PCI_DEV /*name=*/);
  io_iterator_t io_iter;
  kern_return_t kern_rc = IOServiceGetMatchingServices(kIOMainPortDefault /*mainPort=*/,
      matching_dyn_dict /*matching=*/, &io_iter /*existing=*/);
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
      auto *raw_bytes = hw_get_qualified_byte_ptr(io_svc_entry, class_code_ref, 4 /*min_sz=*/);
      UInt32 class_code = (raw_bytes[3] << 24) | (raw_bytes[2] << 16) |
        (raw_bytes[1] << 8) | raw_bytes[0];

      // 0x030000 := display controller's preserved address
      // class_code & 0xFF0000 := extracts 8 high-bits
      if ((class_code & 0xFF0000) == 0x030000)
      {
        GPU_QUAN++;
        gpu_spec spec;
        memset(&spec, 0, sizeof(spec));

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
            auto *raw_bytes = hw_get_qualified_byte_ptr(io_svc_entry, model_ref, -1, true /*need_type_check=*/);
            spec.model = static_cast<std::string>((const char *)raw_bytes);

            CFRelease(model_ref);
          }
        }

        { // Vendor ID
          CFTypeRef vendor_id_ref = IORegistryEntryCreateCFProperty(io_svc_entry, CFSTR("vendor-id"),
              kCFAllocatorDefault, 0);
          if (vendor_id_ref)
          {
            auto *raw_bytes = hw_get_qualified_byte_ptr(io_svc_entry, vendor_id_ref, 2 /*min_sz=*/);
            UInt32 vendor_id = raw_bytes[0] | (raw_bytes[1] << 8);
            switch (vendor_id)
            {
              case 0x1002: spec.vendor = "AMD"; break;
              case 0x10de: spec.vendor = "Nvidia"; break;
              case 0x8086: spec.vendor = "Intel"; break;
              case 0x106b: spec.vendor = "Apple"; break;
            }

            CFRelease(vendor_id_ref);
          }
        }

        { // Device ID
          CFTypeRef dev_id_ref = IORegistryEntryCreateCFProperty(io_svc_entry, CFSTR("device-id"),
              kCFAllocatorDefault, 0);
          if (dev_id_ref)
          {
            auto *raw_bytes = hw_get_qualified_byte_ptr(io_svc_entry, dev_id_ref, 2 /*min_sz=*/);
            UInt32 device_id = raw_bytes[0] | (raw_bytes[1] << 8);
            std::stringstream ss;
            ss << std::hex << device_id;
            spec.dev_id = ss.str();

            CFRelease(dev_id_ref);
          }
        }

        { // VRAM Size
        }

        specs.resize(GPU_QUAN);
        specs.emplace_back(spec);
      }
    }
    CFRelease(class_code_ref);
    IOObjectRelease(io_svc_entry /*object=*/);
  }
  IOObjectRelease(io_iter /*object=*/);

  return specs;
}

int main()
{
  auto gpu_specs = hw_retrieve_gpu_specs();
  println("INFO: GPU Quantity =", GPU_QUAN);
  std::ranges::for_each(gpu_specs, [](const auto &spec) { std::cout << spec << "\n"; });
}
