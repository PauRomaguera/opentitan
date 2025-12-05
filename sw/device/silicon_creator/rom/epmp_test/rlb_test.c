// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/*
bazel test --test_output=streamed --test_timeout=999999 --disk_cache=~/bazel_cache //sw/device/silicon_creator/rom:rlb_test
To add debug: --copt=-DDEBUG
*/

#include "sw/device/silicon_creator/rom/rom_epmp.h"

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/stdasm.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/dif/dif_sram_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/runtime/print.h"
#include "sw/device/lib/testing/pinmux_testutils.h"
#include "sw/device/lib/testing/test_framework/status.h"
#include "sw/device/silicon_creator/lib/base/sec_mmio.h"
#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"
#include "sw/device/silicon_creator/lib/drivers/uart.h"
#include "sw/device/silicon_creator/lib/epmp_test_unlock.h"
#include "sw/device/silicon_creator/rom/rom_epmp.h"

//Generated headers
#include "rstmgr_regs.h"
#include "aon_timer_regs.h"
#include "flash_ctrl_regs.h"
#include "sram_ctrl_regs.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

/**
 * ROM ePMP  baseline test.
 *
 * This test uses the ROM linker script and ePMP setup code to initialize
 * its own ePMP configuration and then attempts to execute instructions, write or read 
 * in various address spaces. 
 */

/*

typedef enum ibex_exc {
  kIbexExcInstrMisaligned = 0,
  kIbexExcInstrAccessFault = 1,
  kIbexExcIllegalInstrFault = 2,
  kIbexExcBreakpoint = 3,
  kIbexExcLoadAccessFault = 5,
  kIbexExcStoreAccessFault = 7,
  kIbexExcUserECall = 8,
  kIbexExcMachineECall = 11,
  kIbexExcMax = 31
} ibex_exc_t;

*/

// ------- Global Variables --------
//The type of last exception (if any) received.
volatile ibex_exc_t exception_received = 0;

// The `mepc` value for the last exception (if any) received.
volatile uintptr_t exception_pc = 0;

/**
 * An instruction that has all bits set. This value is specifically chosen to
 * match an erased flash.
 *
 * Attempts to execute this instruction, `unimp`, will result in an illegal
 * instruction exception.
 */
static const uint32_t kUnimpInstruction = UINT32_MAX;


static bool passed = false;

// ------ Helper functions ------

static uint32_t get_mcause(void) {
  uint32_t mcause;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  return mcause;
}

/**
 * Get the value of the `mepc` register.
 *
 * @returns The value of the machine exception program counter.
 */
static uint32_t get_mepc(void) {
  uint32_t mepc;
  CSR_READ(CSR_REG_MEPC, &mepc);
  return mepc;
}

static void set_mepc(uint32_t pc) { CSR_WRITE(CSR_REG_MEPC, pc); }

static inline uint32_t read_mseccfg(void) {
  uint32_t MSECCFG;
  CSR_READ(CSR_REG_MSECCFG, &MSECCFG);
  return MSECCFG;
}

static inline void write_mseccfg(uint32_t value) {
  CSR_WRITE(CSR_REG_MSECCFG, value);
}

static inline bool addr_in_range(uintptr_t addr, uintptr_t base, uintptr_t size) {
  return (addr - base) < size;
}

// Pointer wrapper for convenience
static inline bool is_in_address_space(const void *ptr, uintptr_t base, uintptr_t size) {
  return addr_in_range((uintptr_t)ptr, base, size);
}


#ifdef DEBUG
static void dump_reset_info(void) {
  uint32_t info =
      abs_mmio_read32(TOP_EARLGREY_RSTMGR_AON_BASE_ADDR +
                      RSTMGR_RESET_INFO_REG_OFFSET);
  LOG_INFO("reset_info=0x%08x", info);
  abs_mmio_write32(TOP_EARLGREY_RSTMGR_AON_BASE_ADDR +
                   RSTMGR_RESET_INFO_REG_OFFSET,
                   info);
}

static const char *addr_region(uintptr_t a) {
  if (addr_in_range(a, TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR,
                     TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES)) return "ROM";
  if (addr_in_range(a, TOP_EARLGREY_EFLASH_BASE_ADDR,
                     TOP_EARLGREY_EFLASH_SIZE_BYTES)) return "eFLASH";
  if (addr_in_range(a, TOP_EARLGREY_RAM_MAIN_BASE_ADDR,
                     TOP_EARLGREY_RAM_MAIN_SIZE_BYTES)) return "RAM";
  if (addr_in_range(a, TOP_EARLGREY_RAM_RET_AON_BASE_ADDR,
                     TOP_EARLGREY_RAM_RET_AON_SIZE_BYTES)) return "RET_RAM";
  if (addr_in_range(a, TOP_EARLGREY_MMIO_BASE_ADDR,
                     TOP_EARLGREY_MMIO_SIZE_BYTES)) return "MMIO";
  return "UNKNOWN";
}

static const char *irq_name(uint32_t code) {
  switch (code) {
    case 3:  return "MSI";   // Machine SW
    case 7:  return "MTI";   // Machine Timer
    case 11: return "MEI";   // Machine External
    default: return "IRQ";
  }
}
/*
static void dump_pmp_min(void) {
  uint32_t mseccfg; 
  CSR_READ(CSR_REG_MSECCFG, &mseccfg);
  
  uint32_t cfg[4] = {0};
  CSR_READ(CSR_REG_PMPCFG0, &cfg[0]);
  CSR_READ(CSR_REG_PMPCFG1, &cfg[1]);
  CSR_READ(CSR_REG_PMPCFG2, &cfg[2]);
  CSR_READ(CSR_REG_PMPCFG3, &cfg[3]);

  uint32_t addr[16] = {0};
  CSR_READ(CSR_REG_PMPADDR0,  &addr[0]);
  CSR_READ(CSR_REG_PMPADDR1,  &addr[1]);
  CSR_READ(CSR_REG_PMPADDR2,  &addr[2]);
  CSR_READ(CSR_REG_PMPADDR3,  &addr[3]);
  CSR_READ(CSR_REG_PMPADDR4,  &addr[4]);
  CSR_READ(CSR_REG_PMPADDR5,  &addr[5]);
  CSR_READ(CSR_REG_PMPADDR6,  &addr[6]);
  CSR_READ(CSR_REG_PMPADDR7,  &addr[7]);
  CSR_READ(CSR_REG_PMPADDR8,  &addr[8]);
  CSR_READ(CSR_REG_PMPADDR9,  &addr[9]);
  CSR_READ(CSR_REG_PMPADDR10, &addr[10]);
  CSR_READ(CSR_REG_PMPADDR11, &addr[11]);
  CSR_READ(CSR_REG_PMPADDR12, &addr[12]);
  CSR_READ(CSR_REG_PMPADDR13, &addr[13]);
  CSR_READ(CSR_REG_PMPADDR14, &addr[14]);
  CSR_READ(CSR_REG_PMPADDR15, &addr[15]);
  for (int i = 0; i < 4; i++) {
    LOG_INFO("PMPCFG%u=0x%08x", i, cfg[i]);
  }
  for (int i = 0; i < 16; i++) {
    LOG_INFO("PMPADDR%u=0x%08x", i, addr[i]);
  }
  LOG_INFO("MSECCFG=0x%08x (MMWP=%u RLB=%u MML=%u)",
    mseccfg, !!(mseccfg & EPMP_MSECCFG_MMWP),
    !!(mseccfg & EPMP_MSECCFG_RLB),
    !!(mseccfg & EPMP_MSECCFG_MML));
}
    */
#endif 
#ifdef DEBUG
static inline void dbg_log_last_trap(const char *op, const void *addr,
                                     ibex_exc_t expect) {
  if (exception_received != expect) {
    LOG_INFO("%s: trap mismatch, mcause=0x%08x expected=0x%08x",
             op,
             (unsigned)exception_received,
             (unsigned)expect);
    LOG_INFO("  mepc=0x%08x addr=%p",
             (unsigned)exception_pc,
             addr);
  } else {
    LOG_INFO("%s: trap as expected, mcause=0x%08x expected=0x%08x",
             op,
             (unsigned)exception_received,
             (unsigned)expect);
    LOG_INFO("  mepc=0x%08x addr=%p",
             (unsigned)exception_pc,
             addr);
  }
}
#else
static inline void dbg_log_last_trap(const char *op, const void *addr,
                                     ibex_exc_t expect) {
  (void)op;
  (void)addr;
  (void)expect;
}
#endif

  



//------- Exception, Interrupt, NMI Handlers ------

/**
 * Interrupt handlers.
 *
 * If operating correctly this test should only trigger exceptions. Interrupts
 * are therefore not recovered.
 */
// NMI = Non-Maskable Interrupt
void rom_nmi_handler(void) {
  #ifdef DEBUG
  uint32_t mcause = get_mcause();
  uint32_t mepc   = get_mepc();
  LOG_INFO("NMI: mcause=0x%08x, mepc=0x%08x, region=%s",
           mcause, mepc, addr_region(mepc));
  #else
  LOG_INFO("IRQ Received, resetting...");
  wait_for_interrupt();
  #endif
}

void rom_interrupt_handler(void) {
  #ifdef DEBUG
  uint32_t mcause = get_mcause();
  uint32_t mepc   = get_mepc();
  uint32_t code   = mcause & 0x1ffu;  // cause code (MSB is interrupt bit)
  uint16_t insn16 = *(const volatile uint16_t *)(uintptr_t)mepc;
  LOG_INFO("IRQ: code=%u(%s), mepc=0x%08x, region=%s, insn16=0x%04x",
           code, irq_name(code), mepc, addr_region(mepc), insn16);
  #else
  LOG_INFO("IRQ Received, resetting...");
  wait_for_interrupt();
  #endif
}

/**
 * Exception handler.
 *
 * Handles different exceptions coming from either execute(), read32() or
 * write32() functions. Gets automatically called when an exception is recieved
 * 
 * For execution:
 * Handle instruction access faults and illegal instructions by setting
 * `exception_received` and `exception_pc` and then returning to the code
 * that jumped (via a call) to the offending instruction.
 *
 * For reads/writes:
 * Handle load and store access faults by setting `exception_received` and
 * `exception_pc` and then returning to the instruction after the offending
 * access. Returning to the correct address sometimes is a bit tricky since
 * Ibex sometimes compresses 32bit instructions into 16bit so we have to add
 * some extra logic, it's not as simple as adding +4 to the program counter.
 *
 * For all other exceptions hang (could also shutdown) so as not to hide them.
 */
void rom_exception_handler(void) __attribute__((interrupt));
void rom_exception_handler(void) {
  uint32_t mcause = get_mcause();
  uint32_t mepc = get_mepc();

  if (mcause == kIbexExcInstrAccessFault || mcause == kIbexExcIllegalInstrFault) {
    exception_received = (ibex_exc_t)mcause;
    exception_pc = mepc;
    
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    set_mepc((uint32_t)ret);
    return;
  }
  if (mcause == kIbexExcLoadAccessFault || mcause == kIbexExcStoreAccessFault) {
    exception_received = (ibex_exc_t)mcause;
    exception_pc = mepc;
    /*
    Advance to the next instruction
    Doesnt work since RISC-V might compress instructions to 16-bit
    So adding 32bit (+4) to the PC doesn't always work 
    set_mepc(mepc + 4);
    */
    //therefore we check the instruction size and add either 4 or 2 bytes
    uint16_t insn16 = *(const uint16_t *)(uintptr_t)mepc;
    uint32_t next = mepc + ((insn16 & 0x3) == 0x3 ? 4u : 2u);
    set_mepc(next);
    return;
  }
  #ifdef DEBUG
  LOG_ERROR("Unexpected exception: mcause=0x%x, mepc=0x%x, resetting...", mcause, mepc);
  #endif
  LOG_ERROR("Unexpected exception, resetting...");
  wait_for_interrupt();
}

//------- Memory Access Helpers -------
/*
 * Helper functions to perform memory accesses and check for expected exceptions.
 *
 * Before executing, resets the global exception exception_received variable to 
 * kIbexExcMax (no exception). Performs a load or store to the given address.
 * If an exception occurs, the exception handler will update exception_received
 * accordingly. After the access, compares exception_received to the expected
 * value and returns whether they match.
 *
 * For reads, the expected values are typically:
 *  - kIbexExcMax: no exception
 *  - kIbexExcLoadAccessFault: load fault (Read denied / unmapped)
 * For writes, the expected values are typically:
 *  - kIbexExcMax: no exception
 *  - kIbexExcStoreAccessFault: store fault (Write denied / unmapped)
 */

static bool read32(const void *addr, ibex_exc_t expect) {
        exception_received = kIbexExcMax;
        //pointer to the target word
        //volatile tells the compiler not to optimize it away
        volatile const uint32_t *p = (const uint32_t *) addr;
        volatile uint32_t sink = *p; //perform the load
        
        (void) sink; //remove unused variable warning
        dbg_log_last_trap("read32", addr, expect);
        return exception_received == expect; 
}
static bool write32(void *addr, uint32_t val, ibex_exc_t expect) {
        exception_received = kIbexExcMax;
        volatile uint32_t *p = (uint32_t *) addr;
        *p = val;
        dbg_log_last_trap("write32", addr, expect);
        return exception_received == expect;
}
// ------- Execute Helper -------

/**
 * Attempt to execute the code at "pc" by calling it like a function.
 *
 * Typically the contents of `pc` should be an invalid instruction such
 * as an all zero value. In this case if execution was blocked by PMP an
 * instruction fault exception will be raised. If however execution was
 * allowed then an illegal instruction exception will be raised instead.
 *
 * Before executing, resets the global exception exception_received variable to
 * kIbexExcMax (no exception) and the exception_pc variable to 0. Performs a
 * call to the given address. If an exception occurs, the exception handler will
 * update exception_received and exception_pc accordingly. After the call,
 * compares exception_received to the expected value and returns whether they
 * match.
 */
static bool execute(const void *pc, ibex_exc_t expect) {
  exception_pc = 0;
  exception_received = kIbexExcMax;

  ((void (*)(void))pc)();
  dbg_log_last_trap("execute", pc, expect);
  if (exception_received != kIbexExcMax && exception_pc != (uintptr_t)pc) {
    return false;
  }
  return exception_received == expect;
}

// Check macro for test conditions
#define CHECK(condition)                  \
  if (!(condition)) {                     \
    LOG_ERROR("ºfail: " #condition); \
    passed = false;                       \
  }

// ---------- Tests ----------

/**
 * 7 & 8)
 * eFLASH can't eXecute + ROM_EXT can't eXecute
 * They live in the same memory, at init time only eFLASH
 * is configured therefore this test serves us for both 
 * eFLASH and ROM_EXT
 */

static void test_noexec_eflash(void) {
  //checking only start/end addresses since doing more testing would
  //take forever in simulation
  uint32_t *eflash = (uint32_t *)TOP_EARLGREY_EFLASH_BASE_ADDR;
  size_t eflash_len = TOP_EARLGREY_EFLASH_SIZE_BYTES / sizeof(eflash[0]);
  CHECK(execute(&eflash[0], kIbexExcInstrAccessFault));
  CHECK(execute(&eflash[eflash_len - 1], kIbexExcInstrAccessFault));
}

/**
 * 15 & 16)
 * ROM_EXT unlock & eXecution
 * Test done by OpenTitan team
 * Test the function used to unlock execution of the ROM extension.
 *
 * Unlock a section of eFlash to simulate the unlocking of the ROM_EXT text.
 * Accesses within the unlocked region should execute (and generate an illegal
 * instruction exception in this case) while accesses outside the unlocked
 * region should still fail with an instruction access fault exception.
 *
 * @param epmp The ePMP state to update.
 */
static void test_unlock_exec_eflash(void) {
  // Define a region to unlock (this is somewhat arbitrary but must be word-
  // aligned and beyond the ROM region, since this same image is placed in the
  // flash).
  uint32_t *eflash = (uint32_t *)TOP_EARLGREY_EFLASH_BASE_ADDR;
  size_t eflash_len = TOP_EARLGREY_EFLASH_SIZE_BYTES / sizeof(eflash[0]);
  uint32_t *image = &eflash[eflash_len / 5];
  size_t image_len = eflash_len / 7;
  epmp_region_t region = {.start = (uintptr_t)&image[0],
                          .end = (uintptr_t)&image[image_len]};

  // Unlock execution of the region and check that the same changes are made
  // to the ePMP state.
  rom_epmp_unlock_rom_ext_rx(region);
  //CHECK(epmp_state_check() == kErrorOk);

  // Verify that execution within the region succeeds.
  // The image must consist of `unimp` instructions so that an illegal
  // instruction exception is generated. Because the region is not written and
  // tests begin with the flash erased, this instruction is expected to be
  // UINT32_MAX.
//  CHECK(image[0] == kUnimpInstruction);
//  CHECK(execute(&image[0], kIbexExcIllegalInstrFault));
//  CHECK(image[image_len - 1] == kUnimpInstruction);
////  CHECK(execute(&image[image_len - 1], kIbexExcIllegalInstrFault));

  // Verify that execution just outside the region still fails.
//  CHECK(execute(&image[-1], kIbexExcInstrAccessFault));
//  CHECK(execute(&image[image_len], kIbexExcInstrAccessFault));
}

/**
 * 17)
 * RLB 1->0
 *
 */
static void test_rlb_one_to_zero(void) {
  //reset has MMWP=1, RLB=1, MML=0.
  //should be 00000111b
  uint32_t m0 = read_mseccfg();
  //check original config, make sure it's correct
  //should change this for the ablation test
  CHECK(m0 & EPMP_MSECCFG_MMWP);
  //00000111b & 00000010b = 1
  CHECK(m0 & EPMP_MSECCFG_RLB);    
  //00000111b & 00000100b = 1
       
  // Clear RLB, preserving the other bits.
  uint32_t m1 = m0 & ~(uint32_t)EPMP_MSECCFG_RLB;
  write_mseccfg(m1);

  // Read back: RLB must be 0, MMWP should remain set.
  uint32_t m2 = read_mseccfg();
  //00000011b & 00000100b = 0
  CHECK((m2 & EPMP_MSECCFG_RLB) == 0);
  CHECK(m2 & EPMP_MSECCFG_MMWP);
}
/**
 * 18)
 * ROM_EXT can't re-lock eXecution
 *
 */


static void test_rom_ext_cant_relock_exec(void) {
  // Define a region to re-lock (this is somewhat arbitrary but must be word-
  // aligned and within the previously unlocked region).
  uint32_t *eflash = (uint32_t *)TOP_EARLGREY_EFLASH_BASE_ADDR;
  size_t eflash_len = TOP_EARLGREY_EFLASH_SIZE_BYTES / sizeof(eflash[0]);
  uint32_t *image = &eflash[eflash_len / 5];
  size_t image_len = eflash_len / 7;
  epmp_region_t region = {.start = (uintptr_t)&image[image_len / 4],
                          .end = (uintptr_t)&image[(image_len * 3) / 4]};

  // Attempt to re-lock execution of the region.
  CSR_WRITE(CSR_REG_PMPADDR3, region.start >> 2);  // low bound
  CSR_WRITE(CSR_REG_PMPADDR4, region.end   >> 2);  // high bound

  // Program pmp4cfg (lowest byte of pmpcfg1) to: A=TOR, L=1, R=1, X=0 (no W).
  // Clear just the pmp4cfg byte, then set the new value.
  CSR_CLEAR_BITS(CSR_REG_PMPCFG1, 0xFFu);                    // clear pmp4cfg
  CSR_SET_BITS(  CSR_REG_PMPCFG1, kEpmpModeTor | kEpmpPermLockedReadOnly);  
  //CHECK(epmp_state_check() == kErrorOk);

  // Verify that execution within the region still succeeds.
  CHECK(image[0] == kUnimpInstruction);
  CHECK(execute(&image[0], kIbexExcIllegalInstrFault));
  CHECK(image[image_len - 1] == kUnimpInstruction);
  CHECK(execute(&image[image_len - 1], kIbexExcIllegalInstrFault));
}
//Test that we CAN lock exec
//only difference is we expect kIbexExcIllegalInstrFault instead of illegal access
//so we do 
static void test_lock_exec_eflash(void) {
  // Define a region to re-lock (this is somewhat arbitrary but must be word-
  // aligned and within the previously unlocked region).
  uint32_t *eflash = (uint32_t *)TOP_EARLGREY_EFLASH_BASE_ADDR;
  size_t eflash_len = TOP_EARLGREY_EFLASH_SIZE_BYTES / sizeof(eflash[0]);
  uint32_t *image = &eflash[eflash_len / 5];
  size_t image_len = eflash_len / 7;
  epmp_region_t region = {.start = (uintptr_t)&image[image_len / 4],
                          .end = (uintptr_t)&image[(image_len * 3) / 4]};

  // Attempt to re-lock execution of the region.
  CSR_WRITE(CSR_REG_PMPADDR3, region.start >> 2);  // low bound
  CSR_WRITE(CSR_REG_PMPADDR4, region.end   >> 2);  // high bound

  // Program pmp4cfg (lowest byte of pmpcfg1) to: A=TOR, L=1, R=1, X=0 (no W).
  // Clear just the pmp4cfg byte, then set the new value.
  CSR_CLEAR_BITS(CSR_REG_PMPCFG1, 0xFFu);                    // clear pmp4cfg
  CSR_SET_BITS(  CSR_REG_PMPCFG1, kEpmpModeTor | kEpmpPermLockedReadOnly);  
  //CHECK(epmp_state_check() == kErrorOk);

  // Verify that execution within the region still succeeds.
  CHECK(image[0] == kUnimpInstruction);
  CHECK(execute(&image[0], kIbexExcInstrAccessFault));
  CHECK(image[image_len - 1] == kUnimpInstruction);
  CHECK(execute(&image[image_len - 1], kIbexExcInstrAccessFault));
}
//test we can't reunlock it
static void test_rom_ext_cant_reunlock_exec(void) {
  // Define a region to re-lock (this is somewhat arbitrary but must be word-
  // aligned and within the previously unlocked region).
  uint32_t *eflash = (uint32_t *)TOP_EARLGREY_EFLASH_BASE_ADDR;
  size_t eflash_len = TOP_EARLGREY_EFLASH_SIZE_BYTES / sizeof(eflash[0]);
  uint32_t *image = &eflash[eflash_len / 5];
  size_t image_len = eflash_len / 7;
  epmp_region_t region = {.start = (uintptr_t)&image[image_len / 4],
                          .end = (uintptr_t)&image[(image_len * 3) / 4]};

  // Attempt to re-lock execution of the region.
  CSR_WRITE(CSR_REG_PMPADDR3, region.start >> 2);  // low bound
  CSR_WRITE(CSR_REG_PMPADDR4, region.end   >> 2);  // high bound

  // Program pmp4cfg (lowest byte of pmpcfg1) to: A=TOR, L=1, R=1, X=0 (no W).
  // Clear just the pmp4cfg byte, then set the new value.
  CSR_CLEAR_BITS(CSR_REG_PMPCFG1, 0xFFu);                    // clear pmp4cfg
  CSR_SET_BITS(CSR_REG_PMPCFG1, kEpmpModeTor | kEpmpPermLockedReadWriteExecute);  
  //CHECK(epmp_state_check() == kErrorOk);

  // Verify that execution within the region still succeeds.
  CHECK(image[0] == kUnimpInstruction);
  CHECK(execute(&image[0], kIbexExcInstrAccessFault));
  CHECK(image[image_len - 1] == kUnimpInstruction);
  CHECK(execute(&image[image_len - 1], kIbexExcInstrAccessFault));
}

/**
 * 19)
 * Can't RLB 0->1
 *
 *
 */
static void test_cant_rlb_zero_to_one(void) {
  // Read current MSECCFG and ensure RLB is already cleared.
  uint32_t m0 = read_mseccfg();
  CHECK((m0 & EPMP_MSECCFG_RLB) == 0);

  // Attempt to set RLB (software write). This should have no effect.
  uint32_t attempt = m0 | EPMP_MSECCFG_RLB;
  write_mseccfg(attempt);

  // Read back and verify RLB stayed cleared; other bits (e.g. MMWP) should
  // remain as expected.
  uint32_t m2 = read_mseccfg();
  CHECK((m2 & EPMP_MSECCFG_RLB) == 0);
  CHECK(m2 & EPMP_MSECCFG_MMWP);
}



void rom_main(void) {
  // Initialize global variables here so that they don't end up in the .data
  // section since OpenTitan ROM does not have one.
  passed = true;
  exception_received = kIbexExcMax;


  // Initialize sec_mmio.
  sec_mmio_init();

  // Configure debug ROM ePMP entry.
  rom_epmp_config_debug_rom(kLcStateProd);

  // Initialize pinmux configuration so we can use the UART.
  dif_pinmux_t pinmux;
  OT_DISCARD(dif_pinmux_init(
      mmio_region_from_addr(TOP_EARLGREY_PINMUX_AON_BASE_ADDR), &pinmux));
  pinmux_testutils_init(&pinmux);
 
  // Enable execution of code in flash.
  flash_ctrl_init();
  flash_ctrl_exec_set(FLASH_CTRL_PARAM_EXEC_EN);
  SEC_MMIO_WRITE_INCREMENT(kFlashCtrlSecMmioInit + kFlashCtrlSecMmioExecSet);
 
  // Configure UART0 as stdout.
  uart_init(kUartNCOValue);
  base_set_stdout((buffer_sink_t){
      .data = NULL,
      .sink = uart_sink,
  });
  //Disable watchdog, if not test resets after some time
  abs_mmio_write32(TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);

  #ifdef DEBUG
  //dump_pmp_min();
  dump_reset_info();
  #endif
  // Start the tests.
  LOG_INFO("Starting RLB Test");

  // Initialize shadow copy of the ePMP register configuration.
  memset(&epmp_state, 0, sizeof(epmp_state));
  rom_epmp_state_init(kLcStateProd);
  //CHECK(epmp_state_check() == kErrorOk);

  //Print RLB = 1, MMWP = 1, MML = 0
  uint32_t mseccfg_init = read_mseccfg();
  LOG_INFO("Debug: Initial MSECCFG=0x%08x (MMWP=%u RLB=%u MML=%u)",
    mseccfg_init, !!(mseccfg_init & EPMP_MSECCFG_MMWP),
    !!(mseccfg_init & EPMP_MSECCFG_RLB),
    !!(mseccfg_init & EPMP_MSECCFG_MML)); 
  LOG_INFO("Setup: eFLASH execution and unlock with L=1");
  test_unlock_exec_eflash();
  
  LOG_INFO("1) Testing eFLASH re-lock execution. RLB == 1 should allow us.");
  test_lock_exec_eflash();
  LOG_INFO("1) Passed.");

  LOG_INFO("2) Testing RLB one to zero transition");
  test_rlb_one_to_zero();
  LOG_INFO("2) Passed."); 
  LOG_INFO("3) Testing ROM_EXT can't re-lock execution. RLB == 0 should prevent it.");
  test_rom_ext_cant_reunlock_exec();
  LOG_INFO("3) Passed.");

  LOG_INFO("4) Testing RLB can't go zero-to-one. Should not be allowed since it is a");
  LOG_INFO("4) sticky bit once set to 0.");
  test_cant_rlb_zero_to_one();
  LOG_INFO("4) Passed.");

  // The test of the ROM's ePMP configuration is now complete. Unlock the
  // DV address space so that the test result can be reported. Assumes that PMP
  // entry 6 is allocated for this purpose.
  CHECK(epmp_unlock_test_status());

  // Report the test status.
  LOG_INFO("All tests %s", passed ? "PASSED" : "FAILED");
  test_status_set(kTestStatusInTest);
  test_status_set(passed ? kTestStatusPassed : kTestStatusFailed);

  // Unreachable if reporting the test status correctly caused the
  // test to stop.
  while (true) {
    wait_for_interrupt();
  }
}
