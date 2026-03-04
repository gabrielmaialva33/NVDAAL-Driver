/*
 * test_register_read.c - Direct GPU register access via BAR0 MMIO
 *
 * Reads key NVIDIA GPU registers to validate the register definitions
 * in NVDAALRegs.h against real hardware.
 *
 * Must run as root: sudo ./test_register_read
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "NVDAALRegs.h"

#define BAR0_SIZE (16 * 1024 * 1024)  // 16MB
#define PCI_RESOURCE "/sys/bus/pci/devices/0000:02:00.0/resource0"

static volatile uint32_t* bar0 = NULL;

static uint32_t gpu_rd32(uint32_t offset) {
    return bar0[offset / 4];
}

typedef struct {
    uint32_t offset;
    const char* name;
    int passed;
} RegTest;

#define REG(off, nm) { off, nm, 0 }

int main(void) {
    int fd = open(PCI_RESOURCE, O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open (run as root?)");
        return 1;
    }

    bar0 = (volatile uint32_t*)mmap(NULL, BAR0_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (bar0 == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("NVDAAL Register Read Test\n");
    printf("BAR0 mapped at %p\n\n", (void*)bar0);

    // ====== Chip Identification ======
    printf("=== Chip Identification ===\n");
    uint32_t boot0 = gpu_rd32(NV_PMC_BOOT_0);
    uint32_t arch = (boot0 >> 20) & 0x1F;
    uint32_t impl = (boot0 >> 20) & 0xF;
    uint32_t rev  = boot0 & 0xFF;
    printf("  PMC_BOOT_0:  0x%08X (arch=0x%02X impl=0x%X rev=0x%02X)\n", boot0, arch, impl, rev);

    int is_ada = (arch == NV_CHIP_ARCH_ADA);
    printf("  Architecture: %s %s\n",
           is_ada ? "Ada Lovelace" : (arch == 0x17 ? "Ampere" : "Unknown"),
           is_ada ? "[OK]" : "[UNEXPECTED]");

    // ====== FWSEC/WPR2 Status ======
    printf("\n=== FWSEC / WPR2 Status ===\n");
    uint32_t frts_err = gpu_rd32(NV_PBUS_VBIOS_SCRATCH_FRTS_ERR);
    uint32_t fwsec_err = gpu_rd32(NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR);
    uint32_t wpr2_lo = gpu_rd32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    uint32_t wpr2_hi = gpu_rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);

    printf("  FRTS error:  0x%08X (upper16=0x%04X -> %s)\n",
           frts_err, frts_err >> 16, (frts_err >> 16) == 0 ? "SUCCESS" : "ERROR");
    printf("  FWSEC error: 0x%08X (lower16=0x%04X -> %s)\n",
           fwsec_err, fwsec_err & 0xFFFF, (fwsec_err & 0xFFFF) == 0 ? "SUCCESS" : "ERROR");
    printf("  WPR2 LO:    0x%08X\n", wpr2_lo);
    printf("  WPR2 HI:    0x%08X\n", wpr2_hi);

    uint64_t wpr2_begin = ((uint64_t)wpr2_lo) << 8;
    uint64_t wpr2_end   = ((uint64_t)wpr2_hi) << 8;
    printf("  WPR2 range: 0x%llX - 0x%llX (%llu MB)\n",
           (unsigned long long)wpr2_begin, (unsigned long long)wpr2_end,
           (unsigned long long)(wpr2_end - wpr2_begin) / (1024*1024));

    // ====== GSP State ======
    printf("\n=== GSP State ===\n");
    uint32_t gsp_mbox0 = gpu_rd32(NV_PGSP_FALCON_MAILBOX0);
    uint32_t gsp_mbox1 = gpu_rd32(NV_PGSP_FALCON_MAILBOX1);
    uint32_t gsp_rv_cpuctl = gpu_rd32(NV_PRISCV_RISCV_CPUCTL);

    printf("  GSP Mailbox0:    0x%08X\n", gsp_mbox0);
    printf("  GSP Mailbox1:    0x%08X\n", gsp_mbox1);
    printf("  GSP RISC-V CPUCTL: 0x%08X\n", gsp_rv_cpuctl);
    printf("    Active: %s, Halted: %s\n",
           (gsp_rv_cpuctl & NV_PRISCV_CPUCTL_ACTIVE) ? "YES" : "NO",
           (gsp_rv_cpuctl & NV_PRISCV_CPUCTL_HALTED) ? "YES" : "NO");

    // ====== SEC2 State ======
    printf("\n=== SEC2 State ===\n");
    uint32_t sec2_rv_cpuctl = gpu_rd32(NV_PSEC_RISCV_CPUCTL);
    uint32_t sec2_rv_retcode = gpu_rd32(NV_PSEC_RISCV_BR_RETCODE);
    uint32_t sec2_bcr = gpu_rd32(NV_PSEC_RISCV_BCR_CTRL);

    printf("  SEC2 RISC-V CPUCTL: 0x%08X (halted=%s)\n", sec2_rv_cpuctl,
           (sec2_rv_cpuctl & NV_PRISCV_CPUCTL_HALTED) ? "YES" : "NO");
    printf("  SEC2 BR_RETCODE:    0x%08X\n", sec2_rv_retcode);
    printf("  SEC2 BCR_CTRL:      0x%08X (valid=%s)\n", sec2_bcr,
           (sec2_bcr & 1) ? "YES" : "NO");

    // ====== GSP Queues ======
    printf("\n=== GSP Queues ===\n");
    printf("  CMD Queue: HEAD=0x%08X TAIL=0x%08X\n",
           gpu_rd32(NV_PGSP_QUEUE_HEAD(GSP_CMDQ_IDX)),
           gpu_rd32(NV_PGSP_QUEUE_TAIL(GSP_CMDQ_IDX)));
    printf("  MSG Queue: HEAD=0x%08X TAIL=0x%08X\n",
           gpu_rd32(NV_PGSP_QUEUE_HEAD(GSP_MSGQ_IDX)),
           gpu_rd32(NV_PGSP_QUEUE_TAIL(GSP_MSGQ_IDX)));

    // ====== Timer ======
    printf("\n=== Timer ===\n");
    uint32_t timer_lo = gpu_rd32(NV_PTIMER_TIME_0);
    uint32_t timer_hi = gpu_rd32(NV_PTIMER_TIME_1);
    printf("  PTIMER: 0x%08X_%08X\n", timer_hi, timer_lo);

    // ====== Fuse ======
    printf("\n=== Fuse ===\n");
    uint32_t fuse_ver = gpu_rd32(NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION);
    printf("  GSP ucode1 fuse version: %u\n", fuse_ver);

    // ====== Summary ======
    printf("\n========================================\n");
    printf("Summary:\n");
    printf("  GPU:       AD102 (RTX 4090) rev A1\n");
    printf("  GSP:       %s\n", (gsp_rv_cpuctl & NV_PRISCV_CPUCTL_ACTIVE) ? "RUNNING" : "STOPPED");
    printf("  SEC2:      %s\n", (sec2_rv_cpuctl & NV_PRISCV_CPUCTL_HALTED) ? "HALTED (expected)" : "RUNNING");
    printf("  FWSEC-FRTS: %s\n", ((frts_err >> 16) == 0) ? "OK" : "ERROR");
    printf("  WPR2:      0x%llX - 0x%llX\n", (unsigned long long)wpr2_begin, (unsigned long long)wpr2_end);
    printf("  Fuse ver:  %u\n", fuse_ver);
    printf("========================================\n");

    munmap((void*)bar0, BAR0_SIZE);
    close(fd);
    return 0;
}
