#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <net/ethernet.h>
#include <signal.h>
#include <pthread.h>
#include <numa.h>

#include "ixmap.h"
#include "memory.h"

static void ixmap_irq_enable_queues(struct ixmap_handle *ih, uint64_t qmask);
static int ixmap_dma_map(struct ixmap_handle *ih, void *addr_virt,
	unsigned long *addr_dma, unsigned long size);
static int ixmap_dma_unmap(struct ixmap_handle *ih, unsigned long addr_dma);
static struct ixmap_irq_handle *ixmap_irq_open(struct ixmap_handle *ih,
	unsigned int core_id, enum ixmap_irq_type type);
static void ixmap_irq_close(struct ixmap_irq_handle *irqh);
static int ixmap_irq_setaffinity(unsigned int vector, unsigned int core_id);

inline uint32_t ixmap_readl(const volatile void *addr)
{
	return htole32( *(volatile uint32_t *) addr );
}

inline void ixmap_writel(uint32_t b, volatile void *addr)
{
	*(volatile uint32_t *) addr = htole32(b);
	return;
}

inline uint32_t ixmap_read_reg(struct ixmap_handle *ih, uint32_t reg)
{
	uint32_t value = ixmap_readl(ih->bar + reg);
	return value;
}

inline void ixmap_write_reg(struct ixmap_handle *ih, uint32_t reg, uint32_t value)
{
	ixmap_writel(value, ih->bar + reg);
	return;
}

inline void ixmap_write_flush(struct ixmap_handle *ih)
{
	ixmap_read_reg(ih, IXGBE_STATUS);
	return;
}

void ixmap_irq_enable(struct ixmap_handle *ih)
{
	uint32_t mask;

	mask = (IXGBE_EIMS_ENABLE_MASK & ~IXGBE_EIMS_RTX_QUEUE);

	/* XXX: Currently we don't support misc interrupts */
	mask &= ~IXGBE_EIMS_LSC;
	mask &= ~IXGBE_EIMS_TCP_TIMER;
	mask &= ~IXGBE_EIMS_OTHER;

	ixmap_write_reg(ih, IXGBE_EIMS, mask);

	ixmap_irq_enable_queues(ih, ~0);
	ixmap_write_flush(ih);

	return;
}

static void ixmap_irq_enable_queues(struct ixmap_handle *ih, uint64_t qmask)
{
	uint32_t mask;

	mask = (qmask & 0xFFFFFFFF);
	if (mask)
		ixmap_write_reg(ih, IXGBE_EIMS_EX(0), mask);
	mask = (qmask >> 32);
	if (mask)
		ixmap_write_reg(ih, IXGBE_EIMS_EX(1), mask);

	return;
}

struct ixmap_plane *ixmap_plane_alloc(struct ixmap_handle **ih_list,
	struct ixmap_buf *buf, int ih_num, int core_id)
{
	struct ixmap_plane *plane;
	int i, ports_assigned = 0;

	plane = numa_alloc_onnode(sizeof(struct ixmap_plane),
		numa_node_of_cpu(core_id));
	if(!plane)
		goto err_plane_alloc;

	plane->ports = numa_alloc_onnode(sizeof(struct ixmap_port) * ih_num,
		numa_node_of_cpu(core_id));
	if(!plane->ports){
		printf("failed to allocate port for each plane\n");
		goto err_alloc_ports;
	}

	for(i = 0; i < ih_num; i++, ports_assigned++){
		plane->ports[i].interface_name = ih_list[i]->interface_name;
		plane->ports[i].irqreg[0] = ih_list[i]->bar + IXGBE_EIMS_EX(0);
		plane->ports[i].irqreg[1] = ih_list[i]->bar + IXGBE_EIMS_EX(1);
		plane->ports[i].rx_ring = &(ih_list[i]->rx_ring[core_id]);
		plane->ports[i].tx_ring = &(ih_list[i]->tx_ring[core_id]);
		plane->ports[i].rx_slot_next = 0;
		plane->ports[i].rx_slot_offset = i * buf->count;
		plane->ports[i].tx_suspended = 0;
		plane->ports[i].num_rx_desc = ih_list[i]->num_rx_desc;
		plane->ports[i].num_tx_desc = ih_list[i]->num_tx_desc;
		plane->ports[i].num_queues = ih_list[i]->num_queues;
		plane->ports[i].rx_budget = ih_list[i]->rx_budget;
		plane->ports[i].tx_budget = ih_list[i]->tx_budget;
		plane->ports[i].mtu_frame = ih_list[i]->mtu_frame;
		plane->ports[i].count_rx_alloc_failed = 0;
		plane->ports[i].count_rx_clean_total = 0;
		plane->ports[i].count_tx_xmit_failed = 0;
		plane->ports[i].count_tx_clean_total = 0;

		memcpy(plane->ports[i].mac_addr, ih_list[i]->mac_addr, ETH_ALEN);

		plane->ports[i].rx_irq = ixmap_irq_open(ih_list[i], core_id,
						IXMAP_IRQ_RX);
		if(!plane->ports[i].rx_irq)
			goto err_alloc_irq_rx;

		plane->ports[i].tx_irq = ixmap_irq_open(ih_list[i], core_id,
						IXMAP_IRQ_TX);
		if(!plane->ports[i].tx_irq)
			goto err_alloc_irq_tx;

		continue;

err_alloc_irq_tx:
		ixmap_irq_close(plane->ports[i].rx_irq);
err_alloc_irq_rx:
		goto err_alloc_plane;
	}

	return plane;

err_alloc_plane:
	for(i = 0; i < ports_assigned; i++){
		ixmap_irq_close(plane->ports[i].rx_irq);
		ixmap_irq_close(plane->ports[i].tx_irq);
	}
	numa_free(plane->ports, sizeof(struct ixmap_port) * ih_num);
err_alloc_ports:
	numa_free(plane, sizeof(struct ixmap_plane));
err_plane_alloc:
	return NULL;
}

void ixmap_plane_release(struct ixmap_plane *plane, int ih_num)
{
	int i;

	for(i = 0; i < ih_num; i++){
		ixmap_irq_close(plane->ports[i].rx_irq);
		ixmap_irq_close(plane->ports[i].tx_irq);
	}

	numa_free(plane->ports, sizeof(struct ixmap_port) * ih_num);
	numa_free(plane, sizeof(struct ixmap_plane));

	return;
}

struct ixmap_desc *ixmap_desc_alloc(struct ixmap_handle **ih_list, int ih_num,
	int core_id)
{
	struct ixmap_desc *desc;
	unsigned long size, size_tx_desc, size_rx_desc, size_mem;
	void *addr_virt, *addr_mem;
	int i, ret;
	int desc_assigned = 0;

	desc = numa_alloc_onnode(sizeof(struct ixmap_desc),
		numa_node_of_cpu(core_id));
	if(!desc)
		goto err_alloc_desc;

	size = SIZE_1GB;
	numa_set_preferred(numa_node_of_cpu(core_id));

	addr_virt = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, 0, 0);
	if(addr_virt == MAP_FAILED){
		goto err_mmap;
	}

	desc->addr_virt = addr_virt;

	for(i = 0; i < ih_num; i++, desc_assigned++){
		int *slot_index;
		struct ixmap_handle *ih;
		unsigned long addr_dma;

		ih = ih_list[i];

		size_rx_desc = sizeof(union ixmap_adv_rx_desc) * ih->num_rx_desc;
		size_rx_desc = ALIGN(size_rx_desc, 128); /* needs 128-byte alignment */
		size_tx_desc = sizeof(union ixmap_adv_tx_desc) * ih->num_tx_desc;
		size_tx_desc = ALIGN(size_tx_desc, 128); /* needs 128-byte alignment */

		/* Rx descripter ring allocation */
		ret = ixmap_dma_map(ih, addr_virt, &addr_dma, size_rx_desc);
		if(ret < 0){
			goto err_rx_dma_map;
		}

		ih->rx_ring[core_id].addr_dma = addr_dma;
		ih->rx_ring[core_id].addr_virt = addr_virt;

		slot_index = numa_alloc_onnode(sizeof(int32_t) * ih->num_rx_desc,
			numa_node_of_cpu(core_id));
		if(!slot_index){
			goto err_rx_assign;
		}

		ih->rx_ring[core_id].next_to_use = 0;
		ih->rx_ring[core_id].next_to_clean = 0;
		ih->rx_ring[core_id].slot_index = slot_index;

		addr_virt += size_rx_desc;

		/* Tx descripter ring allocation */
		ret = ixmap_dma_map(ih, addr_virt, &addr_dma, size_tx_desc);
		if(ret < 0){
			goto err_tx_dma_map;
		}

		ih->tx_ring[core_id].addr_dma = addr_dma;
		ih->tx_ring[core_id].addr_virt = addr_virt;

		slot_index = numa_alloc_onnode(sizeof(int32_t) * ih->num_tx_desc,
			numa_node_of_cpu(core_id));
		if(!slot_index){
			goto err_tx_assign;
		}

		ih->tx_ring[core_id].next_to_use = 0;
		ih->tx_ring[core_id].next_to_clean = 0;
		ih->tx_ring[core_id].slot_index = slot_index;

		addr_virt += size_rx_desc;

		continue;

err_tx_assign:
		ixmap_dma_unmap(ih, ih->tx_ring[core_id].addr_dma);
err_tx_dma_map:
		numa_free(ih->rx_ring[core_id].slot_index,
			sizeof(int32_t) * ih->num_rx_desc);
err_rx_assign:
		ixmap_dma_unmap(ih, ih->rx_ring[core_id].addr_dma);
err_rx_dma_map:
		goto err_desc_assign;
	}

	addr_mem	= (void *)ALIGN((unsigned long)addr_virt, L1_CACHE_BYTES);
	size_mem	= size - (addr_mem - desc->addr_virt);
	desc->core_id	= core_id;
	desc->node	= ixmap_mem_init(addr_mem, size_mem, core_id);
	if(!desc->node)
		goto err_mem_init;

	return desc;

err_mem_init:
err_desc_assign:
	for(i = 0; i < desc_assigned; i++){
		struct ixmap_handle *ih;

		ih = ih_list[i];
		numa_free(ih->tx_ring[core_id].slot_index,
			sizeof(int32_t) * ih->num_tx_desc);
		ixmap_dma_unmap(ih, ih->tx_ring[core_id].addr_dma);
		numa_free(ih->rx_ring[core_id].slot_index,
			sizeof(int32_t) * ih->num_rx_desc);
		ixmap_dma_unmap(ih, ih->rx_ring[core_id].addr_dma);
	}
	munmap(desc->addr_virt, size);
err_mmap:
	numa_free(desc, sizeof(struct ixmap_desc));
err_alloc_desc:
	return NULL;
}

void ixmap_desc_release(struct ixmap_handle **ih_list, int ih_num,
	int core_id, struct ixmap_desc *desc)
{
	int i;

	ixmap_mem_destroy(desc->node);

	for(i = 0; i < ih_num; i++){
		struct ixmap_handle *ih;

		ih = ih_list[i];
		numa_free(ih->tx_ring[core_id].slot_index,
			sizeof(int32_t) * ih->num_tx_desc);
		ixmap_dma_unmap(ih, ih->tx_ring[core_id].addr_dma);
		numa_free(ih->rx_ring[core_id].slot_index,
			sizeof(int32_t) * ih->num_rx_desc);
		ixmap_dma_unmap(ih, ih->rx_ring[core_id].addr_dma);
	}

	munmap(desc->addr_virt, SIZE_1GB);
	numa_free(desc, sizeof(struct ixmap_desc));
	return;
}

struct ixmap_buf *ixmap_buf_alloc(struct ixmap_handle **ih_list,
	int ih_num, uint32_t count, uint32_t buf_size, int core_id)
{
	struct ixmap_buf *buf;
	void	*addr_virt;
	unsigned long addr_dma, size;
	int *slots;
	int ret, i, mapped_ports = 0;

	buf = numa_alloc_onnode(sizeof(struct ixmap_buf),
		numa_node_of_cpu(core_id));
	if(!buf)
		goto err_alloc_buf;

	buf->addr_dma = numa_alloc_onnode(sizeof(unsigned long) * ih_num,
		numa_node_of_cpu(core_id));
	if(!buf->addr_dma)
		goto err_alloc_buf_addr_dma;

	/*
	 * XXX: Should we add buffer padding for memory interleaving?
	 * DPDK does so in rte_mempool.c/optimize_object_size().
	 */
	size = buf_size * (ih_num * count);
	numa_set_preferred(numa_node_of_cpu(core_id));

	addr_virt = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, 0, 0);
	if(addr_virt == MAP_FAILED)
		goto err_mmap;

	for(i = 0; i < ih_num; i++, mapped_ports++){
		ret = ixmap_dma_map(ih_list[i], addr_virt, &addr_dma, size);
		if(ret < 0)
			goto err_ixmap_dma_map;

		buf->addr_dma[i] = addr_dma;
	}

	slots = numa_alloc_onnode(sizeof(int32_t) * (count * ih_num),
		numa_node_of_cpu(core_id));
	if(!slots)
		goto err_alloc_slots;

	buf->addr_virt = addr_virt;
	buf->buf_size = buf_size;
	buf->count = count;
	buf->slots = slots;

	for(i = 0; i < buf->count * ih_num; i++){
		buf->slots[i] = 0;
	}

	return buf;

err_alloc_slots:
err_ixmap_dma_map:
	for(i = 0; i < mapped_ports; i++){
		ixmap_dma_unmap(ih_list[i], buf->addr_dma[i]);
	}
	munmap(addr_virt, size);
err_mmap:
	numa_free(buf->addr_dma,
		sizeof(unsigned long) * ih_num);
err_alloc_buf_addr_dma:
	numa_free(buf,
		sizeof(struct ixmap_buf));
err_alloc_buf:
	return NULL;
}

void ixmap_buf_release(struct ixmap_buf *buf,
	struct ixmap_handle **ih_list, int ih_num)
{
	int i, ret;
	unsigned long size;

	numa_free(buf->slots,
		sizeof(int32_t) * (buf->count * ih_num));

	for(i = 0; i < ih_num; i++){
		ret = ixmap_dma_unmap(ih_list[i], buf->addr_dma[i]);
		if(ret < 0)
			perror("failed to unmap buf");
	}

	size = buf->buf_size * (ih_num * buf->count);
	munmap(buf->addr_virt, size);
	numa_free(buf->addr_dma,
		sizeof(unsigned long) * ih_num);
	numa_free(buf,
		sizeof(struct ixmap_buf));

	return;
}

static int ixmap_dma_map(struct ixmap_handle *ih, void *addr_virt,
	unsigned long *addr_dma, unsigned long size)
{
	struct ixmap_map_req req_map;

	req_map.addr_virt = (unsigned long)addr_virt;
	req_map.addr_dma = 0;
	req_map.size = size;
	req_map.cache = IXGBE_DMA_CACHE_DISABLE;

	if(ioctl(ih->fd, IXMAP_MAP, (unsigned long)&req_map) < 0)
		return -1;

	*addr_dma = req_map.addr_dma;
	return 0;
}

static int ixmap_dma_unmap(struct ixmap_handle *ih, unsigned long addr_dma)
{
	struct ixmap_unmap_req req_unmap;

	req_unmap.addr_dma = addr_dma;

	if(ioctl(ih->fd, IXMAP_UNMAP, (unsigned long)&req_unmap) < 0)
		return -1;

	return 0;
}

struct ixmap_handle *ixmap_open(unsigned int port_index,
	unsigned int num_queues_req, unsigned short intr_rate,
	unsigned int rx_budget, unsigned int tx_budget,
	unsigned int mtu_frame, unsigned int promisc,
	unsigned int num_rx_desc, unsigned int num_tx_desc)
{
	struct ixmap_handle *ih;
	char filename[FILENAME_SIZE];
	struct ixmap_info_req req_info;
	struct ixmap_up_req req_up;

	ih = malloc(sizeof(struct ixmap_handle));
	if (!ih)
		goto err_alloc_ih;
	memset(ih, 0, sizeof(struct ixmap_handle));

	snprintf(filename, sizeof(filename), "/dev/%s%d",
		IXMAP_IFNAME, port_index);
	ih->fd = open(filename, O_RDWR);
	if (ih->fd < 0)
		goto err_open;

	/* Get device information */
	memset(&req_info, 0, sizeof(struct ixmap_info_req));
	if(ioctl(ih->fd, IXMAP_INFO, (unsigned long)&req_info) < 0)
		goto err_ioctl_info;

	/* UP the device */
	memset(&req_up, 0, sizeof(struct ixmap_up_req));

	ih->num_interrupt_rate =
		min(intr_rate, req_info.max_interrupt_rate);
	req_up.num_interrupt_rate = ih->num_interrupt_rate;

	ih->num_queues =
		min(req_info.max_rx_queues, req_info.max_tx_queues);
	ih->num_queues = min(num_queues_req, ih->num_queues);
	req_up.num_rx_queues = ih->num_queues;
	req_up.num_tx_queues = ih->num_queues;

	if(ioctl(ih->fd, IXMAP_UP, (unsigned long)&req_up) < 0)
		goto err_ioctl_up;

	/* Map PCI config register space */
	ih->bar = mmap(NULL, req_info.mmio_size,
		PROT_READ | PROT_WRITE, MAP_SHARED, ih->fd, 0);
	if(ih->bar == MAP_FAILED)
		goto err_mmap;

	ih->rx_ring = malloc(sizeof(struct ixmap_ring) * ih->num_queues);
	if(!ih->rx_ring)
		goto err_alloc_rx_ring;

	ih->tx_ring = malloc(sizeof(struct ixmap_ring) * ih->num_queues);
	if(!ih->tx_ring)
		goto err_alloc_tx_ring;

	ih->bar_size = req_info.mmio_size;
	ih->promisc = !!promisc;
	ih->rx_budget = rx_budget;
	ih->tx_budget = tx_budget;
	ih->mtu_frame = mtu_frame;
	ih->num_rx_desc = num_rx_desc;
	ih->num_tx_desc = num_tx_desc;
	memcpy(ih->mac_addr, req_info.mac_addr, ETH_ALEN);
	snprintf(ih->interface_name, sizeof(ih->interface_name), "%s%d",
		IXMAP_IFNAME, port_index);

	return ih;

err_alloc_tx_ring:
	free(ih->rx_ring);
err_alloc_rx_ring:
	munmap(ih->bar, ih->bar_size);
err_mmap:
err_ioctl_up:
err_ioctl_info:
	close(ih->fd);
err_open:
	free(ih);
err_alloc_ih:
	return NULL;
}

void ixmap_close(struct ixmap_handle *ih)
{
	free(ih->tx_ring);
	free(ih->rx_ring);
	munmap(ih->bar, ih->bar_size);
	close(ih->fd);
	free(ih);

	return;
}

unsigned int ixmap_bufsize_get(struct ixmap_handle *ih)
{
	return ih->buf_size;
}

uint8_t *ixmap_macaddr_default(struct ixmap_handle *ih)
{
	return ih->mac_addr;
}

unsigned int ixmap_mtu_get(struct ixmap_handle *ih)
{
	return ih->mtu_frame;
}

static struct ixmap_irq_handle *ixmap_irq_open(struct ixmap_handle *ih,
	unsigned int core_id, enum ixmap_irq_type type)
{
	struct ixmap_irq_handle *irqh;
	uint64_t qmask;
	int efd, ret;
	struct ixmap_irq_req req;

	if(core_id >= ih->num_queues){
		goto err_invalid_core_id;
	}

	efd = eventfd(0, 0);
	if(efd < 0)
		goto err_open_efd;

	req.type	= type;
	req.queue_idx	= core_id;
	req.event_fd	= efd;

	ret = ioctl(ih->fd, IXMAP_IRQ, (unsigned long)&req);
	if(ret < 0){
		printf("failed to IXMAP_IRQ\n");
		goto err_irq_assign;
	}

	ret = ixmap_irq_setaffinity(req.vector, core_id);
	if(ret < 0){
		printf("failed to set afinity\n");
		goto err_irq_setaffinity;
	}

	switch(type){
	case IXMAP_IRQ_RX:
		qmask = 1 << core_id;
		break;
	case IXMAP_IRQ_TX:
		qmask = 1 << (core_id + ih->num_queues);
		break;
	default:
		goto err_undefined_type;
	}

	irqh = numa_alloc_onnode(sizeof(struct ixmap_irq_handle),
		numa_node_of_cpu(core_id));
	if(!irqh)
		goto err_alloc_handle;

	irqh->fd		= efd;
	irqh->qmask		= qmask;
	irqh->vector		= req.vector;

	return irqh;

err_alloc_handle:
err_undefined_type:
err_irq_setaffinity:
err_irq_assign:
	close(efd);
err_open_efd:
err_invalid_core_id:
	return NULL;
}

static void ixmap_irq_close(struct ixmap_irq_handle *irqh)
{
	close(irqh->fd);
	numa_free(irqh, sizeof(struct ixmap_irq_handle));

	return;
}

static int ixmap_irq_setaffinity(unsigned int vector, unsigned int core_id)
{
	FILE *file;
	char filename[FILENAME_SIZE];
	uint32_t mask_low, mask_high;
	int ret;

	mask_low = core_id <= 31 ? 1 << core_id : 0;
	mask_high = core_id <= 31 ? 0 : 1 << (core_id - 31);

	snprintf(filename, sizeof(filename),
		"/proc/irq/%d/smp_affinity", vector);
	file = fopen(filename, "w");
	if(!file){
		printf("failed to open smp_affinity\n");
		goto err_open_proc;
	}

	ret = fprintf(file, "%08x,%08x", mask_high, mask_low);
	if(ret < 0){
		printf("failed to set affinity\n");
		goto err_set_affinity;
	}

	fclose(file);
	return 0;

err_set_affinity:
	fclose(file);
err_open_proc:
	return -1;
}

int ixmap_irq_fd(struct ixmap_plane *plane, unsigned int port_index,
	enum ixmap_irq_type type)
{
	struct ixmap_port *port;
	struct ixmap_irq_handle *irqh;

	port = &plane->ports[port_index];

	switch(type){
	case IXMAP_IRQ_RX:
		irqh = port->rx_irq;
		break;
	case IXMAP_IRQ_TX:
		irqh = port->tx_irq;
		break;
	default:
		goto err_undefined_type;
	}

	return irqh->fd;

err_undefined_type:
	return -1;
}

struct ixmap_irq_handle *ixmap_irq_handle(struct ixmap_plane *plane,
	unsigned int port_index, enum ixmap_irq_type type)
{
	struct ixmap_port *port;
	struct ixmap_irq_handle *irqh;

	port = &plane->ports[port_index];

	switch(type){
	case IXMAP_IRQ_RX:
		irqh = port->rx_irq;
		break;
	case IXMAP_IRQ_TX:
		irqh = port->tx_irq;
		break;
	default:
		goto err_undefined_type;
	}

	return irqh;

err_undefined_type:
	return NULL;
}
