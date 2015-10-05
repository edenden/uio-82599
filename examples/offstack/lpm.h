#ifndef _IXMAPFWD_LPM_H
#define _IXMAPFWD_LPM_H

#include "linux/list.h"

#define TABLE_SIZE_24 (1 << 24)
#define TABLE_SIZE_8 (1 << 8)

struct lpm_entry {
	struct list_head	list;
	void			*ptr;
	struct ixmap_marea	*area;
};

struct lpm_node {
	struct lpm_node		*next_table;
	struct list_head	head;
	struct ixmap_marea	*area;
};

struct lpm_table {
	struct lpm_node		node[TABLE_SIZE_24];
	unsigned int		max_len;
	void			(*entry_dump)(
				struct list_head *
				);
	int			(*entry_identify)(
				void *,
				unsigned int,
				unsigned int
				);
	int			(*entry_compare)(
				void *,
				unsigned int
				);
	void	 		(*entry_pull)(
				void *
				);
	void			(*entry_put)(
				void *
				);
};

void lpm_init(struct lpm_table *table, unsigned int max_len);
struct lpm_entry *lpm_lookup(struct lpm_table *table,
	void *dst);
int lpm_add(struct lpm_table *table, void *prefix,
	unsigned int prefix_len, unsigned int id,
	void *ptr, struct ixmap_desc *desc);
int lpm_delete(struct lpm_table *table, void *prefix,
	unsigned int prefix_len, unsigned int id);
void lpm_delete_all(struct lpm_table *table);
int lpm_traverse(struct lpm_table *table, void *prefix,
	unsigned int prefix_len);

#endif /* _IXMAPFWD_LPM_H */
