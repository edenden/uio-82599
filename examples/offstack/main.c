#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <endian.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <pthread.h>
#include <ixmap.h>

#include "linux/list.h"
#include "main.h"
#include "thread.h"

static int ixmapfwd_thread_create(struct ixmapfwd *ixmapfwd,
	struct ixmapfwd_thread *thread, int thread_index);
static void ixmapfwd_thread_kill(struct ixmapfwd_thread *thread);
static int ixmapfwd_set_signal(sigset_t *sigset);

static int buf_count = 32768; // per port number of slots
static char *ixmap_interface_array[2];

int main(int argc, char **argv)
{
	struct ixmapfwd		ixmapfwd;
	struct ixmapfwd_thread	*threads;
	int			ret, i, signal;
	int			cores_assigned = 0,
				ports_assigned = 0,
				tun_assigned = 0,
				desc_assigned = 0;
	sigset_t		sigset;

	ixmap_interface_array[0] = "ixgbe0";
	ixmap_interface_array[1] = "ixgbe1";

	ixmapfwd.buf_size = 0;
	ixmapfwd.num_cores = 4;
	ixmapfwd.num_ports = 2;
	ixmapfwd.promisc = 1;
	ixmapfwd.mtu_frame = 0; /* MTU=1522 is used by default. */
	ixmapfwd.intr_rate = IXGBE_20K_ITR;

	ixmapfwd.ih_array = malloc(sizeof(struct ixmap_handle *) * ixmapfwd.num_ports);
	if(!ixmapfwd.ih_array){
		ret = -1;
		goto err_ih_array;
	}

	ixmapfwd.tunh_array = malloc(sizeof(struct tun_handle *) * ixmapfwd.num_ports);
	if(!ixmapfwd.tunh_array){
		ret = -1;
		goto err_tunh_array;
	}

	threads = malloc(sizeof(struct ixmapfwd_thread) * ixmapfwd.num_cores);
	if(!threads){
		ret = -1;
		goto err_alloc_threads;
	}

	for(i = 0; i < ixmapfwd.num_ports; i++, ports_assigned++){
		ixmapfwd.ih_array[i] = ixmap_open(ixmap_interface_array[i],
			ixmapfwd.num_cores, ixmapfwd.intr_rate,
			IXMAP_RX_BUDGET, IXMAP_TX_BUDGET,
			ixmapfwd.mtu_frame, ixmapfwd.promisc,
			IXGBE_MAX_RXD, IXGBE_MAX_TXD);
		if(!ixmapfwd.ih_array[i]){
			printf("failed to ixmap_open, idx = %d\n", i);
			ret = -1;
			goto err_open;
		}
	}

	for(i = 0; i < ixmapfwd.num_cores; i++, desc_assigned++){
		threads[i].desc = ixmap_desc_alloc(ixmapfwd.ih_array,
			ixmapfwd.num_ports, i);
		if(!threads[i].desc){
			printf("failed to ixmap_alloc_descring, idx = %d\n", i);
			printf("please decrease descripter or enable iommu\n");
			ret = -1;
			goto err_desc_alloc;
		}
	}

	for(i = 0; i < ixmapfwd.num_ports; i++){
		ixmap_configure_rx(ixmapfwd.ih_array[i]);
		ixmap_configure_tx(ixmapfwd.ih_array[i]);
		ixmap_irq_enable(ixmapfwd.ih_array[i]);

		/* calclulate maximum buf_size we should prepare */
		if(ixmap_bufsize_get(ixmapfwd.ih_array[i]) > ixmapfwd.buf_size)
			ixmapfwd.buf_size = ixmap_bufsize_get(ixmapfwd.ih_array[i]);
	}

	for(i = 0; i < ixmapfwd.num_ports; i++, tun_assigned++){
		ixmapfwd.tunh_array[i] = tun_open(&ixmapfwd, ixmap_interface_array[i], i);
		if(!ixmapfwd.tunh_array[i]){
			printf("failed to tun_open\n");
			ret = -1;
			goto err_tun_open;
		}
	}

	ret = ixmapfwd_set_signal(&sigset);
	if(ret != 0){
		goto err_set_signal;
	}

	for(i = 0; i < ixmapfwd.num_cores; i++, cores_assigned++){
		threads[i].buf = ixmap_buf_alloc(ixmapfwd.ih_array,
			ixmapfwd.num_ports, buf_count, ixmapfwd.buf_size);
		if(!threads[i].buf){
			printf("failed to ixmap_alloc_buf, idx = %d\n", i);
			printf("please decrease buffer or enable iommu\n");
			goto err_buf_alloc;
		}

		threads[i].plane = ixmap_plane_alloc(ixmapfwd.ih_array,
			threads[i].buf, ixmapfwd.num_ports, i);
		if(!threads[i].plane){
			printf("failed to ixmap_plane_alloc, idx = %d\n", i);
			goto err_plane_alloc;
		}

		threads[i].tun_plane = tun_plane_alloc(&ixmapfwd, i);
		if(!threads[i].tun_plane)
			goto err_tun_plane_alloc;

		ret = ixmapfwd_thread_create(&ixmapfwd, &threads[i], i);
		if(ret < 0){
			goto err_thread_create;
		}

		continue;

err_thread_create:
		tun_plane_release(threads[i].tun_plane);
err_tun_plane_alloc:
		ixmap_plane_release(threads[i].plane);
err_plane_alloc:
		ixmap_buf_release(threads[i].buf,
			ixmapfwd.ih_array, ixmapfwd.num_ports);
err_buf_alloc:
		ret = -1;
		goto err_assign_cores;
	}

	while(1){
		if(sigwait(&sigset, &signal) == 0){
			break;
		}
	}
	ret = 0;

err_assign_cores:
	for(i = 0; i < cores_assigned; i++){
		ixmapfwd_thread_kill(&threads[i]);
		tun_plane_release(threads[i].tun_plane);
		ixmap_plane_release(threads[i].plane);
		ixmap_buf_release(threads[i].buf,
			ixmapfwd.ih_array, ixmapfwd.num_ports);
	}
err_set_signal:
err_tun_open:
	for(i = 0; i < tun_assigned; i++){
		tun_close(&ixmapfwd, i);
	}
err_desc_alloc:
	for(i = 0; i < desc_assigned; i++){
		ixmap_desc_release(ixmapfwd.ih_array,
			ixmapfwd.num_ports, i, threads[i].desc);
	}
err_open:
	for(i = 0; i < ports_assigned; i++){
		ixmap_close(ixmapfwd.ih_array[i]);
	}
	free(threads);
err_alloc_threads:
	free(ixmapfwd.tunh_array);
err_tunh_array:
	free(ixmapfwd.ih_array);
err_ih_array:
	return ret;
}

static int ixmapfwd_thread_create(struct ixmapfwd *ixmapfwd,
	struct ixmapfwd_thread *thread, int thread_index)
{
	cpu_set_t cpuset;
	int ret;

	thread->index		= thread_index;
	thread->num_ports	= ixmapfwd->num_ports;
	thread->ptid		= pthread_self();

	ret = pthread_create(&thread->tid, NULL, thread_process_interrupt, thread);
	if(ret < 0){
		perror("failed to create thread");
		goto err_pthread_create;
	}

	CPU_ZERO(&cpuset);
	CPU_SET(thread->index, &cpuset);
	ret = pthread_setaffinity_np(thread->tid, sizeof(cpu_set_t), &cpuset);
	if(ret < 0){
		perror("failed to set affinity");
		goto err_set_affinity;
	}

	return 0;

err_set_affinity:
	ixmapfwd_thread_kill(thread);
err_pthread_create:
	return -1;
}

static void ixmapfwd_thread_kill(struct ixmapfwd_thread *thread)
{
	int ret;

	ret = pthread_kill(thread->tid, SIGUSR1);
	if(ret != 0)
		perror("failed to kill thread");

	ret = pthread_join(thread->tid, NULL);
	if(ret != 0)
		perror("failed to join thread");

	return;
}

static int ixmapfwd_set_signal(sigset_t *sigset)
{
	int ret;

	sigemptyset(sigset);
	ret = sigaddset(sigset, SIGUSR1);
	if(ret != 0)
		return -1;

	ret = sigaddset(sigset, SIGHUP);
	if(ret != 0)
		return -1;

	ret = sigaddset(sigset, SIGINT);
	if(ret != 0)
		return -1;

	ret = sigaddset(sigset, SIGTERM);
	if(ret != 0)
		return -1;

	ret = pthread_sigmask(SIG_BLOCK, sigset, NULL);
	if(ret != 0)
		return -1;

	return 0;
}

