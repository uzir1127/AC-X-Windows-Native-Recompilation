#ifndef PSPRECO_HARDWARE_STATS_H
#define PSPRECO_HARDWARE_STATS_H

#include <stdint.h>
#include <wchar.h>

struct HardwareStats {
    char cpu_name[128];
    char gpu_name[256];
    double cpu_percent, gpu_percent;
    bool cpu_valid, gpu_valid;
    uint64_t sampled_at;
};

struct HardwareGpuSample {
    const wchar_t *instance;
    double percent;
    bool valid;
};

/* Sum processes on each engine of the selected adapter, then use its
 * busiest engine. Never mix adapters or sum independent engine percentages. */
bool hardware_gpu_usage(const HardwareGpuSample *samples, unsigned count,
                        uint32_t luid_high, uint32_t luid_low, double *out);
void hardware_stats_init(const char *gpu_name, const uint8_t *luid, bool valid,
                         const char *nvidia_pci_bus);
void hardware_stats_set_visible(bool visible);
HardwareStats hardware_stats_read(void);
void hardware_stats_shutdown(void);

#endif
