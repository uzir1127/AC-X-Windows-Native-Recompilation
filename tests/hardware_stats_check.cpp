#include "../runtime/host/hardware_stats.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <SDL3/SDL.h>

int main(int argc, char **argv)
{
    const HardwareGpuSample samples[] = {
        {L"pid_1_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",20,true},
        {L"pid_2_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",30,true},
        {L"pid_1_luid_0x00000000_0x00001234_phys_0_eng_1_engtype_Copy",60,true},
        {L"pid_3_luid_0x00000000_0x00005678_phys_0_eng_0_engtype_3D",99,true},
        {L"pid_4_luid_0x00000000_0x00001234_phys_0_eng_2_engtype_3D",95,false},
        {L"pid_5_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",NAN,true},
        {L"pid_6_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",-20,true},
        {L"bad_instance",100,true}
    };
    int bad=0; double v=-1;
    if (!hardware_gpu_usage(samples,8,0,0x1234,&v) || v!=60) ++bad;
    if (!hardware_gpu_usage(samples,8,0,0x5678,&v) || v!=99) ++bad;
    if (hardware_gpu_usage(samples,8,1,0x1234,&v)) ++bad;
    if (hardware_gpu_usage(samples,0,0,0x1234,&v)) ++bad;
    const HardwareGpuSample capped[] = {
        {samples[0].instance,70,true},{samples[1].instance,60,true}
    };
    if (!hardware_gpu_usage(capped,2,0,0x1234,&v) || v!=100) ++bad;
    const HardwareGpuSample zero[] = {{samples[0].instance,0,true}};
    if (!hardware_gpu_usage(zero,1,0,0x1234,&v) || v!=0) ++bad;
    const HardwareGpuSample physical[] = {
        {samples[0].instance,40,true},
        {L"pid_1_luid_0x00000000_0x00001234_phys_1_eng_0_engtype_3D",50,true}
    };
    if (!hardware_gpu_usage(physical,2,0,0x1234,&v) || v!=50) ++bad;
    printf("GPU aggregation: %s (adapter isolation, process sums, busiest engine, invalid data, zero load, cap, physical nodes)\n",bad?"FAIL":"PASS");
    if (argc == 3 && !strcmp(argv[1], "--live-pci")) {
        SDL_Init(0);
        hardware_stats_init("Selected NVIDIA GPU", nullptr, false, argv[2]);
        hardware_stats_set_visible(true);
        HardwareStats h = {};
        for (int i=0;i<60;++i) {
            SDL_Delay(100); h=hardware_stats_read();
            if (h.cpu_valid && h.gpu_valid) break;
        }
        printf("Live CPU %s: %.1f%% (%s); GPU %.1f%% (%s)\n",h.cpu_name,h.cpu_percent,h.cpu_valid?"valid":"unavailable",h.gpu_percent,h.gpu_valid?"valid":"unavailable");
        if (!h.cpu_valid || !h.gpu_valid) ++bad;
        hardware_stats_set_visible(false);
        SDL_Delay(1200);uint64_t stopped=hardware_stats_read().sampled_at;
        SDL_Delay(1200);if(hardware_stats_read().sampled_at!=stopped)++bad;
        hardware_stats_set_visible(true);
        SDL_Delay(2200);h=hardware_stats_read();
        if(h.sampled_at<=stopped||!h.cpu_valid||!h.gpu_valid)++bad;
        hardware_stats_shutdown();SDL_Quit();
        printf("Live sampling and hide/resume: %s\n",bad?"FAIL":"PASS");
    }
    return bad?1:0;
}
