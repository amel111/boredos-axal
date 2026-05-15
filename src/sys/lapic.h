// Copyright (c) 2023-2026 azzuhry (amel111)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

// IPI vector used for scheduling on APs
#define IPI_SCHED_VECTOR 0x41

// AXAL: IPI vector for TLB shootdown
#define IPI_TLB_SHOOTDOWN_VECTOR 0x42

// Initialize LAPIC access (maps registers via HHDM)
void lapic_init(void);

// Enable LAPIC (set SVR bit 8)
void lapic_enable(void);

// Send End-of-Interrupt to the local APIC
void lapic_eoi(void);

// Send a scheduling IPI to all APs (excludes self)
void lapic_send_ipi_all(void);

// AXAL: Send TLB shootdown IPI to all other CPUs
void lapic_send_tlb_shootdown(void);

#endif
