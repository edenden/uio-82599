#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/epoll.h>

#include <net/ethernet.h>
#include <pthread.h>

#include "main.h"
#include "forward.h"
#include "descring.h"

static int epoll_add(int fd_ep, int fd);

void *process_interrupt(void *data)
{
	struct ixgbe_thread *thread = data;
	struct epoll_event events[EPOLL_MAXEVENTS];
	int fd_ep, fd_intrx, fd_inttx, i, num_fd;
	char filename[FILENAME_SIZE];
	uint64_t qmask;

	/* Rx interrupt fd preparing */
	snprintf(filename, sizeof(filename),
		"/dev/%s-intrx%d", thread->int_name, thread->index);
	fd_intrx = open(filename, O_RDWR);
        if (fd_intrx < 0)
		return NULL;

        /* Tx interrupt fd preparing */
        snprintf(filename, sizeof(filename),
		"/dev/%s-inttx%d", thread->int_name, thread->index);
        fd_inttx = open(filename, O_RDWR);
        if (fd_inttx < 0)
                return NULL;

	/* epoll fd preparing */
	fd_ep = epoll_create(EPOLL_MAXEVENTS);
	if(fd_ep < 0){
		perror("failed to make epoll fd");
		return NULL;
	}

	if(epoll_add(fd_ep, fd_intrx) != 0){
		perror("failed to add fd in epoll");
		return NULL;
	}

	if(epoll_add(fd_ep, fd_inttx) != 0){
		perror("failed to add fd in epoll");
		return NULL;
	}

	ixgbe_alloc_rx_buffers(ring, ixgbe_desc_unused(ring));
	qmask = 1 << thread->index;

	while(1){
		num_fd = epoll_wait(fd_ep, events, EPOLL_MAXEVENTS, -1);
		if(num_fd <= 0){
			perror("epoll_wait");
			break;
		}

		for(i = 0; i < num_fd; i++){
			if(events[i].data.fd == fd_intrx){
				/* Rx descripter cleaning */

				ixgbe_clean_rx_irq(ring, budget);
				ixgbe_irq_enable_queues(ih, qmask);

			}else if(events[i].data.fd == fd_inttx){
				/* Tx descripter cleaning */

				ixgbe_clean_tx_irq(ring, budget);
				ixgbe_irq_enable_queues(ih, qmask);

			}
		}
	}

	return NULL;
}

static void ixgbe_irq_enable_queues(struct ixgbe_handle *ih, uint64_t qmask)
{
        u32 mask;

	mask = (qmask & 0xFFFFFFFF);
	if (mask)
		IXGBE_WRITE_REG(hw, IXGBE_EIMS_EX(0), mask);
	mask = (qmask >> 32);
	if (mask)
		IXGBE_WRITE_REG(hw, IXGBE_EIMS_EX(1), mask);
	return;
}

void ixgbe_alloc_rx_buffers(struct ixgbe_ring *rx_ring, u16 cleaned_count)
{
        union ixgbe_adv_rx_desc *rx_desc;
	dma_addr_t addr_dma;
        uint16_t i = rx_ring->next_to_use;

        /* nothing to do */
        if (!cleaned_count)
                return;

        rx_desc = IXGBE_RX_DESC(rx_ring, i);

	/*
	 * To know that rx_desc arrives rear of descripter buffer.
	 * (For unlikely(!i)... process)
	 */
        i -= rx_ring->count;

        do {
		addr_dma = ixgbe_assign_buffer(rx_ring);
		if(!addr_dma)
                        break;

                /*
                 * Refresh the desc even if buffer_addrs didn't change
                 * because each write-back erases this info.
                 */
                rx_desc->read.pkt_addr = cpu_to_le64(addr_dma);
                rx_desc++;
                i++;
                if (unlikely(!i)) {
                        rx_desc = IXGBE_RX_DESC(rx_ring, 0);
                        i -= rx_ring->count;
                }

                /* clear the hdr_addr for the next_to_use descriptor */
                rx_desc->read.hdr_addr = 0;

                cleaned_count--;
        } while (cleaned_count);

        i += rx_ring->count;

        if (rx_ring->next_to_use != i){
		rx_ring->next_to_use = i;
		/*
		 * Force memory writes to complete before letting h/w
		 * know there are new descriptors to fetch.  (Only
		 * applicable for weak-ordered memory model archs,
		 * such as IA-64).
		 */
		wmb();
		ixgbe_write_tail(rx_ring, i);
	}
}

static int ixgbe_clean_rx_irq(struct ixgbe_ring *rx_ring,
	int budget)
{
        unsigned int total_rx_packets = 0;
        uint16_t cleaned_count = ixgbe_desc_unused(rx_ring);
	uint32_t next_to_clean;

        do {
                union ixgbe_adv_rx_desc *rx_desc;

                /* return some buffers to hardware, one at a time is too slow */
                if (cleaned_count >= IXGBE_RX_BUFFER_WRITE) {
                        ixgbe_alloc_rx_buffers(rx_ring, cleaned_count);
                        cleaned_count = 0;
                }

                rx_desc = IXGBE_RX_DESC(rx_ring, rx_ring->next_to_clean);

                if (!ixgbe_test_staterr(rx_desc, IXGBE_RXD_STAT_DD))
                        break;

                if(!ixgbe_test_staterr(rx_desc, IXGBE_RXD_STAT_EOP))
                        break;

                /*
                 * This memory barrier is needed to keep us from reading
                 * any other fields out of the rx_desc until we know the
                 * RXD_STAT_DD bit is set
                 */
                rmb();

                /* retrieve a buffer from the ring */
                skb = ixgbe_fetch_rx_buffer(rx_ring, rx_desc);

                /* exit if we failed to retrieve a buffer */
                if (!skb)
                        break;

                cleaned_count++;
		next_to_clean = rx_ring->next_to_clean + 1;
		rx_ring->next_to_clean = 
			(next_to_clean < rx_ring->count) ? next_to_clean : 0;

		/* verify that the packet does not have any known errors */
		if (unlikely(ixgbe_test_staterr(rx_desc,
			IXGBE_RXDADV_ERR_FRAME_ERR_MASK))) {
			continue;
		}

                /* update budget accounting */
                total_rx_packets++;
        } while (likely(total_rx_packets < budget));

        if (cleaned_count)
                ixgbe_alloc_rx_buffers(rx_ring, cleaned_count);

        return total_rx_packets;
}

static int ixgbe_clean_tx_irq(struct ixgbe_ring *tx_ring,
	int budget)
{
        union ixgbe_adv_tx_desc *tx_desc;
        unsigned int total_tx_packets = 0;
        unsigned int i = tx_ring->next_to_clean;

        tx_desc = IXGBE_TX_DESC(tx_ring, i);
        i -= tx_ring->count;

	union ixgbe_adv_tx_desc *eop_desc;
	eop_desc = IXGBE_TX_DESC(tx_ring,
		tx_ring->next_to_use ? tx_ring->next_to_use - 1 : tx_ring->count - 1);

	/* if DD is not set pending work has not been completed */
	if (!(eop_desc->wb.status & cpu_to_le32(IXGBE_TXD_STAT_DD)))
		goto out;

	do {
		/* free the skb */
		dev_kfree_skb_any(tx_buffer->skb);

		if(tx_desc == eop_desc)
			break;

		tx_desc++;
		i++;
		if(unlikely(!i)){
			i -= tx_ring->count;
			tx_desc = IXGBE_TX_DESC(tx_ring, 0);
		}

		total_tx_packets++;
	} while (likely(total_tx_packets < budget));

	i += tx_ring->count;
	tx_ring->next_to_clean = i;

out:
        return total_tx_packets;
}

static int epoll_add(int fd_ep, int fd)
{
	struct epoll_event event;

	memset(&event, 0, sizeof(struct epoll_event));
	event.events = EPOLLIN;
	event.data.fd = fd;
	return epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd, &event);
}

