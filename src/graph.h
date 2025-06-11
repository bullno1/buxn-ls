#ifndef BUXN_LS_GRAPH_H
#define BUXN_LS_GRAPH_H

#include <barena.h>

typedef struct buxn_ls_edge_s buxn_ls_edge_t;
typedef struct buxn_ls_node_base_s buxn_ls_node_base_t;

struct buxn_ls_edge_s {
	buxn_ls_edge_t* next_in;
	buxn_ls_edge_t* next_out;

	buxn_ls_node_base_t* from;
	buxn_ls_node_base_t* to;
};

struct buxn_ls_node_base_s {
	buxn_ls_edge_t* in_edges;
	buxn_ls_edge_t* out_edges;
};

static inline buxn_ls_edge_t*
buxn_ls_graph_add_edge(
	struct barena_s* arena,
	buxn_ls_node_base_t* from_node,
	buxn_ls_node_base_t* to_node
) {
	buxn_ls_edge_t* edge = barena_memalign(
		arena,
		sizeof(buxn_ls_edge_t), _Alignof(buxn_ls_edge_t)
	);

	edge->from = from_node;
	edge->to = to_node;

	edge->next_out = from_node->out_edges;
	from_node->out_edges = edge;
	edge->next_in = to_node->in_edges;
	to_node->in_edges = edge;

	return edge;
}

#endif
