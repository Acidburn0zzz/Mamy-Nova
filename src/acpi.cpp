/*
 * Advanced Configuration and Power Interface (ACPI)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of the NOVA microhypervisor.
 *
 * NOVA is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOVA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 */

#include "acpi.h"
#include "acpi_dmar.h"
#include "acpi_fadt.h"
#include "acpi_madt.h"
#include "acpi_mcfg.h"
#include "acpi_rsdp.h"
#include "acpi_rsdt.h"
#include "assert.h"
#include "bits.h"
#include "gsi.h"
#include "hpt.h"
#include "io.h"
#include "stdio.h"
#include "x86.h"

Paddr Acpi::dmar, Acpi::fadt, Acpi::madt, Acpi::mcfg, Acpi::rsdt, Acpi::xsdt;

Acpi_gas Acpi::pm1a_sts;
Acpi_gas Acpi::pm1b_sts;
Acpi_gas Acpi::pm1a_ena;
Acpi_gas Acpi::pm1b_ena;
Acpi_gas Acpi::pm1a_cnt;
Acpi_gas Acpi::pm1b_cnt;
Acpi_gas Acpi::pm2_cnt;
Acpi_gas Acpi::pm_tmr;
Acpi_gas Acpi::reset_reg;

uint32      Acpi::tmr_ovf;
uint32      Acpi::feature;
uint32      Acpi::smi_cmd;
uint8       Acpi::enable_val;
uint8       Acpi::reset_val;

unsigned    Acpi::gsi;
unsigned    Acpi::irq;

bool        Acpi_table_madt::sci_overridden = false;

void Acpi::setup_sci()
{
    gsi = Gsi::irq_to_gsi (irq);

    if (!Acpi_table_madt::sci_overridden) {
        Acpi_intr sci_override;
        sci_override.bus = 0;
        sci_override.irq = static_cast<uint8>(irq);
        sci_override.gsi = gsi;
        sci_override.flags.pol = Acpi_inti::POL_CONFORMING;
        sci_override.flags.trg = Acpi_inti::TRG_CONFORMING;
        Acpi_table_madt::parse_intr (&sci_override);
    }

    Gsi::set (gsi);

    trace (TRACE_ACPI, "ACPI: GSI:%#x TMR:%lu", gsi, tmr_msb() + 1);
}

void Acpi::enable()
{
    setup_sci();

    if (smi_cmd && enable_val) {
        Io::out (smi_cmd, enable_val);
        while (!(read (PM1_CNT) & PM1_CNT_SCI_EN))
            pause();
    }

    write (PM1_ENA, PM1_ENA_PWRBTN | PM1_ENA_GBL | PM1_ENA_TMR);

    for (; tmr_ovf = read (PM_TMR) >> tmr_msb(), read (PM1_STS) & PM1_STS_TMR; write (PM1_STS, PM1_STS_TMR)) ;
}

void Acpi::delay (unsigned ms)
{
    unsigned cnt = timer_frequency * ms / 1000;
    unsigned val = read (PM_TMR);

    while ((read (PM_TMR) - val) % (1UL << 24) < cnt)
        pause();
}

uint64 Acpi::time()
{
    uint32 dummy;
    mword b = tmr_msb(), c = read (PM_TMR), p = 1UL << b;
    return div64 (1000000 * ((tmr_ovf + ((c >> b ^ tmr_ovf) & 1)) * static_cast<uint64>(p) + (c & (p - 1))), timer_frequency, &dummy);
}

void Acpi::reset()
{
    write (RESET, reset_val);
}

void Acpi::setup()
{
    Acpi_rsdp::parse();

    if (xsdt)
        static_cast<Acpi_table_rsdt *>(Hpt::remap (xsdt))->parse (xsdt, sizeof (uint64));
    else if (rsdt)
        static_cast<Acpi_table_rsdt *>(Hpt::remap (rsdt))->parse (rsdt, sizeof (uint32));

    if (fadt)
        static_cast<Acpi_table_fadt *>(Hpt::remap (fadt))->parse();
    if (madt)
        static_cast<Acpi_table_madt *>(Hpt::remap (madt))->parse();
    if (mcfg)
        static_cast<Acpi_table_mcfg *>(Hpt::remap (mcfg))->parse();
    if (dmar)
        static_cast<Acpi_table_dmar *>(Hpt::remap (dmar))->parse();

    Gsi::init();

    enable();
}

unsigned Acpi::read (Register reg)
{
    switch (reg) {
        case PM1_STS:
            return hw_read (&pm1a_sts) | hw_read (&pm1b_sts);
        case PM1_ENA:
            return hw_read (&pm1a_ena) | hw_read (&pm1b_ena);
        case PM1_CNT:
            return hw_read (&pm1a_cnt) | hw_read (&pm1b_cnt);
        case PM2_CNT:
            return hw_read (&pm2_cnt);
        case PM_TMR:
            return hw_read (&pm_tmr);
        case RESET:
            break;
    }

    return 0;
}

void Acpi::write (Register reg, unsigned val)
{
    // XXX: Spec requires that certain bits be preserved.

    switch (reg) {
        case PM1_STS:
            hw_write (&pm1a_sts, val);
            hw_write (&pm1b_sts, val);
            break;
        case PM1_ENA:
            hw_write (&pm1a_ena, val);
            hw_write (&pm1b_ena, val);
            break;
        case PM1_CNT:
            hw_write (&pm1a_cnt, val);
            hw_write (&pm1b_cnt, val);
            break;
        case PM2_CNT:
            hw_write (&pm2_cnt, val);
            break;
        case PM_TMR:                    // read-only
            break;
        case RESET:
            hw_write (&reset_reg, val);
            break;
    }
}

unsigned Acpi::hw_read (Acpi_gas *gas)
{
    if (!gas->bits)     // Register not implemented
        return 0;

    if (gas->asid == Acpi_gas::IO) {
        switch (gas->bits) {
            case 8:
                return Io::in<uint8>(static_cast<unsigned>(gas->addr));
            case 16:
                return Io::in<uint16>(static_cast<unsigned>(gas->addr));
            case 32:
                return Io::in<uint32>(static_cast<unsigned>(gas->addr));
        }
    }

    panic ("Unimplemented ASID %u\n", gas->asid);
}

void Acpi::hw_write (Acpi_gas *gas, unsigned val)
{
    if (!gas->bits)     // Register not implemented
        return;

    if (gas->asid == Acpi_gas::IO) {
        switch (gas->bits) {
            case 8:
                Io::out (static_cast<unsigned>(gas->addr), static_cast<uint8>(val));
                return;
            case 16:
                Io::out (static_cast<unsigned>(gas->addr), static_cast<uint16>(val));
                return;
            case 32:
                Io::out (static_cast<unsigned>(gas->addr), static_cast<uint32>(val));
                return;
        }
    }

    panic ("Unimplemented ASID %u\n", gas->asid);
}

void Acpi::interrupt()
{
    unsigned sts = read (PM1_STS);

    if (sts & PM1_STS_TMR)
        tmr_ovf++;

    write (PM1_STS, sts);
}
