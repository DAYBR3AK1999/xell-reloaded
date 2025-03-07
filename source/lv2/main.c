#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <console/console.h>  // Fix for CONSOLE_COLOR_CYAN
#include <xenon_smc/xenon_smc.h> // Fix for xenon_smc_read_temp

#include <debug.h>
#include <xenos/xenos.h>
#include <console/console.h>
#include <time/time.h>
#include <ppc/timebase.h>
#include <usb/usbmain.h>
#include <sys/iosupport.h>
#include <ppc/register.h>
#include <xenon_nand/xenon_sfcx.h>
#include <xenon_nand/xenon_config.h>
#include <xenon_soc/xenon_secotp.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_soc/xenon_io.h>
#include <xenon_sound/sound.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_smc/xenon_gpio.h>
#include <xb360/xb360.h>
#include <network/network.h>
#include <httpd/httpd.h>
#include <diskio/ata.h>
#include <elf/elf.h>
#include <version.h>
#include <byteswap.h>

#include "asciiart.h"
#include "config.h"
#include "file.h"
#include "tftp/tftp.h"

#include "log.h"

void do_asciiart() {
    char *p = asciiart;
    while (*p)
        console_putch(*p++);
    printf(asciitail);
}

void dumpana() {
    int i;
    for (i = 0; i < 0x100; ++i) {
        uint32_t v;
        xenon_smc_ana_read(i, &v);
        printf("0x%08x, ", (unsigned int)v);
        if ((i & 0x7) == 0x7)
            printf(" // %02x\n", (unsigned int)(i & ~0x7));
    }
}

void print_temperatures() {
    uint8_t cpu_temp, gpu_temp, edram_temp;
    
    xenon_smc_read_temp(0, &cpu_temp);
    xenon_smc_read_temp(1, &gpu_temp);
    xenon_smc_read_temp(2, &edram_temp);
    
    printf("\n====================\n");
    printf(" * Console Temperatures:\n");
    printf("   - CPU:   %d°C\n", cpu_temp);
    printf("   - GPU:   %d°C\n", gpu_temp);
    printf("   - EDRAM: %d°C\n", edram_temp);
    printf("====================\n");
}

void print_uptime() {
    uint64_t tb = mftb(); 
    uint64_t seconds = tb / 50000000; 

    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds % 3600) / 60;
    uint32_t sec = seconds % 60;

    printf("\n====================\n");
    printf(" * System Uptime: %02d:%02d:%02d\n", hours, minutes, sec);
    printf("====================\n");
}

// Store fuse data
char FUSES[350];
char CBLDV[17]; 
char FGLDV[80];
int cbldvcount;
int fgldvcount;

unsigned char stacks[6][0x10000];

void reset_timebase_task() {
    mtspr(284, 0); 
    mtspr(285, 0); 
    mtspr(284, 0);
}

void synchronize_timebases() {
    xenon_thread_startup();
    
    std((void*)0x200611a0, 0);

    int i;
    for (i = 1; i < 6; ++i) {
        xenon_run_thread_task(i, &stacks[i][0xff00], (void *)reset_timebase_task);
        while (xenon_is_thread_task_running(i));
    }

    reset_timebase_task(); 
    std((void*)0x200611a0, 0x1ff);
}

int main() {
    LogInit();
    int i;

    printf("\n====================\n");
    printf("  ANA Dump Before Init\n");
    printf("====================\n");
    dumpana();

    synchronize_timebases();

    *(volatile uint32_t*)0xea00106c = 0x1000000;
    *(volatile uint32_t*)0xea001064 = 0x10;
    *(volatile uint32_t*)0xea00105c = 0xc000000;

    xenon_smc_start_bootanim();

    setbuf(stdout, NULL);

    xenos_init(VIDEO_MODE_AUTO);

    printf("\n====================\n");
    printf("  ANA Dump After Init\n");
    printf("====================\n");
    dumpana();

#ifdef HEXAMODS_THEME
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_BLUE); // CYAN not available, using GREY
#elif defined SWIZZY_THEME
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE); 
#elif defined XTUDO_THEME
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_PINK);
#elif defined DEFAULT_THEME
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_BLUE); 
#else
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GREEN);
#endif

    console_init();

    printf("\n=====================================\n");
    printf("        XeLL RELOADED - HexaMods     \n");
    printf("=====================================\n\n");

    do_asciiart();

    xenon_sound_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    if (xenon_get_console_type() != REV_CORONA_PHISON) {
        printf(" * NAND Init\n");
        sfcx_init();
        if (sfc.initialized != SFCX_INITIALIZED) {
            printf(" ! SFCX initialization failure\n");
            printf(" ! NAND features unavailable\n");
            delay(5);
        }
    }

    xenon_config_init();

#ifndef NO_NETWORKING
    printf(" * Network Init\n");
    network_init();
    printf(" * Starting HTTP Server... Success\n");
    httpd_start();
#endif

    printf(" * USB Init\n");
    usb_init();
    usb_do_poll();

    printf(" * SATA HDD Init\n");
    xenon_ata_init();

#ifndef NO_DVD
    printf(" * SATA DVD Init\n");
    xenon_atapi_init();
#endif

    mount_all_devices();
    findDevices();

    console_clrscr();

    network_print_config(); // ✅ Always display IP address
    print_temperatures();
    print_uptime();

#ifndef NO_PRINT_CONFIG
    printf("\n====================\n");
    printf(" * FUSES - Save this info safely:\n");
    printf("====================\n");

    char *fusestr = FUSES;
    char *cbldvstr = CBLDV;
    char *fgldvstr = FGLDV;

    for (i = 0; i < 12; ++i) {
        u64 line;
        unsigned int hi, lo;
        
        line = xenon_secotp_read_line(i);
        hi = line >> 32;
        lo = line & 0xffffffff;

        fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
    }

    printf(FUSES);

    print_cpu_dvd_keys();

    printf(" * CPU PVR: %08x\n", mfspr(287));

    switch (xenon_get_console_type()) {
        case 0: printf(" * Console: Xenon\n"); break;
        case 1: printf(" * Console: Xenon/Zephyr\n"); break;
        case 2: printf(" * Console: Falcon\n"); break;
        case 3: printf(" * Console: Jasper\n"); break;
        case 4: printf(" * Console: Trinity\n"); break;
        case 5: printf(" * Console: Corona\n"); break;
        case 6: printf(" * Console: Corona MMC\n"); break;
        case 7: printf(" * Console: Winchester (Impossible)\n"); break;
        default: printf(" * Console: Unknown\n"); break;
    }

    printf(" * 2BL LDV: %d\n", cbldvcount);
    printf(" * 6BL LDV: %d\n", fgldvcount);
#endif

    LogDeInit();

    printf("\n * Searching for files on local storage and TFTP...\n\n");

    for (;;) {
        fileloop();
        tftp_loop();
        console_clrline();
        usb_do_poll();
    }

    return 0;
}
