# Note 4 STM FTS touch: no IRQs under mainline (2026-11-22)

## Error 0x620009
stmfts prints event[6..1] as unpadded %x, so "620009" = ev[6]=6 ev[5]=2 ev[4]=0 ev[3]=0 ev[2]=0 ev[1]=0x09.
Downstream ds-fts_ts.c:962-971: EVENTID_ERROR (0x0F) with byte1 0x09 = "Fail detect SYNC"
(byte1 0x08 = autotune fail, byte2 0 mutual / 1 self).
=> The FTS sees no display HSYNC. It scans locked to the OLED DDI's touch HSYNC output.

## DDI side: what drives the sync
Downstream panel driver (universal5433 lineage-18.1, drivers/video/exynos/panels/):
- s6e3ha2_param.h:156-164
    SEQ_TOUCH_HSYNC_ON_ID2_00 = BD 33 11 02 16 02 16
    SEQ_TOUCH_HSYNC_ON        = BD 11 11 02 16 02 16
- s6e3ha2_mipi_lcd.c:1174-1179 (ldi_init, under F0 5A 5A + FC 5A 5A, after reading the ID):
    if (lcd->id[1] == 0) BD 33 ... else BD 11 ...
Mainline panel-samsung-s6e3ha2.c:353-357 always sends BD 33 ... (the TM2 / ID2=00 variant).
Downstream fts_ts.c (fts_get_panel_revision comment) shows a retail Note 4 OCTA ID of 40 81 45, so id[1]=0x81 != 0, which needs BD 11.

## FTS-side sync config (flash config, written only on downstream FW update)
fts_fwu.c:200-218 fts_set_octa_type: B0 00 C8 07/03 (TA gpio), B0 07 0A 59/49 (SYNC), then FB save.
Read the sync register: write B2 07 0A 04, then read 0x85 until event 0x12 (fts_fwu.c:374).

## Downstream init order (fts_init, ds-fts_ts.c:694-788)
B6 00 23 01 (reset), 10ms, wait for event 0x10 on 0x85 -> AA release info -> chip id B6 00 07 ->
FW update check -> 91 SLEEPOUT -> 93 SENSEON -> (9B key sense only if mskey) -> A2 force cal ->
A1 flush -> B6 00 1C 41 int enable -> read 84.
The reinit path on resume (ds-fts_ts.c:2298-2365) does the same with 50ms after 91 and after 93, and no A2.
It does NOT run A3/A4 autotune at every boot; that is only in fts_fw_init after a flash, followed by FC save.
IRQ: read B6 00 45 (count = val>>10), then 0x86 * count*8. No ack write is needed.

## IRQ/pin
stm,irq_type = <8200> = 0x2008 = IRQF_ONESHOT | IRQF_TRIGGER_LOW, the same as mainline LEVEL_LOW.
attn_irq: gpa1-1 func 0xf (EINT), pud 0 (none), drv 0. Mainline has no pinctrl node (pud left at reset default).
