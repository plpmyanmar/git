#ifndef SPARSE_INDEX_H__
#define SPARSE_INDEX_H__

struct index_state;
#define SPARSE_INDEX_MEMORY_ONLY (1 << 0)
int convert_to_sparse(struct index_state *istate, int flags);

/**
 * Some places in the codebase expect to search for a specific path.
 * This path might be outside of the sparse-checkout definition, in
 * which case a sparse-index may not contain a path for that index.
 *
 * Given an index and a path, check to see if a leading directory for
 * 'path' exists in the index as a sparse directory. In that case,
 * expand that sparse directory to a full range of cache entries and
 * populate the index accordingly.
 */
void expand_to_path(struct index_state *istate,
		    const char *path, size_t pathlen, int icase);

struct repository;
int set_sparse_index_config(struct repository *repo, int enable);

struct pattern_list;

/**
 * Scan the given index and compare its entries to the given pattern list.
 * If the index is sparse and the pattern list uses cone mode patterns,
 * then modify the index to contain the all of the file entries within that
 * new pattern list. This expands sparse directories only as far as needed.
 *
 * If the pattern list is NULL or does not use cone mode patterns, then the
 * index is expanded to a full index.
 */
void expand_to_pattern_list(struct index_state *istate,
			      struct pattern_list *pl);

#endif
