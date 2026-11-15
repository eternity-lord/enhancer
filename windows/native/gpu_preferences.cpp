// Ask hybrid-GPU drivers to expose the high-performance adapter to the process.  This is a
// preference, not a hard requirement: Vulkan enumeration still validates the driver/device and
// the native engine keeps its CPU fallback for unsupported or unstable adapters.
#ifdef _WIN32
extern "C" __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001UL;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif
