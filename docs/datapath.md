# ENA Data Plane and Packet Processing Specification

## 1. IO Queue Architecture

The ENA datapath uses paired rings:
- **TX Submission Queue (TX SQ)**: Host submits packet descriptors to device.
- **TX Completion Queue (TX CQ)**: Device writes completed packet indices back to host.
- **RX Submission Queue (RX SQ)**: Host submits empty `uk_netbuf` buffers to device.
- **RX Completion Queue (RX CQ)**: Device reports received packet buffers and metadata.

---

## 2. Transmit (TX) Datapath

### 2.1 Standard Host-Memory TX Mode
1. Unikraft calls `uk_netdev_tx_one` or `uk_netdev_txq_xmit`.
2. Driver reads the packet from `struct uk_netbuf`.
3. Driver populates `ena_eth_io_tx_desc`:
   - Physical address of buffer payload (`paddr`).
   - Length of packet in bytes.
   - Flags: `FIRST`, `LAST`, `COMPL_REQ`, Checksum offload bits.
4. Driver advances the TX SQ tail and writes to the TX doorbell register.
5. On completion polling/interrupt:
   - Driver reads `ena_eth_io_tx_cdesc` from TX CQ.
   - Driver frees or recycles the corresponding `uk_netbuf`.

### 2.2 Low Latency Queue (LLQ) Push Mode
- Used on EC2 instances with BAR2 support.
- TX descriptor and packet header (up to configured inline size) are written directly to BAR2 MMIO.
- Remainder of packet body is referenced via DMA address.
- Bypasses PCIe read transactions by device, reducing transmit latency.

---

## 3. Receive (RX) Datapath

### 3.1 Buffer Allocation & SQ Replenishment
1. During queue setup, allocate `N` network buffers (`uk_netbuf`).
2. Write physical addresses to `ena_eth_io_rx_desc` in the RX SQ.
3. Advance the RX SQ tail and write the RX doorbell register.

### 3.2 Packet Intake
1. Driver checks the `phase` bit of `ena_eth_io_rx_cdesc` in the RX CQ.
2. When the phase bit matches the current cycle:
   - Extract packet length and status flags (checksum status, packet hash).
   - Retrieve the stored `struct uk_netbuf` reference.
   - Adjust `netbuf->len` to match received size.
   - Pass `uk_netbuf` to Unikraft network stack (`uknetdev` callback).
   - Allocate replacement `uk_netbuf` and submit back to RX SQ.
3. Increment CQ head pointer and toggle phase bit on ring wrap.

---

## 4. Offloading Capabilities

The native ENA driver supports hardware offloads:
- **TX Checksum Offload**: IPv4 checksum, TCP/UDP checksum over IPv4/IPv6.
- **RX Checksum Validation**: Hardware validates L3/L4 checksum and sets flags in CQ descriptor.
- **Large Receive Offload (LRO)**: Not implemented. The driver passes each received segment to the stack.
- **Scatter-Gather (SG)**: Not implemented. Each packet uses one descriptor. See Section 5.

---

## 5. Scatter-Gather (SG) Status and Roadmap

### 5.1 Current Implementation Status
The driver currently implements single-descriptor transmit and receive processing. Each packet maps to exactly one buffer descriptor in the Submission Queue.

### 5.2 Technical Implications
- **Jumbo Frames**: TX sends jumbo frames from one contiguous buffer of at least 9000 bytes. RX cannot receive them. Each RX descriptor holds one 2048-byte buffer (`ENA_RX_BUF_SIZE`). A completion longer than 2048 bytes is dropped, and the ring keeps working.
- **TCP Segmentation Offload (TSO)**: TSO is not active without multi-descriptor chain assembly.
- **Buffer Allocation**: RX replenishment allocates single contiguous `uk_netbuf` instances of `ENA_RX_BUF_SIZE` (2048 bytes).

### 5.3 Roadmap for Scatter-Gather Support
1. **Multi-Descriptor Submission**: Update `ena_tx_submit` to break multi-fragment `uk_netbuf` chains across consecutive SQ descriptors with `FIRST` and `LAST` flags.
2. **Scatter RX Reassembly**: Track chained RX completions across descriptors before dispatching full packets to Unikraft netdev callbacks.
3. **TSO Integration**: Implement large packet header segmentation and metadata population in ENA TX descriptors.
