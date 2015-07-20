struct fib_entry {
	uint32_t		prefix[4];
	unsigned int		prefix_len;
	uint32_t		nexthop[4];
	unsigned int		port_index;
};

struct fib {
	struct trie_node	*trie_node;
};