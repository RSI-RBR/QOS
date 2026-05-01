# Networking Bring-up Notes

This repository now includes a minimal network core and NIC abstraction:

- `include/nic.h` driver interface
- `include/net.h` raw send/recv API
- `include/ethernet.h` Ethernet header helpers
- `kernel/net.c` queueing/stats/core logic
- `kernel/nic_stub.c` loopback NIC (for protocol testing without hardware)
- `include/usb_host.h`, `kernel/usb_host.c` DWC2 host phase-1 bring-up scaffold

## Current behavior

- `net_init()` selects `stub-loopback` backend.
- `net_send_raw()` transmits to NIC backend.
- Stub backend feeds TX frames back into RX path on `net_poll()`.
- `net_recv_raw()` returns queued raw frames.
- Timer IRQ calls `net_poll()` so receive progress is continuous.

## Replacing stub with real NIC

Implement a new driver module that satisfies `nic_driver_t`:

1. `init()`
   - reset MAC
   - configure PHY (or MDIO attach)
   - allocate/init RX and TX descriptor rings
   - configure DMA engine
   - enable MAC RX/TX
   - enable NIC interrupt sources

2. `set_rx_handler()`
   - store callback from net core
   - invoke callback for each fully received frame

3. `send()`
   - copy frame into TX DMA buffer
   - post TX descriptor
   - kick TX DMA

4. `poll()`
   - reclaim completed TX descriptors
   - drain RX descriptors
   - call RX handler with each frame
   - re-arm RX descriptors

5. `link_up()`
   - return PHY link state

Then switch `nic_probe_default()` to return your hardware driver.

## USB (Pi 3 onboard Ethernet path) status

Phase 1 is now in place:

- DWC2 core ID probe
- Core reset
- Forced host mode
- Root port power
- Status dump (`HPRT0/HCFG/GINTSTS/PCGCTL`)

Next step (Phase 2) is EP0 control transfer support:

1. Host-channel allocation for control endpoint
2. `SETUP` stage transfer
3. optional `DATA` stage (IN/OUT)
4. `STATUS` stage
5. read device descriptor for root-port device

## IRQ hook

`interrupt_register_bank2_irq(mask, handler)` is available to register additional
GPU interrupt-bank-2 handlers (same path SDHOST uses).

For NICs with separate interrupt controllers, keep IRQ ack/dispatch in your
driver and call into RX/TX drain logic from handler or from `poll()`.

## Suggested next protocol order

1. Ethernet frame parser
2. ARP cache + request/reply
3. IPv4 parse/build
4. ICMP echo reply (ping)
5. UDP sockets
