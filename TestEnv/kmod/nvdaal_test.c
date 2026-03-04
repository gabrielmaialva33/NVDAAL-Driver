/*
 * nvdaal_test.c - NVIDIA RTX 4090 GPU Register Access Test Module
 *
 * Read-only kernel module for inspecting GPU register state on an
 * NVIDIA RTX 4090 (AD102).  The module does NOT claim or enable the
 * PCI device; it simply locates the device via pci_get_domain_bus_and_slot(),
 * ioremaps BAR0, reads registers, and logs the results.
 *
 * This is safe to load while the nvidia proprietary driver owns the device.
 *
 * Usage:
 *   sudo insmod nvdaal_test.ko                         # default 0000:02:00.0
 *   sudo insmod nvdaal_test.ko pci_addr="0000:03:00.0" # override
 *   dmesg | grep nvdaal_test
 *   sudo rmmod nvdaal_test
 *
 * License: MIT
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/io.h>

MODULE_LICENSE("MIT");
MODULE_AUTHOR("NVDAAL Project");
MODULE_DESCRIPTION("Read-only GPU register probe for NVIDIA RTX 4090 (AD102)");
MODULE_VERSION("1.0");

/* ---- Module parameters -------------------------------------------------- */

static char *pci_addr = "0000:02:00.0";
module_param(pci_addr, charp, 0444);
MODULE_PARM_DESC(pci_addr, "PCI address of the GPU (default: 0000:02:00.0)");

/* ---- Register offsets (from NVDAALRegs.h) -------------------------------- */

/* Chip identification */
#define NV_PMC_BOOT_0                     0x00000000
#define NV_PMC_BOOT_42                    0x00000188

/* PMC */
#define NV_PMC_ENABLE                     0x00000200
#define NV_PMC_DEVICE_ENABLE              0x00000600
#define NV_PMC_INTR_EN_0                  0x00000140

/* PBUS */
#define NV_PBUS_VBIOS_SCRATCH             0x00001400
#define NV_PBUS_PCI_NV_19                 0x0000184C

/* WPR2 (Write Protected Region 2) */
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO      0x001FA824
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI      0x001FA828

/* FWSEC / FRTS status */
#define NV_PBUS_VBIOS_SCRATCH_FRTS_ERR    (NV_PBUS_VBIOS_SCRATCH + (0x0E * 4))  /* 0x001438 */
#define NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR   (NV_PBUS_VBIOS_SCRATCH + (0x15 * 4))  /* 0x001454 */

/* GSP Falcon interface (0x110000 base) */
#define NV_PGSP_BASE                      0x00110000
#define FALCON_MAILBOX0                   0x0040
#define FALCON_MAILBOX1                   0x0044
#define FALCON_CPUCTL                     0x0100
#define FALCON_BOOTVEC                    0x0104
#define FALCON_IDLESTATE                  0x004C
#define NV_PGSP_FALCON_MAILBOX0           (NV_PGSP_BASE + FALCON_MAILBOX0)
#define NV_PGSP_FALCON_MAILBOX1           (NV_PGSP_BASE + FALCON_MAILBOX1)
#define NV_PGSP_FALCON_CPUCTL             (NV_PGSP_BASE + FALCON_CPUCTL)
#define NV_PGSP_FALCON_BOOTVEC            (NV_PGSP_BASE + FALCON_BOOTVEC)
#define NV_PGSP_FALCON_IDLESTATE          (NV_PGSP_BASE + FALCON_IDLESTATE)

/* GSP queue head/tail */
#define NV_PGSP_QUEUE_HEAD(i)             (0x00110C00 + (i) * 8)
#define NV_PGSP_QUEUE_TAIL(i)             (0x00110C80 + (i) * 8)
#define GSP_CMDQ_IDX                      0
#define GSP_MSGQ_IDX                      1

/* GSP RISC-V control (Ada Lovelace: 0x118000 base) */
#define NV_PRISCV_RISCV_CPUCTL            0x00118388
#define NV_PRISCV_RISCV_BCR_CTRL          0x00118668
#define NV_PRISCV_RISCV_BR_RETCODE        0x00118400
#define NV_PRISCV_RISCV_CORE_HALT         0x00118544
#define NV_PGC6_BSI_SECURE_SCRATCH_14     0x00118F58

/* RISC-V CPUCTL bits */
#define NV_PRISCV_CPUCTL_HALTED           (1 << 4)
#define NV_PRISCV_CPUCTL_ACTIVE           (1 << 7)

/* SEC2 (0x840000 base) */
#define NV_PSEC_BASE                      0x00840000
#define NV_PSEC_FALCON_CPUCTL             (NV_PSEC_BASE + FALCON_CPUCTL)
#define NV_PSEC_FALCON_MAILBOX0           (NV_PSEC_BASE + FALCON_MAILBOX0)
#define NV_PSEC_FALCON_MAILBOX1           (NV_PSEC_BASE + FALCON_MAILBOX1)
#define NV_PSEC_RISCV_CPUCTL              0x00841388
#define NV_PSEC_RISCV_BR_RETCODE          0x00841400
#define NV_PSEC_RISCV_BCR_CTRL            0x00841668

/* Timer */
#define NV_PTIMER_TIME_0                  0x00009400
#define NV_PTIMER_TIME_1                  0x00009410

/* Architecture constants */
#define NV_CHIP_ARCH_AMPERE               0x17
#define NV_CHIP_ARCH_ADA                  0x19
#define NV_CHIP_ARCH_BLACKWELL            0x1B

/* BAR0 size (16 MB) */
#define BAR0_SIZE                         0x01000000

/* ---- Module state ------------------------------------------------------- */

static struct pci_dev *gpu_pdev;
static void __iomem *bar0;

/* ---- Helpers ------------------------------------------------------------ */

static inline u32 gpu_rd32(u32 reg)
{
    return ioread32(bar0 + reg);
}

static const char *arch_name(u32 arch)
{
    switch (arch) {
    case NV_CHIP_ARCH_AMPERE:   return "Ampere (GA1xx)";
    case NV_CHIP_ARCH_ADA:      return "Ada Lovelace (AD1xx)";
    case NV_CHIP_ARCH_BLACKWELL: return "Blackwell (GB2xx)";
    default:                    return "Unknown";
    }
}

/* ---- Parse PCI address string ------------------------------------------- */

static int parse_pci_addr(const char *str, int *domain, int *bus, int *dev, int *fn)
{
    /* Expected format: DDDD:BB:DD.F */
    if (sscanf(str, "%x:%x:%x.%x", domain, bus, dev, fn) == 4)
        return 0;
    /* Fallback: BB:DD.F (domain 0) */
    *domain = 0;
    if (sscanf(str, "%x:%x.%x", bus, dev, fn) == 3)
        return 0;
    return -EINVAL;
}

/* ---- Init --------------------------------------------------------------- */

static int __init nvdaal_test_init(void)
{
    int domain, bus, dev, fn;
    resource_size_t bar0_phys;
    resource_size_t bar0_len;
    u32 val;
    u32 boot0, arch, impl, rev;
    u32 riscv_cpuctl, falcon_cpuctl;
    u32 wpr2_lo, wpr2_hi;
    u32 frts_err, fwsec_err;
    u32 timer_lo, timer_hi;
    int i;

    pr_info("nvdaal_test: ============================================\n");
    pr_info("nvdaal_test: NVIDIA GPU Register Probe - Loading\n");
    pr_info("nvdaal_test: Target PCI address: %s\n", pci_addr);
    pr_info("nvdaal_test: ============================================\n");

    /* Parse PCI address */
    if (parse_pci_addr(pci_addr, &domain, &bus, &dev, &fn)) {
        pr_err("nvdaal_test: Invalid PCI address format: %s\n", pci_addr);
        return -EINVAL;
    }
    pr_info("nvdaal_test: Parsed PCI: domain=%04x bus=%02x dev=%02x fn=%x\n",
            domain, bus, dev, fn);

    /* Find the PCI device */
    gpu_pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(dev, fn));
    if (!gpu_pdev) {
        pr_err("nvdaal_test: PCI device %s not found\n", pci_addr);
        return -ENODEV;
    }
    pr_info("nvdaal_test: Found PCI device: %04x:%04x (subsys %04x:%04x)\n",
            gpu_pdev->vendor, gpu_pdev->device,
            gpu_pdev->subsystem_vendor, gpu_pdev->subsystem_device);

    /* Get BAR0 physical address and length */
    bar0_phys = pci_resource_start(gpu_pdev, 0);
    bar0_len  = pci_resource_len(gpu_pdev, 0);
    if (!bar0_phys || !bar0_len) {
        pr_err("nvdaal_test: BAR0 not configured (phys=0x%llx len=0x%llx)\n",
               (u64)bar0_phys, (u64)bar0_len);
        pci_dev_put(gpu_pdev);
        gpu_pdev = NULL;
        return -ENODEV;
    }
    pr_info("nvdaal_test: BAR0 physical: 0x%llx, length: 0x%llx (%llu MB)\n",
            (u64)bar0_phys, (u64)bar0_len, (u64)bar0_len >> 20);

    /* Map BAR0 - we do NOT call pci_enable_device() or pci_request_regions()
     * because the nvidia driver already owns this device. */
    bar0 = ioremap(bar0_phys, bar0_len < BAR0_SIZE ? bar0_len : BAR0_SIZE);
    if (!bar0) {
        pr_err("nvdaal_test: Failed to ioremap BAR0\n");
        pci_dev_put(gpu_pdev);
        gpu_pdev = NULL;
        return -ENOMEM;
    }
    pr_info("nvdaal_test: BAR0 mapped at virtual address %px\n", bar0);

    /* ------------------------------------------------------------------ */
    /* 1. Chip Identification                                              */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- Chip Identification ---\n");

    boot0 = gpu_rd32(NV_PMC_BOOT_0);
    arch  = (boot0 >> 20) & 0x1F;
    impl  = (boot0 >> 16) & 0x0F;
    rev   = boot0 & 0xFF;
    pr_info("nvdaal_test: PMC_BOOT_0     = 0x%08X\n", boot0);
    pr_info("nvdaal_test:   Architecture  = 0x%02X (%s)\n", arch, arch_name(arch));
    pr_info("nvdaal_test:   Implementation= 0x%X\n", impl);
    pr_info("nvdaal_test:   Revision      = 0x%02X\n", rev);

    val = gpu_rd32(NV_PMC_BOOT_42);
    pr_info("nvdaal_test: PMC_BOOT_42    = 0x%08X\n", val);

    /* ------------------------------------------------------------------ */
    /* 2. PMC Enable / Interrupt Status                                    */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- PMC Status ---\n");

    val = gpu_rd32(NV_PMC_ENABLE);
    pr_info("nvdaal_test: PMC_ENABLE      = 0x%08X\n", val);
    val = gpu_rd32(NV_PMC_DEVICE_ENABLE);
    pr_info("nvdaal_test: PMC_DEV_ENABLE  = 0x%08X\n", val);
    val = gpu_rd32(NV_PMC_INTR_EN_0);
    pr_info("nvdaal_test: PMC_INTR_EN_0   = 0x%08X\n", val);

    /* ------------------------------------------------------------------ */
    /* 3. GSP State (RISC-V core + Falcon interface)                       */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- GSP State ---\n");

    riscv_cpuctl = gpu_rd32(NV_PRISCV_RISCV_CPUCTL);
    pr_info("nvdaal_test: GSP RISCV CPUCTL (0x%06X) = 0x%08X [%s%s]\n",
            NV_PRISCV_RISCV_CPUCTL, riscv_cpuctl,
            (riscv_cpuctl & NV_PRISCV_CPUCTL_ACTIVE) ? "ACTIVE " : "",
            (riscv_cpuctl & NV_PRISCV_CPUCTL_HALTED) ? "HALTED" : "RUNNING");

    falcon_cpuctl = gpu_rd32(NV_PGSP_FALCON_CPUCTL);
    pr_info("nvdaal_test: GSP Falcon CPUCTL (0x%06X) = 0x%08X\n",
            NV_PGSP_BASE + FALCON_CPUCTL, falcon_cpuctl);

    val = gpu_rd32(NV_PGSP_FALCON_MAILBOX0);
    pr_info("nvdaal_test: GSP Falcon MBOX0 (0x%06X) = 0x%08X\n",
            NV_PGSP_BASE + FALCON_MAILBOX0, val);
    val = gpu_rd32(NV_PGSP_FALCON_MAILBOX1);
    pr_info("nvdaal_test: GSP Falcon MBOX1 (0x%06X) = 0x%08X\n",
            NV_PGSP_BASE + FALCON_MAILBOX1, val);

    val = gpu_rd32(NV_PGSP_FALCON_BOOTVEC);
    pr_info("nvdaal_test: GSP Falcon BOOTVEC (0x%06X) = 0x%08X\n",
            NV_PGSP_BASE + FALCON_BOOTVEC, val);
    val = gpu_rd32(NV_PGSP_FALCON_IDLESTATE);
    pr_info("nvdaal_test: GSP Falcon IDLE   (0x%06X) = 0x%08X\n",
            NV_PGSP_BASE + FALCON_IDLESTATE, val);

    val = gpu_rd32(NV_PRISCV_RISCV_BCR_CTRL);
    pr_info("nvdaal_test: GSP RISCV BCR_CTRL (0x%06X) = 0x%08X\n",
            NV_PRISCV_RISCV_BCR_CTRL, val);
    val = gpu_rd32(NV_PRISCV_RISCV_BR_RETCODE);
    pr_info("nvdaal_test: GSP RISCV BR_RETCODE (0x%06X) = 0x%08X\n",
            NV_PRISCV_RISCV_BR_RETCODE, val);
    val = gpu_rd32(NV_PRISCV_RISCV_CORE_HALT);
    pr_info("nvdaal_test: GSP RISCV CORE_HALT (0x%06X) = 0x%08X\n",
            NV_PRISCV_RISCV_CORE_HALT, val);

    val = gpu_rd32(NV_PGC6_BSI_SECURE_SCRATCH_14);
    pr_info("nvdaal_test: BSI_SECURE_SCRATCH_14 (0x%06X) = 0x%08X\n",
            NV_PGC6_BSI_SECURE_SCRATCH_14, val);

    /* ------------------------------------------------------------------ */
    /* 4. SEC2 State                                                       */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- SEC2 State ---\n");

    val = gpu_rd32(NV_PSEC_FALCON_CPUCTL);
    pr_info("nvdaal_test: SEC2 Falcon CPUCTL (0x%06X) = 0x%08X\n",
            NV_PSEC_BASE + FALCON_CPUCTL, val);
    val = gpu_rd32(NV_PSEC_FALCON_MAILBOX0);
    pr_info("nvdaal_test: SEC2 Falcon MBOX0  (0x%06X) = 0x%08X\n",
            NV_PSEC_BASE + FALCON_MAILBOX0, val);
    val = gpu_rd32(NV_PSEC_FALCON_MAILBOX1);
    pr_info("nvdaal_test: SEC2 Falcon MBOX1  (0x%06X) = 0x%08X\n",
            NV_PSEC_BASE + FALCON_MAILBOX1, val);

    val = gpu_rd32(NV_PSEC_RISCV_CPUCTL);
    pr_info("nvdaal_test: SEC2 RISCV CPUCTL  (0x%06X) = 0x%08X [%s%s]\n",
            NV_PSEC_RISCV_CPUCTL, val,
            (val & NV_PRISCV_CPUCTL_ACTIVE) ? "ACTIVE " : "",
            (val & NV_PRISCV_CPUCTL_HALTED) ? "HALTED" : "RUNNING");
    val = gpu_rd32(NV_PSEC_RISCV_BR_RETCODE);
    pr_info("nvdaal_test: SEC2 RISCV BR_RET  (0x%06X) = 0x%08X\n",
            NV_PSEC_RISCV_BR_RETCODE, val);
    val = gpu_rd32(NV_PSEC_RISCV_BCR_CTRL);
    pr_info("nvdaal_test: SEC2 RISCV BCR_CTRL(0x%06X) = 0x%08X\n",
            NV_PSEC_RISCV_BCR_CTRL, val);

    /* ------------------------------------------------------------------ */
    /* 5. WPR2 Status                                                      */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- WPR2 (Write Protected Region 2) ---\n");

    wpr2_lo = gpu_rd32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    wpr2_hi = gpu_rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    pr_info("nvdaal_test: WPR2_ADDR_LO (0x%06X) = 0x%08X\n",
            NV_PFB_PRI_MMU_WPR2_ADDR_LO, wpr2_lo);
    pr_info("nvdaal_test: WPR2_ADDR_HI (0x%06X) = 0x%08X\n",
            NV_PFB_PRI_MMU_WPR2_ADDR_HI, wpr2_hi);
    pr_info("nvdaal_test: WPR2 enabled: %s\n",
            (wpr2_lo >> 31) & 1 ? "YES" : "NO");

    if ((wpr2_lo >> 31) & 1) {
        u64 lo_addr = ((u64)(wpr2_lo & 0xFFFFF) << 20);
        u64 hi_addr = ((u64)(wpr2_hi & 0xFFFFF) << 20);
        pr_info("nvdaal_test: WPR2 range: 0x%llx - 0x%llx (%llu MB)\n",
                lo_addr, hi_addr,
                hi_addr > lo_addr ? (hi_addr - lo_addr) >> 20 : 0);
    }

    /* ------------------------------------------------------------------ */
    /* 6. FWSEC / FRTS Status                                              */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- FWSEC / FRTS Status ---\n");

    frts_err = gpu_rd32(NV_PBUS_VBIOS_SCRATCH_FRTS_ERR);
    pr_info("nvdaal_test: FRTS error reg (0x%06X) = 0x%08X (upper16=0x%04X -> %s)\n",
            NV_PBUS_VBIOS_SCRATCH_FRTS_ERR, frts_err,
            (frts_err >> 16) & 0xFFFF,
            ((frts_err >> 16) & 0xFFFF) == 0 ? "SUCCESS" : "ERROR");

    fwsec_err = gpu_rd32(NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR);
    pr_info("nvdaal_test: FWSEC error reg (0x%06X) = 0x%08X (lower16=0x%04X -> %s)\n",
            NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR, fwsec_err,
            fwsec_err & 0xFFFF,
            (fwsec_err & 0xFFFF) == 0 ? "SUCCESS" : "ERROR");

    val = gpu_rd32(NV_PBUS_PCI_NV_19);
    pr_info("nvdaal_test: PBUS_PCI_NV_19  (0x%06X) = 0x%08X (BAR0 active: %s)\n",
            NV_PBUS_PCI_NV_19, val,
            (val & 1) ? "YES" : "NO");

    /* ------------------------------------------------------------------ */
    /* 7. Timer                                                            */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- Timer ---\n");

    timer_hi = gpu_rd32(NV_PTIMER_TIME_1);
    timer_lo = gpu_rd32(NV_PTIMER_TIME_0);
    pr_info("nvdaal_test: PTIMER_TIME_1 (0x%06X) = 0x%08X\n",
            NV_PTIMER_TIME_1, timer_hi);
    pr_info("nvdaal_test: PTIMER_TIME_0 (0x%06X) = 0x%08X\n",
            NV_PTIMER_TIME_0, timer_lo);
    pr_info("nvdaal_test: GPU timer: 0x%08X_%08X\n", timer_hi, timer_lo);

    /* ------------------------------------------------------------------ */
    /* 8. GSP Queue Head/Tail                                              */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- GSP Queues ---\n");

    for (i = 0; i < 8; i++) {
        u32 head = gpu_rd32(NV_PGSP_QUEUE_HEAD(i));
        u32 tail = gpu_rd32(NV_PGSP_QUEUE_TAIL(i));
        if (head != 0 || tail != 0 || i < 2) {
            pr_info("nvdaal_test: Queue[%d] HEAD=0x%08X  TAIL=0x%08X%s\n",
                    i, head, tail,
                    i == GSP_CMDQ_IDX ? "  (CMD)" :
                    i == GSP_MSGQ_IDX ? "  (MSG)" : "");
        }
    }

    /* ------------------------------------------------------------------ */
    /* 9. PBUS Scratch registers (first few)                               */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: --- PBUS VBIOS Scratch (selection) ---\n");

    for (i = 0; i < 4; i++) {
        val = gpu_rd32(NV_PBUS_VBIOS_SCRATCH + i * 4);
        pr_info("nvdaal_test: VBIOS_SCRATCH[%02d] (0x%06X) = 0x%08X\n",
                i, NV_PBUS_VBIOS_SCRATCH + i * 4, val);
    }

    /* ------------------------------------------------------------------ */
    /* Summary                                                             */
    /* ------------------------------------------------------------------ */
    pr_info("nvdaal_test: ============================================\n");
    pr_info("nvdaal_test: SUMMARY\n");
    pr_info("nvdaal_test:   GPU         : %s (0x%02X) rev %c%X\n",
            arch_name(arch), arch,
            'A' + ((rev >> 4) & 0xF), rev & 0xF);
    pr_info("nvdaal_test:   PCI ID      : %04x:%04x\n",
            gpu_pdev->vendor, gpu_pdev->device);
    pr_info("nvdaal_test:   BAR0        : 0x%llx (%llu MB)\n",
            (u64)bar0_phys, (u64)bar0_len >> 20);
    pr_info("nvdaal_test:   GSP RISC-V  : %s\n",
            (riscv_cpuctl & NV_PRISCV_CPUCTL_ACTIVE) ? "ACTIVE (nvidia driver running)" :
            (riscv_cpuctl & NV_PRISCV_CPUCTL_HALTED) ? "HALTED" : "UNKNOWN");
    pr_info("nvdaal_test:   WPR2        : %s\n",
            (wpr2_lo >> 31) & 1 ? "ENABLED" : "DISABLED");
    pr_info("nvdaal_test:   FRTS        : %s\n",
            ((frts_err >> 16) & 0xFFFF) == 0 ? "OK (no error)" : "ERROR");
    pr_info("nvdaal_test:   FWSEC       : %s\n",
            (fwsec_err & 0xFFFF) == 0 ? "OK (no error)" : "ERROR");
    pr_info("nvdaal_test: ============================================\n");
    pr_info("nvdaal_test: Module loaded successfully. Use dmesg to review.\n");

    return 0;
}

/* ---- Exit --------------------------------------------------------------- */

static void __exit nvdaal_test_exit(void)
{
    pr_info("nvdaal_test: Unloading module...\n");

    if (bar0) {
        iounmap(bar0);
        bar0 = NULL;
        pr_info("nvdaal_test: BAR0 unmapped\n");
    }

    if (gpu_pdev) {
        pci_dev_put(gpu_pdev);
        gpu_pdev = NULL;
        pr_info("nvdaal_test: PCI device reference released\n");
    }

    pr_info("nvdaal_test: Module unloaded\n");
}

module_init(nvdaal_test_init);
module_exit(nvdaal_test_exit);
