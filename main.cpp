#include <IOKit/IOKitLib.h>

#include <cassert>
#include <iostream>

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

const char *_PCI_DEV = "IOPCIDevice";

std::pair<bool, int> hw_carry_gpu()
{
  int gpu_quan = 0;
  bool found = false;

  CFMutableDictionaryRef matching_dyn_dict = IOServiceMatching(_PCI_DEV /*name=*/);
  io_iterator_t io_iter;
  kern_return_t kern_rc = IOServiceGetMatchingServices(kIOMainPortDefault /*mainPort=*/,
      matching_dyn_dict /*matching=*/, &io_iter /*existing=*/);
  println("INFO: Kernel's return-code:", kern_rc, KERN_SUCCESS);
  if (kern_rc != KERN_SUCCESS)
    return {found, gpu_quan};

  // typedef io_object_t io_service_t;
  io_service_t io_svc; // uint32_t
  while ((io_svc = IOIteratorNext(io_iter)))
  {
    // typedef const void * CFTypeRef;
    CFTypeRef class_code_ref = IORegistryEntryCreateCFProperty(io_svc /*entry=*/, CFSTR("class-code") /*key=*/,
        kCFAllocatorDefault /*allocator=*/, 0 /*options=*/);
    if (class_code_ref != NULL)
    {
      auto class_code_data = static_cast<CFDataRef>(class_code_ref);
      // typedef long CFIndex;
      CFIndex class_code_sz = CFDataGetLength(class_code_data);
      if (class_code_sz < 4)
      {
        CFRelease(class_code_ref);
        IOObjectRelease(io_svc /*object=*/);
        continue;
      }

      const UInt8 *raw_bytes = CFDataGetBytePtr(class_code_data);
      assert(sizeof(raw_bytes) >= 4);
      UInt32 class_code = (raw_bytes[3] << 24) | (raw_bytes[2] << 16) |
        (raw_bytes[1] << 8) | raw_bytes[0];

      // 0x030000 := display controller's preserved address
      // class_code & 0xFF0000 := extracts 8 high-bits
      if ((class_code & 0xFF0000) == 0x030000)
      {
        found = true;
        gpu_quan++;
      }
    }
    CFRelease(class_code_ref);
    IOObjectRelease(io_svc /*object=*/);
  }
  IOObjectRelease(io_iter /*object=*/);

  return {found, gpu_quan};
}

int main()
{
  const char *_Y = "YES";
  const char *_N = "NO";
  auto ans = hw_carry_gpu();
  println("INFO: { Existing:", ans.first ? _Y : _N, ", GPU Quan:", ans.second, "}");
}
