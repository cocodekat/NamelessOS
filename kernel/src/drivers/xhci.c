#include <stdint.h>
#include <stddef.h>
#include <memory.h>
#include "xhci.h"
#include "../pci.h"
#include "../mm.h"
#include "../heap.h"
#include "../serial.h"
#include "../commands.h"

// ---------------------------------------------------------------------
// TRB and ring plumbing
// ---------------------------------------------------------------------

typedef struct
{
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP_STAGE 2
#define TRB_TYPE_DATA_STAGE 3
#define TRB_TYPE_STATUS_STAGE 4
#define TRB_TYPE_LINK 6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_DISABLE_SLOT 10
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIGURE_ENDPOINT 12
#define TRB_TYPE_NOOP_COMMAND 23
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_COMMAND_COMPLETION_EVENT 33
#define TRB_TYPE_PORT_STATUS_CHANGE_EVENT 34

#define RING_SIZE 64 // 63 usable TRBs + 1 Link TRB

typedef struct
{
    xhci_trb_t trbs[RING_SIZE] __attribute__((aligned(64)));
    int enqueue_index;
    int cycle_bit;
} xhci_ring_t;

static void ring_init(xhci_ring_t *ring)
{
    memset(ring->trbs, 0, sizeof(ring->trbs));
    ring->enqueue_index = 0;
    ring->cycle_bit = 1;
}

// Pushes one TRB, patching in the ring's current producer cycle bit, and
// handles wraparound via a Link TRB in the ring's last slot (standard
// xHCI ring convention -- the controller follows the Link TRB back to
// slot 0 rather than falling off the end of the array).
static void ring_push(xhci_ring_t *ring, uint64_t parameter, uint32_t status, uint32_t control)
{
    control = (control & ~1u) | (uint32_t)(ring->cycle_bit & 1);

    ring->trbs[ring->enqueue_index].parameter = parameter;
    ring->trbs[ring->enqueue_index].status = status;
    ring->trbs[ring->enqueue_index].control = control;

    ring->enqueue_index++;
    if (ring->enqueue_index == RING_SIZE - 1)
    {
        xhci_trb_t *link = &ring->trbs[RING_SIZE - 1];
        link->parameter = virt_to_phys(&ring->trbs[0]);
        link->status = 0;
        link->control = (uint32_t)((TRB_TYPE_LINK << 10) | (1u << 1) | (ring->cycle_bit & 1)); // Toggle Cycle
        ring->enqueue_index = 0;
        ring->cycle_bit ^= 1;
    }
}

// ---------------------------------------------------------------------
// MMIO register access
// ---------------------------------------------------------------------

static inline uint32_t reg32(volatile void *addr) { return *(volatile uint32_t *)addr; }
static inline void set_reg32(volatile void *addr, uint32_t v) { *(volatile uint32_t *)addr = v; }
static inline uint64_t reg64(volatile void *addr) { return *(volatile uint64_t *)addr; }
static inline void set_reg64(volatile void *addr, uint64_t v) { *(volatile uint64_t *)addr = v; }

// Capability register offsets (from mmio base)
#define CAP_CAPLENGTH 0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18

// Operational register offsets (from op base = mmio base + CAPLENGTH)
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_PAGESIZE 0x08
#define OP_DNCTRL 0x14
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(n) (0x400 + 0x10 * ((n) - 1)) // n is 1-based

#define USBCMD_RUN (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH (1u << 0)
#define USBSTS_CNR (1u << 11)

#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_PR (1u << 4)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK (0xFu << PORTSC_SPEED_SHIFT)
#define PORTSC_CSC (1u << 17)
#define PORTSC_PEC (1u << 18)
#define PORTSC_WRC (1u << 19)
#define PORTSC_OCC (1u << 20)
#define PORTSC_PRC (1u << 21)
#define PORTSC_PLC (1u << 22)
#define PORTSC_CEC (1u << 23)
#define PORTSC_RW1C_ALL (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

// Reads current value and strips bits that would have a side effect if
// blindly written back (RW1C status bits, and PED which *disables* the
// port if you write 1 to it). Callers OR in exactly the one bit they
// mean to change.
static uint32_t portsc_safe_base(uint32_t current)
{
    return current & ~(PORTSC_RW1C_ALL | PORTSC_PED);
}

// Runtime register offsets (from runtime base = mmio base + RTSOFF)
#define RT_IR0 0x20
#define IR_IMAN 0x00
#define IR_ERSTSZ 0x08
#define IR_ERSTBA 0x10
#define IR_ERDP 0x18

typedef struct
{
    uint64_t ring_segment_base;
    uint32_t ring_segment_size; // low 16 bits used
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

// ---------------------------------------------------------------------
// Device/Input Context helpers (variable context size: 32 or 64 bytes,
// per HCCPARAMS1.CSZ -- real hardware disagrees on this, so both are
// supported rather than assumed)
// ---------------------------------------------------------------------

static int g_context_size = 32; // set during init from HCCPARAMS1 bit 2

static uint32_t *ctx_slot(void *blob, int has_input_control)
{
    uint8_t *base = (uint8_t *)blob;
    if (has_input_control)
        base += g_context_size;
    return (uint32_t *)base;
}

static uint32_t *ctx_ep(void *blob, int has_input_control, int dci)
{
    uint8_t *base = (uint8_t *)blob;
    if (has_input_control)
        base += g_context_size;
    base += g_context_size; // skip slot context
    base += (size_t)(dci - 1) * g_context_size;
    return (uint32_t *)base;
}

static uint32_t *ctx_input_control(void *blob)
{
    return (uint32_t *)blob;
}

// Generously sized so it works regardless of context_size (32 or 64) --
// enough room for Input Control Context + Slot Context + 2 Endpoint
// Contexts (EP0 and one interrupt IN endpoint), at the larger size.
#define INPUT_CONTEXT_SIZE (33 * 64)
#define DEVICE_CONTEXT_SIZE (32 * 64)

static uint8_t g_input_ctx[INPUT_CONTEXT_SIZE] __attribute__((aligned(64)));
static uint8_t g_device_ctx[DEVICE_CONTEXT_SIZE] __attribute__((aligned(64)));

// ---------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------

static volatile uint8_t *g_mmio = NULL;
static volatile uint8_t *g_op = NULL;
static volatile uint8_t *g_rt = NULL;
static volatile uint32_t *g_doorbells = NULL;

static uint32_t g_max_slots = 0;
static uint32_t g_max_ports = 0;

static uint64_t *g_dcbaa = NULL; // Device Context Base Address Array
static xhci_ring_t g_cmd_ring;
static xhci_ring_t g_ep0_ring;
static xhci_ring_t g_intr_ring;

static volatile xhci_trb_t g_event_ring[RING_SIZE] __attribute__((aligned(64)));
static xhci_erst_entry_t g_erst[1] __attribute__((aligned(64)));
static int g_event_dequeue = 0;
static int g_event_ccs = 1; // consumer cycle state

static uint8_t g_slot_id = 0;
static int g_ready = 0;
static int g_ep0_max_packet = 8;
static int g_intr_endpoint_num = 1; // overwritten once we parse the endpoint descriptor
static int g_report_len = 8;

// DMA buffers -- all inside our own kernel image/BSS so virt_to_phys() works.
static uint8_t g_xfer_buf[512] __attribute__((aligned(64)));
static uint8_t g_report_buf[8] __attribute__((aligned(64)));
static uint8_t g_prev_report[8];

// ---------------------------------------------------------------------
// Event ring consumption
// ---------------------------------------------------------------------

static void event_ring_advance_erdp(void)
{
    uint64_t phys = virt_to_phys(&g_event_ring[g_event_dequeue]);
    volatile uint8_t *erdp_reg = g_rt + RT_IR0 + IR_ERDP;
    set_reg64((void *)erdp_reg, (phys & ~0xFull) | (1u << 3)); // bit3 EHB: write 1 to clear
}

// Returns 1 and fills *trb if an event is waiting; consumes it (advances
// the dequeue pointer) either way nothing is left half-read. Returns 0
// if the ring is empty right now.
static int event_ring_pop(xhci_trb_t *out)
{
    volatile xhci_trb_t *trb = &g_event_ring[g_event_dequeue];
    if ((trb->control & 1u) != (uint32_t)g_event_ccs)
    {
        return 0; // cycle bit doesn't match what we expect next -- nothing new
    }

    *out = *trb;

    g_event_dequeue++;
    if (g_event_dequeue == RING_SIZE)
    {
        g_event_dequeue = 0;
        g_event_ccs ^= 1;
    }
    event_ring_advance_erdp();
    return 1;
}

// Spins on the event ring until it sees a Command Completion Event, up to
// a bounded number of polls (we have no timer here, so "timeout" just
// means "gave up spinning" -- on real hardware a stuck command usually
// means a bad Input Context, not a slow controller).
#define COMMAND_POLL_LIMIT 250000000

static int wait_command_completion(uint8_t *out_completion_code,
                                   uint8_t *out_slot_id)
{
    xhci_trb_t trb;

    for (uint32_t spins = 0; spins < COMMAND_POLL_LIMIT; spins++)
    {
        if (!event_ring_pop(&trb))
            continue;

        uint32_t trb_type = (trb.control >> 10) & 0x3F;
        uint8_t completion_code = (uint8_t)((trb.status >> 24) & 0xFF);
        uint8_t slot_id = (uint8_t)((trb.control >> 24) & 0xFF);

        serial_print("[xhci] event type=");
        serial_print_uint(trb_type);

        serial_print(" cc=");
        serial_print_uint(completion_code);

        serial_print(" slot=");
        serial_print_uint(slot_id);

        serial_print("\n");

        if (trb_type == TRB_TYPE_COMMAND_COMPLETION_EVENT)
        {
            *out_completion_code = completion_code;
            *out_slot_id = slot_id;
            return 1;
        }
    }

    return 0;
}

static void ring_doorbell(uint32_t index, uint32_t target)
{
    asm volatile("mfence" ::: "memory");
    g_doorbells[index] = target;
    (void)g_doorbells[index];
}

// ---------------------------------------------------------------------
// Command ring helpers
// ---------------------------------------------------------------------

static int cmd_enable_slot(uint8_t *out_slot_id)
{
    ring_push(&g_cmd_ring, 0, 0, (uint32_t)(TRB_TYPE_ENABLE_SLOT << 10));
    ring_doorbell(0, 0);

    uint8_t code, slot;
    if (!wait_command_completion(&code, &slot))
    {
        serial_print("[xhci] enable slot: timed out\n");
        return 0;
    }
    if (code != 1)
    {
        serial_print("[xhci] enable slot: completion code ");
        serial_print_uint(code);
        serial_print("\n");
        return 0;
    }
    *out_slot_id = slot;
    return 1;
}

// Frees a slot we enabled but decided not to use (e.g. it turned out not
// to be a keyboard), so the controller can hand it back out to whatever
// port we try next instead of leaking it for good.
static void cmd_disable_slot(uint8_t slot_id)
{
    ring_push(&g_cmd_ring, 0, 0,
              (uint32_t)((TRB_TYPE_DISABLE_SLOT << 10) |
                         ((uint32_t)slot_id << 24)));

    ring_doorbell(0, 0);

    uint8_t code, slot;
    wait_command_completion(&code, &slot); // best-effort
}

static int cmd_address_device(uint8_t slot_id, void *input_ctx, int bsr)
{
    uint64_t phys = virt_to_phys(input_ctx);

    ring_push(&g_cmd_ring, phys, 0,
              (uint32_t)((TRB_TYPE_ADDRESS_DEVICE << 10) |
                         ((uint32_t)slot_id << 24) |
                         (bsr ? (1u << 9) : 0)));

    xhci_trb_t *trb =
        &g_cmd_ring.trbs[(g_cmd_ring.enqueue_index + RING_SIZE - 1) % RING_SIZE];

    serial_print("[xhci] ADDRESS DEVICE TRB\n");
    serial_print("  param=");
    serial_print_hex64(trb->parameter);
    serial_print("\n");

    serial_print("  status=");
    serial_print_hex64(trb->status);
    serial_print("\n");

    serial_print("  control=");
    serial_print_hex64(trb->control);
    serial_print("\n");

    serial_print("  input ctx phys=");
    serial_print_hex64(phys);
    serial_print("\n");
    serial_print("[xhci] command ring enqueue index=");
    serial_print_uint((unsigned int)g_cmd_ring.enqueue_index);
    serial_print(" cycle=");
    serial_print_uint((unsigned int)g_cmd_ring.cycle_bit);
    serial_print("\n");

    serial_print("[xhci] CMD TRB phys=");
    serial_print_hex64(
        virt_to_phys(&g_cmd_ring.trbs[(g_cmd_ring.enqueue_index + RING_SIZE - 1) % RING_SIZE]));
    serial_print("\n");
    ring_doorbell(0, 0);

    uint8_t code, slot;
    if (!wait_command_completion(&code, &slot))
    {
        serial_print("[xhci] NO COMMAND COMPLETION EVENT SEEN\n");

        uint32_t crcr_now = reg64(g_op + OP_CRCR);
        serial_print("[xhci] CRCR now=");
        serial_print_hex64(crcr_now);
        serial_print("\n");

        uint32_t sts = reg32(g_op + OP_USBSTS);
        uint32_t cmd = reg32(g_op + OP_USBCMD);
        uint64_t crcr = reg64(g_op + OP_CRCR);
        uint64_t dcbaap = reg64(g_op + OP_DCBAAP);

        serial_print("[xhci] address device: timed out\n");

        serial_print("[xhci] USBSTS=");
        serial_print_hex64(sts);
        serial_print("\n");

        serial_print("[xhci] USBCMD=");
        serial_print_hex64(cmd);
        serial_print("\n");

        serial_print("[xhci] CRCR=");
        serial_print_hex64(crcr);
        serial_print("\n");

        serial_print("[xhci] DCBAAP=");
        serial_print_hex64(dcbaap);
        serial_print("\n");

        return 0;
    }

    if (code != 1)
    {
        serial_print("[xhci] address device: completion code ");
        serial_print_uint(code);
        serial_print("\n");
        return 0;
    }
    return 1;
}

static int cmd_configure_endpoint(uint8_t slot_id, void *input_ctx)
{
    uint64_t phys = virt_to_phys(input_ctx);
    ring_push(&g_cmd_ring, phys, 0, (uint32_t)((TRB_TYPE_CONFIGURE_ENDPOINT << 10) | ((uint32_t)slot_id << 24)));
    ring_doorbell(0, 0);

    uint8_t code, slot;
    if (!wait_command_completion(&code, &slot))
    {
        serial_print("[xhci] configure endpoint: timed out\n");
        return 0;
    }
    if (code != 1)
    {
        serial_print("[xhci] configure endpoint: completion code ");
        serial_print_uint(code);
        serial_print("\n");
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------
// Control transfers on EP0 (DCI 1)
// ---------------------------------------------------------------------

// Spins for a Transfer Event on EP0's ring specifically. Same caveat as
// wait_command_completion regarding the lack of a real timeout.
static int wait_ep0_transfer(uint8_t *out_completion_code)
{
    xhci_trb_t trb;

    for (uint32_t spins = 0; spins < COMMAND_POLL_LIMIT; spins++)
    {
        if (event_ring_pop(&trb))
        {
            uint32_t trb_type = (trb.control >> 10) & 0x3F;
            uint8_t cc = (uint8_t)((trb.status >> 24) & 0xFF);

            serial_print("[xhci] EP0 EVENT type=");
            serial_print_uint(trb_type);
            serial_print(" cc=");
            serial_print_uint(cc);
            serial_print("\n");

            if (trb_type == TRB_TYPE_TRANSFER_EVENT)
            {
                *out_completion_code = cc;
                return 1;
            }
        }
    }

    return 0;
}

// Issues one control transfer: Setup Stage, optional Data Stage, Status
// Stage. `data` may be NULL if wLength is 0. Returns 1 on success
// (completion code Success=1 or Short Packet=13, both fine for a read
// that returned less than the buffer size).
static int control_transfer(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                            uint16_t wIndex, uint16_t wLength, void *data, int data_in)
{
    uint64_t setup_param = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) | ((uint64_t)wValue << 16) | ((uint64_t)wIndex << 32) | ((uint64_t)wLength << 48);

    uint32_t trt = (wLength == 0) ? 0 : (data_in ? 3 : 2); // 0=No Data,2=OUT,3=IN
    uint32_t ep0_start = g_ep0_ring.enqueue_index;
    // IDT (Immediate Data, bit 6) is mandatory on a Setup Stage TRB: it
    // tells the controller that the Setup packet lives inline in the
    // TRB's Parameter field rather than being a pointer to fetch via
    // DMA. Without it, the controller tries to DMA-read the setup
    // packet from what is actually just our bit-packed request fields
    // misread as a physical address -- the transfer then just silently
    // never completes (no event, no error, forever).
    ring_push(&g_ep0_ring, setup_param, 8,
              (uint32_t)((TRB_TYPE_SETUP_STAGE << 10) |
                         (trt << 16) |
                         (1u << 6)));

    if (wLength > 0)
    {
        uint64_t phys = virt_to_phys(data);
        uint32_t dir_bit = data_in ? (1u << 16) : 0;

        ring_push(&g_ep0_ring, phys, wLength,
                  (uint32_t)((TRB_TYPE_DATA_STAGE << 10) |
                             dir_bit));
    }

    uint32_t status_dir_bit = (wLength > 0 && data_in) ? 0 : (1u << 16);

    ring_push(&g_ep0_ring, 0, 0,
              (uint32_t)((TRB_TYPE_STATUS_STAGE << 10) |
                         status_dir_bit |
                         (1u << 5)));

    serial_print("[xhci] EP0 TRBs before doorbell\n");
    for (int i = 0; i < 3; i++)
    {
        uint32_t idx = ep0_start + (uint32_t)i;
        if (idx >= RING_SIZE - 1)
            idx -= RING_SIZE - 1;

        serial_print("[xhci] TRB ");
        serial_print_uint(i);
        serial_print(" param=");
        serial_print_hex64(g_ep0_ring.trbs[idx].parameter);
        serial_print(" status=");
        serial_print_hex64(g_ep0_ring.trbs[idx].status);
        serial_print(" control=");
        serial_print_hex64(g_ep0_ring.trbs[idx].control);
        serial_print("\n");
    }
    ring_doorbell(g_slot_id, 1); // DCI 1 = control endpoint

    uint8_t code;
    if (!wait_ep0_transfer(&code))
    {
        serial_print("[xhci] control transfer: timed out\n");

        serial_print("[xhci] OUTPUT EP0 CONTEXT after timeout\n");
        uint64_t *dc = (uint64_t *)g_device_ctx;

        serial_print("[xhci] word 4 = ");
        serial_print_hex64(dc[4]);
        serial_print("\n");

        serial_print("[xhci] word 5 = ");
        serial_print_hex64(dc[5]);
        serial_print("\n");

        serial_print("[xhci] word 6 = ");
        serial_print_hex64(dc[6]);
        serial_print("\n");

        serial_print("[xhci] word 7 = ");
        serial_print_hex64(dc[7]);
        serial_print("\n");

        return 0;
    }
    if (code != 1 && code != 13)
    {
        serial_print("[xhci] control transfer: completion code ");
        serial_print_uint(code);
        serial_print("\n");
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------
// Descriptor parsing (just enough to find the boot-keyboard interface
// and its interrupt IN endpoint -- no hubs, no other classes)
// ---------------------------------------------------------------------

#define DESC_TYPE_DEVICE 1
#define DESC_TYPE_CONFIGURATION 2
#define DESC_TYPE_INTERFACE 4
#define DESC_TYPE_ENDPOINT 5

static int find_hid_keyboard_endpoint(const uint8_t *config_desc, uint16_t total_len,
                                      int *out_interface_num, int *out_endpoint_num,
                                      int *out_max_packet)
{
    size_t offset = 0;
    int in_hid_interface = 0;

    while (offset + 2 <= total_len)
    {
        uint8_t len = config_desc[offset];
        uint8_t type = config_desc[offset + 1];
        if (len == 0)
            break;

        if (type == DESC_TYPE_INTERFACE && len >= 9)
        {
            uint8_t interface_class = config_desc[offset + 5];
            uint8_t interface_subclass = config_desc[offset + 6];
            uint8_t interface_protocol = config_desc[offset + 7];
            // Class 3 = HID, SubClass 1 = Boot Interface, Protocol 1 = Keyboard
            in_hid_interface = (interface_class == 3 && interface_subclass == 1 && interface_protocol == 1);
            if (in_hid_interface)
            {
                *out_interface_num = config_desc[offset + 2];
            }
        }
        else if (type == DESC_TYPE_ENDPOINT && len >= 7 && in_hid_interface)
        {
            uint8_t ep_addr = config_desc[offset + 2];
            uint8_t attributes = config_desc[offset + 3];
            uint16_t max_packet = (uint16_t)(config_desc[offset + 4] | (config_desc[offset + 5] << 8));
            int is_in = (ep_addr & 0x80) != 0;
            int is_interrupt = (attributes & 0x03) == 3;
            if (is_in && is_interrupt)
            {
                *out_endpoint_num = ep_addr & 0x0F;
                *out_max_packet = max_packet;
                return 1;
            }
        }

        offset += len;
    }
    return 0;
}

// ---------------------------------------------------------------------
// HID boot-keyboard report -> ASCII (matches keyboard.c's kb_getc()
// convention: printable ASCII, '\n'/'\b'/'\t', Ctrl+letter as 0x01-0x1A,
// and arrow keys as the 3-byte ESC '[' 'A'/'B'/'C'/'D' sequence)
// ---------------------------------------------------------------------

static char usage_to_ascii(uint8_t usage, int shift)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    if (usage >= 0x04 && usage <= 0x1D)
    {
        return shift ? upper[usage - 0x04] : lower[usage - 0x04];
    }
    if (usage >= 0x1E && usage <= 0x26)
    { // '1'-'9'
        static const char digits[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        return shift ? shifted[usage - 0x1E] : digits[usage - 0x1E];
    }
    switch (usage)
    {
    case 0x27:
        return shift ? ')' : '0';
    case 0x28:
        return '\n';
    case 0x2A:
        return '\b';
    case 0x2B:
        return '\t';
    case 0x2C:
        return ' ';
    case 0x2D:
        return shift ? '_' : '-';
    case 0x2E:
        return shift ? '+' : '=';
    case 0x2F:
        return shift ? '{' : '[';
    case 0x30:
        return shift ? '}' : ']';
    case 0x31:
        return shift ? '|' : '\\';
    case 0x33:
        return shift ? ':' : ';';
    case 0x34:
        return shift ? '"' : '\'';
    case 0x35:
        return shift ? '~' : '`';
    case 0x36:
        return shift ? '<' : ',';
    case 0x37:
        return shift ? '>' : '.';
    case 0x38:
        return shift ? '?' : '/';
    default:
        return 0; // unmapped: function keys, numpad, lock keys, etc.
    }
}

// Small pending-byte buffer, same idea as keyboard.c's, so a single
// "arrow key pressed" event can be returned as three separate poll calls.
static char g_pending[8];
static int g_pending_len = 0;
static int g_pending_pos = 0;

static void push_pending(const char *bytes, int n)
{
    g_pending_len = n;
    g_pending_pos = 0;
    for (int i = 0; i < n; i++)
        g_pending[i] = bytes[i];
}

// Compares the new 8-byte boot report against the previous one and
// queues translated bytes for every key that is newly pressed (present
// now, absent before). This is level-triggered hardware (the report is
// a full snapshot, not an edge event), so without this we'd re-emit
// every held-down key on every single poll -- comparing against the
// previous report is what makes it behave like a normal keypress event.
static void process_report(const uint8_t *report)
{
    uint8_t modifiers = report[0];
    int shift = (modifiers & 0x22) != 0; // bit1 LShift, bit5 RShift
    int ctrl = (modifiers & 0x11) != 0;  // bit0 LCtrl,  bit4 RCtrl

    for (int i = 2; i < 8; i++)
    {
        uint8_t usage = report[i];
        if (usage == 0)
            continue;

        int was_pressed = 0;
        for (int j = 2; j < 8; j++)
        {
            if (g_prev_report[j] == usage)
            {
                was_pressed = 1;
                break;
            }
        }
        if (was_pressed)
            continue; // still held from last report, not a new press

        if (usage == 0x4F)
        {
            char seq[3] = {0x1B, '[', 'C'};
            push_pending(seq, 3);
            return;
        } // Right
        if (usage == 0x50)
        {
            char seq[3] = {0x1B, '[', 'D'};
            push_pending(seq, 3);
            return;
        } // Left
        if (usage == 0x51)
        {
            char seq[3] = {0x1B, '[', 'B'};
            push_pending(seq, 3);
            return;
        } // Down
        if (usage == 0x52)
        {
            char seq[3] = {0x1B, '[', 'A'};
            push_pending(seq, 3);
            return;
        } // Up

        char ascii = usage_to_ascii(usage, shift);
        if (ascii == 0)
            continue;

        if (ctrl && ascii >= 'a' && ascii <= 'z')
        {
            char c = (char)(ascii - 'a' + 1);
            push_pending(&c, 1);
            return;
        }
        if (ctrl && ascii >= 'A' && ascii <= 'Z')
        {
            char c = (char)(ascii - 'A' + 1);
            push_pending(&c, 1);
            return;
        }

        push_pending(&ascii, 1);
        return; // one new key per report is plenty for our polling rate
    }
}

// Re-arms the interrupt endpoint's transfer ring with a fresh Normal TRB
// to receive the next report, and rings its doorbell.
static void queue_next_report(void)
{
    uint64_t phys = virt_to_phys(g_report_buf);
    int dci = 2 * g_intr_endpoint_num + 1; // Interrupt IN endpoint DCI
    ring_push(&g_intr_ring, phys, (uint32_t)g_report_len, (uint32_t)((TRB_TYPE_NORMAL << 10) | (1u << 5)));
    ring_doorbell(g_slot_id, (uint32_t)dci);
}

int xhci_poll_key(void)
{
    if (!g_ready)
        return -1;

    if (g_pending_pos < g_pending_len)
    {
        return (unsigned char)g_pending[g_pending_pos++];
    }

    xhci_trb_t trb;
    if (event_ring_pop(&trb))
    {
        uint32_t trb_type = (trb.control >> 10) & 0x3F;
        if (trb_type == TRB_TYPE_TRANSFER_EVENT)
        {
            uint8_t code = (uint8_t)((trb.status >> 24) & 0xFF);
            if (code == 1 || code == 13)
            { // Success or Short Packet
                process_report(g_report_buf);
                memcpy(g_prev_report, g_report_buf, sizeof(g_prev_report));
            }
            queue_next_report();
        }
    }

    if (g_pending_pos < g_pending_len)
    {
        return (unsigned char)g_pending[g_pending_pos++];
    }
    return -1;
}

// ---------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------

static int reset_controller(void)
{
    if (reg32(g_op + OP_USBCMD) & USBCMD_RUN)
    {
        set_reg32(g_op + OP_USBCMD, reg32(g_op + OP_USBCMD) & ~USBCMD_RUN);
        while (!(reg32(g_op + OP_USBSTS) & USBSTS_HCH))
        {
        }
    }

    set_reg32(g_op + OP_USBCMD, reg32(g_op + OP_USBCMD) | USBCMD_HCRST);
    while (reg32(g_op + OP_USBCMD) & USBCMD_HCRST)
    {
    }
    while (reg32(g_op + OP_USBSTS) & USBSTS_CNR)
    {
    }
    return 1;
}

static int reset_port(int port, int *out_speed)
{
    volatile uint32_t *portsc = (volatile uint32_t *)(g_op + OP_PORTSC(port));

    if (!(reg32(portsc) & PORTSC_CCS))
        return 0; // nothing plugged in here

    set_reg32(portsc, portsc_safe_base(reg32(portsc)) | PORTSC_PR);
    while (!(reg32(portsc) & PORTSC_PRC))
    {
    }
    set_reg32(portsc, portsc_safe_base(reg32(portsc)) | PORTSC_PRC); // clear the change bit

    uint32_t val = reg32(portsc);
    if (!(val & PORTSC_PED))
        return 0; // reset completed but port didn't enable

    *out_speed = (int)((val & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);
    return 1;
}

// Tries to bring up whatever's plugged into `port` (already reset, with
// negotiated `speed`) as a HID boot keyboard. Returns 1 and leaves the
// driver ready (g_ready=1) on success. Returns 0 if the device isn't a
// keyboard or any step fails -- in that case it cleans up the slot it
// allocated so the caller can move on and try the next port.
static int try_setup_keyboard_on_port(int port, int speed)
{
    // Speed IDs per xHCI spec Table 5-26 / PORTSC: 1=Full,2=Low,3=High,4=Super(+)
    switch (speed)
    {
    case 2:
        g_ep0_max_packet = 8;
        break;
    case 1:
        g_ep0_max_packet = 8;
        break;
    case 3:
        g_ep0_max_packet = 64;
        break;
    default:
        g_ep0_max_packet = 512;
        break;
    }

    uint8_t slot_id;
    if (!cmd_enable_slot(&slot_id))
        return 0;

    g_slot_id = slot_id;

    /* xHCI requires the DCBAA slot entry to point at the device context
     * before issuing Address Device. */
    memset(g_device_ctx, 0, sizeof(g_device_ctx));
    g_dcbaa[slot_id] = virt_to_phys(g_device_ctx);

    serial_print("[xhci] enabled slot ");

    memset(g_input_ctx, 0, sizeof(g_input_ctx));

    uint32_t *icc = ctx_input_control(g_input_ctx);
    icc[1] = (1u << 0) | (1u << 1); // Add Context: Slot + EP0

    uint32_t *slot_ctx = ctx_slot(g_input_ctx, 1);
    slot_ctx[0] =
        (1u << 27) |             // Context Entries = 1
        ((uint32_t)speed << 20); // Speed

    slot_ctx[1] =
        ((uint32_t)port << 16); // Root Hub Port Number

    ring_init(&g_ep0_ring);

    uint64_t ep0_ring_phys = virt_to_phys(&g_ep0_ring.trbs[0]);

    uint32_t *ep0_ctx = ctx_ep(g_input_ctx, 1, 1);

    ep0_ctx[1] =
        (4u << 3) | // EP Type = Control
        (3u << 1) | // CErr = 3
        ((uint32_t)g_ep0_max_packet << 16);

    ep0_ctx[2] =
        (uint32_t)(ep0_ring_phys | 1u); // DCS = 1

    ep0_ctx[3] =
        (uint32_t)(ep0_ring_phys >> 32);

    ep0_ctx[4] = 8; // Average TRB Length

    serial_print("[xhci] DEVICE ctx phys = ");
    serial_print_hex64(virt_to_phys(g_device_ctx));
    serial_print("\n");

    serial_print("[xhci] DCBAA[slot] = ");
    serial_print_hex64(g_dcbaa[slot_id]);
    serial_print("\n");

    serial_print("[xhci] INPUT ctx phys = ");
    serial_print_hex64(virt_to_phys(g_input_ctx));
    serial_print("\n");

    serial_print("[xhci] input ctx words:\n");

    for (int i = 0; i < 12; i++)
    {
        serial_print("  ");
        serial_print_uint(i);
        serial_print(": ");
        serial_print_hex64(((uint64_t *)g_input_ctx)[i]);
        serial_print("\n");
    }

    serial_print("[xhci] EP0 ring phys = ");
    serial_print_hex64(ep0_ring_phys);
    serial_print("\n");

    uint32_t portsc_now = reg32((volatile void *)(g_op + OP_PORTSC(port)));

    serial_print("[xhci] PORTSC before Address Device = ");
    serial_print_hex64(portsc_now);
    serial_print("\n");

    serial_print("[xhci]   CCS=");
    serial_print_uint((portsc_now & PORTSC_CCS) ? 1 : 0);
    serial_print(" PED=");
    serial_print_uint((portsc_now & PORTSC_PED) ? 1 : 0);
    serial_print(" PR=");
    serial_print_uint((portsc_now & PORTSC_PR) ? 1 : 0);
    serial_print(" PRC=");
    serial_print_uint((portsc_now & PORTSC_PRC) ? 1 : 0);
    serial_print(" speed=");
    serial_print_uint((portsc_now & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);
    serial_print("\n");

    serial_print("[xhci] >>> CALLING cmd_address_device <<<\n");

    if (!cmd_address_device(slot_id, g_input_ctx, 1))
    {
        serial_print("[xhci] >>> ADDRESS DEVICE FAILED <<<\n");
        cmd_disable_slot(slot_id);
        return 0;
    }

    serial_print("[xhci] >>> ADDRESS DEVICE RETURNED SUCCESS <<<\n");
    serial_print("[xhci] device addressed\n");

    // Get Device Descriptor (18 bytes) -- mostly just to sanity-check
    // we're talking to something real; we don't currently need any of
    // its fields beyond having confirmed the transfer works.
    if (!control_transfer(0x80, 6, (1u << 8), 0, 8, g_xfer_buf, 1))
    {
        serial_print("[xhci] get device descriptor failed\n");
        cmd_disable_slot(slot_id);
        return 0;
    }

    // Get Configuration Descriptor: first the 9-byte header for the real
    // total length, then the whole thing.
    if (!control_transfer(0x80, 6, (2u << 8), 0, 9, g_xfer_buf, 1))
    {
        serial_print("[xhci] get configuration descriptor (header) failed\n");
        cmd_disable_slot(slot_id);
        return 0;
    }
    uint16_t total_len = (uint16_t)(g_xfer_buf[2] | (g_xfer_buf[3] << 8));
    if (total_len > sizeof(g_xfer_buf))
        total_len = sizeof(g_xfer_buf);

    if (!control_transfer(0x80, 6, (2u << 8), 0, total_len, g_xfer_buf, 1))
    {
        serial_print("[xhci] get configuration descriptor (full) failed\n");
        cmd_disable_slot(slot_id);
        return 0;
    }

    // Transition Default -> Addressed (real SET_ADDRESS)
    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    icc = ctx_input_control(g_input_ctx);
    icc[1] = (1u << 0) | (1u << 1); // Add Context: Slot + EP0

    slot_ctx = ctx_slot(g_input_ctx, 1);
    slot_ctx[0] = (1u << 27) | ((uint32_t)speed << 20);
    slot_ctx[1] = ((uint32_t)port << 16);

    ep0_ctx = ctx_ep(g_input_ctx, 1, 1);
    ep0_ctx[1] = (4u << 3) | (3u << 1) | ((uint32_t)g_ep0_max_packet << 16);
    ep0_ctx[2] = (uint32_t)(ep0_ring_phys | 1u);
    ep0_ctx[3] = (uint32_t)(ep0_ring_phys >> 32);
    ep0_ctx[4] = 8;

    if (!cmd_address_device(slot_id, g_input_ctx, 0)) // BSR=0
    {
        serial_print("[xhci] address device (BSR=0) failed\n");
        cmd_disable_slot(slot_id);
        return 0;
    }

    int interface_num = 0, endpoint_num = 0, intr_max_packet = 8;
    if (!find_hid_keyboard_endpoint(g_xfer_buf, total_len, &interface_num, &endpoint_num, &intr_max_packet))
    {
        serial_print("[xhci] not a HID boot keyboard, trying next port\n");
        cmd_disable_slot(slot_id);
        return 0;
    }
    g_intr_endpoint_num = endpoint_num;
    if (intr_max_packet > (int)sizeof(g_report_buf))
        intr_max_packet = sizeof(g_report_buf);
    g_report_len = intr_max_packet;

    uint8_t config_value = g_xfer_buf[5];
    if (!control_transfer(0x00, 9, config_value, 0, 0, NULL, 0))
    {
        serial_print("[xhci] set configuration failed\n");
        cmd_disable_slot(slot_id);
        return 0;
    }

    // Class-specific SET_PROTOCOL -> Boot Protocol (wValue=0), so every
    // report is a fixed 8-byte [modifiers][reserved][6 keycodes] layout
    // instead of an arbitrary HID report we'd need a parser for.
    if (!control_transfer(0x21, 0x0B, 0, (uint16_t)interface_num, 0, NULL, 0))
    {
        serial_print("[xhci] set protocol (boot) failed\n");
        cmd_disable_slot(slot_id);
        return 0;
    }

    ring_init(&g_intr_ring);
    int dci = 2 * g_intr_endpoint_num + 1;

    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    icc = ctx_input_control(g_input_ctx);
    icc[1] = (uint32_t)(1u << dci);
    icc[1] |= (1u << 0); // Slot Context must also be evaluated whenever we touch a new endpoint

    slot_ctx[0] = (uint32_t)(((uint32_t)dci << 27) | ((uint32_t)speed << 20)); // Context Entries = highest DCI used

    uint32_t *intr_ctx = ctx_ep(g_input_ctx, 1, dci);
    intr_ctx[0] = (uint32_t)(3u << 16);                                   // Interval: modest polling period, controller will round to nearest legal value
    intr_ctx[1] = (uint32_t)((7u << 3) | ((uint32_t)g_report_len << 16)); // EP Type = Interrupt IN
    uint64_t intr_ring_phys = virt_to_phys(&g_intr_ring.trbs[0]);
    intr_ctx[2] = (uint32_t)(intr_ring_phys | 1u);
    intr_ctx[3] = (uint32_t)(intr_ring_phys >> 32);
    intr_ctx[4] = (uint32_t)g_report_len; // Average TRB Length

    if (!cmd_configure_endpoint(slot_id, g_input_ctx))
    {
        cmd_disable_slot(slot_id);
        return 0;
    }
    serial_print("[xhci] endpoint configured, keyboard ready\n");

    memset(g_prev_report, 0, sizeof(g_prev_report));
    g_ready = 1;
    queue_next_report();
    return 1;
}

// ---------------------------------------------------------------------
// Millisecond delay via the legacy 8254 PIT, channel 2, using the
// classic "speaker gate" polling trick -- no interrupts or timer
// subsystem required. Needed because the USB 2.0 spec mandates a
// minimum ~10ms "Reset Recovery Time" after a port reset before the
// host may talk to the device at all. Real silicon actually needs this
// settle time to be ready to respond; virtual/emulated USB devices
// (QEMU) don't bother enforcing it, which is why enumeration can work
// perfectly under QEMU while every real device times out without this.
// ---------------------------------------------------------------------

#define PIT_FREQUENCY_HZ 1193182u

static inline void outb_pit(uint16_t port, uint8_t val)
{
    asm volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}
static inline uint8_t inb_pit(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// Busy-waits for up to ~54ms (channel 2's counter is 16-bit, so that's
// the longest single chunk representable at the PIT's ~1.193MHz clock).
static void pit_wait_ticks(uint16_t ticks)
{
    // Bit0 of port 0x61 gates channel 2's clock; bit1 would route it to
    // the PC speaker, which we leave off since we only want the timing,
    // not the beep.
    uint8_t p61 = inb_pit(0x61);
    outb_pit(0x61, (uint8_t)((p61 & ~0x02u) | 0x00u));

    outb_pit(0x43, 0xB0); // channel 2, lobyte/hibyte access, mode 0, binary
    outb_pit(0x42, (uint8_t)(ticks & 0xFF));
    outb_pit(0x42, (uint8_t)((ticks >> 8) & 0xFF));

    // Toggle the gate low->high to (re)start the countdown from the
    // value we just loaded.
    uint8_t gate = inb_pit(0x61) & ~0x01u;
    outb_pit(0x61, gate);
    outb_pit(0x61, gate | 0x01u);

    while (!(inb_pit(0x61) & 0x20))
    {
    } // bit5 (OUT2) goes high at terminal count
}

static void pit_delay_ms(uint32_t ms)
{
    while (ms > 0)
    {
        uint32_t chunk = (ms > 50) ? 50 : ms; // stay well under the 16-bit counter's ~54ms ceiling
        uint32_t ticks = (PIT_FREQUENCY_HZ / 1000u) * chunk;
        pit_wait_ticks((uint16_t)ticks);
        ms -= chunk;
    }
}

void xhci_init(void)
{
    serial_print("[xhci] looking for controller...\n");

    pci_device_t dev;
    if (!pci_find_by_class(0x0C, 0x03, 0x30, &dev))
    {
        serial_print("[xhci] no xHCI controller found on PCI\n");
        return;
    }

    pci_enable_device(dev);
    uint64_t bar_phys = pci_read_bar(dev, 0);
    if (bar_phys == 0)
    {
        serial_print("[xhci] BAR0 wasn't a memory BAR, giving up\n");
        return;
    }

    // The HHDM Limine sets up only covers memory it saw in the boot
    // memory map -- it does NOT guarantee coverage of a PCI BAR's MMIO
    // window (real hardware routinely places these in PCI-hole address
    // ranges outside that map). Map it ourselves before touching a
    // single register, or the very first read below can fault on an
    // address phys_to_virt() happily computed but nothing ever mapped.
    uint64_t bar_size = pci_bar_size(dev, 0);
    if (bar_size == 0)
        bar_size = 0x10000; // fallback: shouldn't happen for a real memory BAR
    mm_map_mmio(bar_phys, bar_size);

    g_mmio = (volatile uint8_t *)phys_to_virt(bar_phys);
    serial_print("[xhci] MMIO base phys=");
    serial_print_hex64(bar_phys);
    serial_print("\n");

    uint8_t cap_length = g_mmio[CAP_CAPLENGTH];
    g_op = g_mmio + cap_length;

    uint32_t hcsparams1 = reg32(g_mmio + CAP_HCSPARAMS1);
    g_max_slots = hcsparams1 & 0xFF;
    g_max_ports = (hcsparams1 >> 24) & 0xFF;

    uint32_t hcsparams2 = reg32(g_mmio + CAP_HCSPARAMS2);
    uint32_t scratch_hi = (hcsparams2 >> 21) & 0x1F;
    uint32_t scratch_lo = (hcsparams2 >> 27) & 0x1F;
    uint32_t num_scratchpads = (scratch_hi << 5) | scratch_lo;

    uint32_t hccparams1 = reg32(g_mmio + CAP_HCCPARAMS1);
    g_context_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    /*
     * Claim xHCI ownership from firmware/BIOS.
     *
     * The xHCI Legacy Support Capability is located through the
     * Extended Capabilities Pointer in HCCPARAMS1.
     */
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFFu;

    while (xecp != 0)
    {
        volatile uint32_t *ext = (volatile uint32_t *)(g_mmio + ((uintptr_t)xecp << 2));
        uint32_t hdr = reg32(ext);

        uint32_t cap_id = hdr & 0xFFu;
        uint32_t next = (hdr >> 8) & 0xFFu;

        if (cap_id == 1)
        {
            /* xHCI USB Legacy Support Capability */
            uint32_t leg = reg32(ext);

            serial_print("[xhci] USBLEGSUP before handoff = ");
            serial_print_hex64(leg);
            serial_print("\n");

            /* Request OS ownership. */
            leg |= (1u << 24);
            set_reg32(ext, leg);

            /* Wait for BIOS ownership to clear. */
            int released = 0;

            for (int i = 0; i < 1000; i++)
            {
                uint32_t now = reg32(ext);

                if (!(now & (1u << 16)))
                {
                    released = 1;
                    break;
                }

                pit_delay_ms(1);
            }

            uint32_t final_leg = reg32(ext);

            serial_print("[xhci] USBLEGSUP after handoff = ");
            serial_print_hex64(final_leg);
            serial_print("\n");

            if (released)
            {
                serial_print("[xhci] xHCI BIOS ownership released\n");
            }
            else
            {
                serial_print("[xhci] WARNING: BIOS still owns xHCI\n");

                /*
                 * Last-resort takeover, matching the behavior used by
                 * mature xHCI drivers for broken firmware.
                 */
                final_leg &= ~(1u << 16);
                final_leg |= (1u << 24);
                set_reg32(ext, final_leg);

                serial_print("[xhci] forced xHCI OS ownership\n");
            }

            break;
        }

        xecp = next;
    }

    uint32_t dboff = reg32(g_mmio + CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff = reg32(g_mmio + CAP_RTSOFF) & ~0x1Fu;
    g_doorbells = (volatile uint32_t *)(g_mmio + dboff);
    g_rt = g_mmio + rtsoff;

    uint32_t pagesize = reg32(g_op + OP_PAGESIZE);

    serial_print("[xhci] max_slots=");
    serial_print_uint(g_max_slots);
    serial_print(" max_ports=");
    serial_print_uint(g_max_ports);
    serial_print(" context_size=");
    serial_print_uint((unsigned int)g_context_size);
    serial_print(" scratchpads=");
    serial_print_uint(num_scratchpads);
    serial_print(" PAGESIZE=");
    serial_print_hex64(pagesize);
    serial_print("\n");

    reset_controller();

    // Device Context Base Address Array: one 64-bit pointer per slot, plus
    // slot 0 reserved for the scratchpad buffer array pointer (if any).
    size_t dcbaa_size = (size_t)(g_max_slots + 1) * sizeof(uint64_t);
    g_dcbaa = (uint64_t *)kalloc(dcbaa_size + 64);
    if (g_dcbaa == NULL)
    {
        serial_print("[xhci] kalloc failed for DCBAA (out of heap?), giving up\n");
        return;
    }
    g_dcbaa = (uint64_t *)(((uintptr_t)g_dcbaa + 63) & ~(uintptr_t)63); // 64-byte align
    memset(g_dcbaa, 0, dcbaa_size);

    if (num_scratchpads > 0)
    {
        uint64_t *scratch_array = (uint64_t *)kalloc(num_scratchpads * sizeof(uint64_t) + 64);
        if (scratch_array == NULL)
        {
            serial_print("[xhci] kalloc failed for scratchpad array (out of heap?), giving up\n");
            return;
        }
        scratch_array = (uint64_t *)(((uintptr_t)scratch_array + 63) & ~(uintptr_t)63);
        for (uint32_t i = 0; i < num_scratchpads; i++)
        {
            void *page = kalloc(4096 + 4096);
            if (page == NULL)
            {
                serial_print("[xhci] kalloc failed for scratchpad buffer ");
                serial_print_uint(i);
                serial_print(" of ");
                serial_print_uint(num_scratchpads);
                serial_print(" (out of heap?), giving up\n");
                return;
            }
            page = (void *)(((uintptr_t)page + 4095) & ~(uintptr_t)4095);
            /*
             * xHCI requires every scratchpad buffer to be cleared
             * before the controller is allowed to use it.
             */
            memset(page, 0, 4096);
            scratch_array[i] = virt_to_phys(page);
        }
        g_dcbaa[0] = virt_to_phys(scratch_array);
        serial_print("[xhci] DCBAA[0] scratchpad array phys=");
        serial_print_hex64(g_dcbaa[0]);
        serial_print("\n");
    }

    set_reg64(g_op + OP_DCBAAP, virt_to_phys(g_dcbaa));

    serial_print("[xhci] DCBAA phys=");
    serial_print_hex64(virt_to_phys(g_dcbaa));
    serial_print("\n");

    ring_init(&g_cmd_ring);

    serial_print("[xhci] CMD ring phys=");
    serial_print_hex64(virt_to_phys(&g_cmd_ring.trbs[0]));
    serial_print("\n");

    set_reg64(g_op + OP_CRCR, virt_to_phys(&g_cmd_ring.trbs[0]) | 1u);

    memset(g_event_ring, 0, sizeof(g_event_ring));
    g_event_dequeue = 0;
    g_event_ccs = 1;

    serial_print("[xhci] EVENT ring phys=");
    serial_print_hex64(virt_to_phys(g_event_ring));
    serial_print("\n");

    serial_print("[xhci] ERST phys=");
    serial_print_hex64(virt_to_phys(g_erst));
    serial_print("\n");

    g_erst[0].ring_segment_base = virt_to_phys(g_event_ring);
    g_erst[0].ring_segment_size = RING_SIZE;
    g_erst[0].reserved = 0;

    set_reg32(g_rt + RT_IR0 + IR_ERSTSZ, 1);
    set_reg64(g_rt + RT_IR0 + IR_ERDP, virt_to_phys(g_event_ring));
    set_reg64(g_rt + RT_IR0 + IR_ERSTBA, virt_to_phys(g_erst));

    set_reg32(g_op + OP_CONFIG, 1); // MaxSlotsEn = 1, we only need one device

    set_reg32(g_op + OP_USBCMD, reg32(g_op + OP_USBCMD) | USBCMD_RUN);
    while (reg32(g_op + OP_USBSTS) & USBSTS_HCH)
    {
    }

    serial_print("[xhci] controller running, scanning ports\n");

    // Try every connected port in turn until one of them turns out to be
    // a HID boot keyboard. This matters as soon as more than one USB
    // device is plugged in -- e.g. booting off a USB stick with a
    // keyboard in another port -- since whichever device isn't a
    // keyboard needs to be skipped rather than causing us to give up.
    for (uint32_t port = 1; port <= g_max_ports; port++)
    {
        int speed = 0;
        if (!reset_port((int)port, &speed))
            continue; // nothing here, or didn't enable

        serial_print("[xhci] device on port ");
        serial_print_uint(port);
        serial_print(" speed=");
        serial_print_uint((unsigned int)speed);
        serial_print("\n");

        // USB 2.0 Reset Recovery Time: the device needs a moment after
        // reset before it's ready to be addressed or talked to. Skipping
        // this is fine in QEMU (which doesn't model it) but real devices
        // will just never respond, which looks exactly like every
        // transaction timing out -- because that's exactly what happens.
        pit_delay_ms(20);

        if (try_setup_keyboard_on_port((int)port, speed))
        {
            return; // g_ready is now 1, keyboard is live
        }
    }

    serial_print("[xhci] no HID boot keyboard found on any port\n");
}