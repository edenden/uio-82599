//#define DEBUG

#define ALIGN(x,a)		__ALIGN_MASK(x,(typeof(x))(a)-1)
#define __ALIGN_MASK(x,mask)	(((x)+(mask))&~(mask))

#define FILENAME_SIZE 256

#define min(x, y) ({				\
	typeof(x) _min1 = (x);			\
	typeof(y) _min2 = (y);			\
	(void) (&_min1 == &_min2);		\
	_min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({				\
	typeof(x) _max1 = (x);			\
	typeof(y) _max2 = (y);			\
	(void) (&_max1 == &_max2);		\
	_max1 > _max2 ? _max1 : _max2; })

#define CONFIG_X86_L1_CACHE_SHIFT \
				(6)
#define L1_CACHE_SHIFT		(CONFIG_X86_L1_CACHE_SHIFT)
#define L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)

#ifdef DEBUG
#define ixmap_print(args...) printf("ixgbe: " args)
#else
#define ixmap_print(args...)
#endif

#define DMA_64BIT_MASK		0xffffffffffffffffULL
#define DMA_BIT_MASK(n)		(((n) == 64) ? \
				DMA_64BIT_MASK : ((1ULL<<(n))-1))

/* General Registers */
#define IXGBE_STATUS		0x00008

/* Interrupt Registers */
#define IXGBE_EIMS		0x00880
#define IXGBE_EIMS_EX(_i)	(0x00AA0 + (_i) * 4)

#define IXGBE_EICR_RTX_QUEUE	0x0000FFFF /* RTx Queue Interrupt */
#define IXGBE_EICR_LSC		0x00100000 /* Link Status Change */
#define IXGBE_EICR_TCP_TIMER	0x40000000 /* TCP Timer */
#define IXGBE_EICR_OTHER	0x80000000 /* Interrupt Cause Active */
#define IXGBE_EIMS_RTX_QUEUE	IXGBE_EICR_RTX_QUEUE /* RTx Queue Interrupt */
#define IXGBE_EIMS_LSC		IXGBE_EICR_LSC /* Link Status Change */
#define IXGBE_EIMS_TCP_TIMER	IXGBE_EICR_TCP_TIMER /* TCP Timer */
#define IXGBE_EIMS_OTHER	IXGBE_EICR_OTHER /* INT Cause Active */
#define IXGBE_EIMS_ENABLE_MASK ( \
				IXGBE_EIMS_RTX_QUEUE    | \
				IXGBE_EIMS_LSC          | \
				IXGBE_EIMS_TCP_TIMER    | \
				IXGBE_EIMS_OTHER)

struct ixmap_ring {
	void		*addr_virtual;
	unsigned long	addr_dma;

	uint8_t		*tail;
	uint16_t	next_to_use;
	uint16_t	next_to_clean;
	int		*slot_index;
};

struct ixmap_buf {
	void		*addr_virtual;
	unsigned long	*addr_dma;
	uint32_t	buf_size;
	uint32_t	count;

	uint32_t	free_count;
	int		*free_index;
};

struct ixmap_bulk {
	uint16_t	count;
	int		*slot_index;
	uint32_t	*size;
};

struct ixmap_handle {
 	int			fd;
	void			*bar;
	unsigned long		bar_size;

	char			*interface_name;
	struct ixmap_ring	*tx_ring;
	struct ixmap_ring	*rx_ring;
	struct ixmap_buf	*buf;

	void			*addr_virtual;
	unsigned long		addr_dma;
	uint32_t		num_tx_desc;
	uint32_t		num_rx_desc;
	uint32_t		budget;

	uint32_t		num_queues;
	uint16_t		num_interrupt_rate;
	uint32_t		promisc;
	uint32_t		mtu_frame;
	uint32_t		buf_size;
};

struct ixmap_irqdev_handle {
	int			fd;
	uint32_t		port_index;
	uint64_t		qmask;
};

struct ixmap_port {
	char			*interface_name;
	void			*irqreg[2];
	struct ixmap_ring	*rx_ring;
	struct ixmap_ring	*tx_ring;
	uint32_t		mtu_frame;
	uint32_t		num_tx_desc;
	uint32_t		num_rx_desc;
	uint32_t		num_queues;
	uint32_t		budget;

	unsigned long		count_rx_alloc_failed;
	unsigned long		count_rx_clean_total;
	unsigned long		count_tx_xmit_failed;
	unsigned long		count_tx_clean_total;
};

struct ixmap_instance {
	struct ixmap_port 	*ports;
};

enum {
	IXGBE_DMA_CACHE_DEFAULT = 0,
	IXGBE_DMA_CACHE_DISABLE,
	IXGBE_DMA_CACHE_WRITECOMBINE
};

enum ixmap_irq_direction {
	IXMAP_IRQ_RX = 0,
	IXMAP_IRQ_TX,
};

/* Receive Descriptor - Advanced */
union ixmap_adv_rx_desc {
	struct {
		uint64_t pkt_addr; /* Packet buffer address */
		uint64_t hdr_addr; /* Header buffer address */
	} read;
	struct {
		struct {
			union {
				uint32_t data;
				struct {
					uint16_t pkt_info; /* RSS, Pkt type */
					uint16_t hdr_info; /* Splithdr, hdrlen */
				} hs_rss;
			} lo_dword;
			union {
				uint32_t rss; /* RSS Hash */
				struct {
					uint16_t ip_id; /* IP id */
					uint16_t csum; /* Packet Checksum */
				} csum_ip;
			} hi_dword;
		} lower;
		struct {
			uint32_t status_error; /* ext status/error */
			uint16_t length; /* Packet length */
			uint16_t vlan; /* VLAN tag */
		} upper;
	} wb;  /* writeback */
};

/* Transmit Descriptor - Advanced */
union ixmap_adv_tx_desc {
	struct {
		uint64_t buffer_addr; /* Address of descriptor's data buf */
		uint32_t cmd_type_len;
		uint32_t olinfo_status;
	} read;
	struct {
		uint64_t rsvd; /* Reserved */
		uint32_t nxtseq_seed;
		uint32_t status;
	} wb;
};

/* Ioctl defines */

#define UIO_IXGBE_INFO		_IOW('E', 201, int)
/* MAC and PHY info */
struct uio_ixmap_info_req {
	unsigned long	mmio_base;
	unsigned long	mmio_size;

	uint16_t	mac_type;
	uint8_t		mac_addr[ETH_ALEN];
	uint16_t	phy_type;

	uint16_t	max_interrupt_rate;
	uint16_t	num_interrupt_rate;
	uint32_t	num_rx_queues;
	uint32_t	num_tx_queues;
	uint32_t	max_rx_queues;
	uint32_t	max_tx_queues;
	uint32_t	max_msix_vectors;
};

#define UIO_IXGBE_UP		_IOW('E', 202, int)
struct uio_ixmap_up_req {
	uint16_t		num_interrupt_rate;
	uint32_t		num_rx_queues;
	uint32_t		num_tx_queues;
};

#define UIO_IXGBE_MAP		_IOW('U', 210, int)
struct uio_ixmap_map_req {
        unsigned long addr_virtual;
        unsigned long addr_dma;
        unsigned long size;
        uint8_t cache;
};

#define UIO_IXGBE_UNMAP		_IOW('U', 211, int)
struct uio_ixmap_unmap_req {
        unsigned long addr_dma;
};

#define UIO_IRQ_INFO		_IOW('E', 201, int)
struct uio_irq_info_req {
	uint32_t	vector;
	uint16_t	entry;
};


static inline uint32_t readl(const volatile void *addr)
{
	return htole32( *(volatile uint32_t *) addr );
}

static inline void writel(uint32_t b, volatile void *addr)
{
	*(volatile uint32_t *) addr = htole32(b);
}

static inline uint32_t IXGBE_READ_REG(struct ixmap_handle *ih, uint32_t reg)
{
	uint32_t value = readl(ih->bar + reg);
	return value;
}

static inline void IXGBE_WRITE_REG(struct ixmap_handle *ih, uint32_t reg, uint32_t value)
{
	writel(value, ih->bar + reg);
	return;
}

#define IXGBE_WRITE_FLUSH(a)	IXGBE_READ_REG(a, IXGBE_STATUS)