#include "cache.h"
#include "git-compat-util.h"
#include "pack.h"
#include "packfile.h"
#include "midx.h"

#define MIDX_LARGE_OFFSET_NEEDED 0x80000000

struct object_id *get_midx_head_oid(const char *pack_dir, struct object_id *oid)
{
	struct strbuf head_filename = STRBUF_INIT;
	char oid_hex[GIT_MAX_HEXSZ + 1];
	FILE *f;

	strbuf_addstr(&head_filename, pack_dir);
	strbuf_addstr(&head_filename, "/midx-head");

	f = fopen(head_filename.buf, "r");
	strbuf_release(&head_filename);

	if (!f)
		return 0;

	if (!fgets(oid_hex, sizeof(oid_hex), f))
		die("Failed to read midx-head");

	fclose(f);

	if (get_oid_hex(oid_hex, oid))
		return 0;
	return oid;
}

struct pack_midx_details_internal {
	uint32_t pack_int_id;
	uint32_t internal_offset;
};

static struct midxed_git *alloc_midxed_git(int extra)
{
	struct midxed_git *m = xmalloc(st_add(sizeof(*m), extra));
	memset(m, 0, sizeof(*m));
	m->midx_fd = -1;

	return m;
}

static struct midxed_git *load_midxed_git_one(const char *midx_file, const char *pack_dir)
{
	void *midx_map;
	const unsigned char *data;
	struct pack_midx_header *hdr;
	size_t midx_size, packs_len;
	struct stat st;
	uint32_t i;
	struct midxed_git *midx;
	int fd = git_open(midx_file);

	if (fd < 0)
		return 0;
	if (fstat(fd, &st)) {
		close(fd);
		return 0;
	}
	midx_size = xsize_t(st.st_size);

	if (midx_size < 16 + 8 * 5 + 4 * 256 + GIT_MAX_RAWSZ) {
		close(fd);
		die("midx file %s is too small", midx_file);
	}
	midx_map = xmmap(NULL, midx_size, PROT_READ, MAP_PRIVATE, fd, 0);
	data = (const unsigned char *)midx_map;

	hdr = midx_map;
	if (ntohl(hdr->midx_signature) != MIDX_SIGNATURE) {
		munmap(midx_map, midx_size);
		die("MIDX signature %X does not match signature %X",
		    ntohl(hdr->midx_signature), MIDX_SIGNATURE);
	}

	if (ntohl(hdr->midx_version) != MIDX_VERSION)
		die("MIDX version %X does not match version %X",
		    ntohl(hdr->midx_version), MIDX_VERSION);

	/* Time to fill a midx struct */
	midx = alloc_midxed_git(strlen(pack_dir) + 1);

	midx->hdr = hdr;
	midx->midx_fd = fd;
	midx->data = midx_map;
	midx->data_len = midx_size;

	/* read chunk ids to find pointers */
	for (i = 0; i <= hdr->num_chunks; i++) {
		uint32_t chunk_id = ntohl(*(uint32_t*)(data + sizeof(*hdr) + 12 * i));
		uint64_t chunk_offset1 = ntohl(*(uint32_t*)(data + sizeof(*hdr) + 12 * i + 4));
		uint32_t chunk_offset2 = ntohl(*(uint32_t*)(data + sizeof(*hdr) + 12 * i + 8));
		uint64_t chunk_offset = (chunk_offset1 << 32) | chunk_offset2;

		if (sizeof(data) == 4 && chunk_offset >> 32)
			die(_("unable to memory-map in 32-bit address space"));

		switch (chunk_id) {
			case MIDX_CHUNKID_PACKLOOKUP:
				midx->chunk_pack_lookup = data + chunk_offset;
				break;

			case MIDX_CHUNKID_PACKNAMES:
				midx->chunk_pack_names = data + chunk_offset;
				break;

			case MIDX_CHUNKID_OIDFANOUT:
				midx->chunk_oid_fanout = data + chunk_offset;
				break;

			case MIDX_CHUNKID_OIDLOOKUP:
				midx->chunk_oid_lookup = data + chunk_offset;
				break;

			case MIDX_CHUNKID_OBJECTOFFSETS:
				midx->chunk_object_offsets = data + chunk_offset;
				break;

			case MIDX_CHUNKID_LARGEOFFSETS:
				midx->chunk_large_offsets = data + chunk_offset;
				break;

			case 0:
				break;

			default:
				die("Unrecognized MIDX chunk id: %08x", chunk_id);
		}
	}

	midx->num_objects = ntohl(*((uint32_t*)(midx->chunk_oid_fanout + 255 * 4)));
	midx->num_packs = ntohl(midx->hdr->num_packs);

	packs_len = st_mult(sizeof(struct packed_git*), midx->num_packs);

	if (packs_len) {
		ALLOC_ARRAY(midx->packs, midx->num_packs);
		ALLOC_ARRAY(midx->pack_names, midx->num_packs);
		memset(midx->packs, 0, packs_len);

		for (i = 0; i < midx->num_packs; i++) {
			uint32_t name_offset = ntohl(*(uint32_t*)(midx->chunk_pack_lookup + 4 * i));
			midx->pack_names[i] = (const char*)(midx->chunk_pack_names + name_offset);
		}
	}

	strcpy(midx->pack_dir, pack_dir);
	return midx;
}

struct midxed_git *get_midxed_git(const char *pack_dir, struct object_id *midx_oid)
{
	struct midxed_git *m;
	struct strbuf midx_file = STRBUF_INIT;
	strbuf_addstr(&midx_file, pack_dir);
	strbuf_addstr(&midx_file, "/midx-");
	strbuf_addstr(&midx_file, oid_to_hex(midx_oid));
	strbuf_addstr(&midx_file, ".midx");

	m = load_midxed_git_one(midx_file.buf, pack_dir);
	strbuf_release(&midx_file);
	return m;
}

struct pack_midx_details *nth_midxed_object_details(struct midxed_git *m,
						    uint32_t n,
						    struct pack_midx_details *d)
{
	struct pack_midx_details_internal *d_internal;
	const unsigned char *details = m->chunk_object_offsets;

	if (n >= m->num_objects) {
		return NULL;
	}

	d_internal = (struct pack_midx_details_internal*)(details + 8 * n);
	d->pack_int_id = ntohl(d_internal->pack_int_id);
	d->offset = ntohl(d_internal->internal_offset);

	if (m->chunk_large_offsets && d->offset & MIDX_LARGE_OFFSET_NEEDED) {
		uint32_t large_offset = d->offset ^ MIDX_LARGE_OFFSET_NEEDED;
		const unsigned char *large_offsets = m->chunk_large_offsets + 8 * large_offset;

		d->offset =  (((uint64_t)ntohl(*((uint32_t *)(large_offsets + 0)))) << 32) |
					 ntohl(*((uint32_t *)(large_offsets + 4)));
	}

	return d;
}

struct pack_midx_entry *nth_midxed_object_entry(struct midxed_git *m,
						uint32_t n,
						struct pack_midx_entry *e)
{
	struct pack_midx_details details;
	const unsigned char *index = m->chunk_oid_lookup;

	if (!nth_midxed_object_details(m, n, &details))
		return NULL;

	memcpy(e->oid.hash, index + m->hdr->hash_len * n, m->hdr->hash_len);
	e->pack_int_id = details.pack_int_id;
	e->offset = details.offset;

	return e;
}

int contains_pack(struct midxed_git *m, const char *pack_name)
{
	uint32_t first = 0, last = m->num_packs;

	while (first < last) {
		uint32_t mid = first + (last - first) / 2;
		const char *current;
		int cmp;

		current = m->pack_names[mid];
		cmp = strcmp(pack_name, current);
		if (!cmp)
			return 1;
		if (cmp > 0) {
			first = mid + 1;
			continue;
		}
		last = mid;
	}

	return 0;
}

static int midx_oid_compare(const void *_a, const void *_b)
{
	struct pack_midx_entry *a = *(struct pack_midx_entry **)_a;
	struct pack_midx_entry *b = *(struct pack_midx_entry **)_b;
	return oidcmp(&a->oid, &b->oid);
}

static void write_midx_chunk_packlookup(
	struct hashfile *f,
	const char **pack_names, uint32_t nr_packs)
{
	uint32_t i, cur_len = 0;

	for (i = 0; i < nr_packs; i++) {
		uint32_t swap_len = htonl(cur_len);
		hashwrite(f, &swap_len, 4);
		cur_len += strlen(pack_names[i]) + 1;
	}
}

static void write_midx_chunk_packnames(
	struct hashfile *f,
	const char **pack_names, uint32_t nr_packs)
{
	uint32_t i;
	for (i = 0; i < nr_packs; i++) {
		hashwrite(f, pack_names[i], strlen(pack_names[i]) + 1);
	}
}

static void write_midx_chunk_oidfanout(
	struct hashfile *f,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct pack_midx_entry **list = objects;
	struct pack_midx_entry **last = objects + nr_objects;
	uint32_t count_distinct = 0;
	uint32_t i;

	/*
	* Write the first-level table (the list is sorted,
	* but we use a 256-entry lookup to be able to avoid
	* having to do eight extra binary search iterations).
	*/
	for (i = 0; i < 256; i++) {
		struct pack_midx_entry **next = list;
		struct pack_midx_entry *prev = 0;
		uint32_t swap_distinct;

		while (next < last) {
			struct pack_midx_entry *obj = *next;
			if (obj->oid.hash[0] != i)
				break;

			if (!prev || oidcmp(&(prev->oid), &(obj->oid)))
			{
				count_distinct++;
			}

			prev = obj;
			next++;
		}

		swap_distinct = htonl(count_distinct);
		hashwrite(f, &swap_distinct, 4);
		list = next;
	}
}

static void write_midx_chunk_oidlookup(
	struct hashfile *f, unsigned char hash_len,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct pack_midx_entry **list = objects;
	struct object_id *last_oid = 0;
	uint32_t i;

	for (i = 0; i < nr_objects; i++) {
		struct pack_midx_entry *obj = *list++;

		if (last_oid && !oidcmp(last_oid, &obj->oid))
			continue;

		last_oid = &obj->oid;
		hashwrite(f, obj->oid.hash, (int)hash_len);
	}
}

static void write_midx_chunk_objectoffsets(
	struct hashfile *f, int large_offset_needed,
	struct pack_midx_entry **objects, uint32_t nr_objects, uint32_t *pack_perm)
{
	struct pack_midx_entry **list = objects;
	struct object_id *last_oid = 0;
	uint32_t i, nr_large_offset = 0;

	for (i = 0; i < nr_objects; i++) {
		struct pack_midx_details_internal details;
		struct pack_midx_entry *obj = *list++;

		if (last_oid && !oidcmp(last_oid, &obj->oid))
			continue;

		last_oid = &obj->oid;

		details.pack_int_id = htonl(pack_perm[obj->pack_int_id]);

		if (large_offset_needed && obj->offset >> 31)
			details.internal_offset = (MIDX_LARGE_OFFSET_NEEDED | nr_large_offset++);
		else
			details.internal_offset = (uint32_t)obj->offset;

		details.internal_offset = htonl(details.internal_offset);
		hashwrite(f, &details, 8);
	}
}

static void write_midx_chunk_largeoffsets(
	struct hashfile *f, uint32_t nr_large_offset,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct pack_midx_entry **list = objects;
	struct object_id *last_oid = 0;

	while (nr_large_offset) {
		struct pack_midx_entry *obj = *list++;
		uint64_t offset = obj->offset;
		uint32_t split[2];

		if (last_oid && !oidcmp(last_oid, &obj->oid))
			continue;

		last_oid = &obj->oid;

		if (!(offset >> 31))
			continue;

		split[0] = htonl(offset >> 32);
		split[1] = htonl(offset & 0xffffffff);

		hashwrite(f, split, 8);
		nr_large_offset--;
	}
}

struct pack_pair {
	uint32_t pack_int_id;
	const char *pack_name;
};

static int pack_pair_compare(const void *_a, const void *_b)
{
	struct pack_pair *a = (struct pack_pair *)_a;
	struct pack_pair *b = (struct pack_pair *)_b;
	return strcmp(a->pack_name, b->pack_name);
}

static void sort_packs_by_name(const char **pack_names, uint32_t nr_packs, uint32_t *perm)
{
	uint32_t i;
	struct pack_pair *pairs;

	ALLOC_ARRAY(pairs, nr_packs);

	for (i = 0; i < nr_packs; i++) {
		pairs[i].pack_int_id = i;
		pairs[i].pack_name = pack_names[i];
	}

	QSORT(pairs, nr_packs, pack_pair_compare);

	for (i = 0; i < nr_packs; i++) {
		pack_names[i] = pairs[i].pack_name;
		perm[pairs[i].pack_int_id] = i;
	}
}

const char *write_midx_file(
	const char *pack_dir,
	const char *midx_name,
	const char **pack_names,          uint32_t nr_packs,
	struct pack_midx_entry **objects, uint32_t nr_objects)
{
	struct hashfile *f;
	struct pack_midx_entry **sorted_by_sha;
	int i, chunk, fd;
	struct pack_midx_header hdr;
	uint32_t chunk_ids[7];
	uint64_t chunk_offsets[7];
	unsigned char large_offset_needed = 0;
	unsigned int nr_large_offset = 0;
	unsigned char final_hash[GIT_MAX_RAWSZ];
	const char *final_hex;
	int rename_needed = 0;
	uint32_t count_distinct = 0;
	int total_name_len = 0;
	uint32_t *pack_perm;

	if (!core_midx)
		return 0;

	/* Sort packs */
	if (nr_packs) {
		ALLOC_ARRAY(pack_perm, nr_packs);
		sort_packs_by_name(pack_names, nr_packs, pack_perm);
	} else {
		pack_perm = 0;
	}

	/* Sort objects */
	if (nr_objects) {
		sorted_by_sha = objects;

		QSORT(sorted_by_sha, nr_objects, midx_oid_compare);

		count_distinct = 1;
		for (i = 0; i < nr_objects; i++) {
			if (!i ||
			    !oidcmp(&sorted_by_sha[i-1]->oid, &sorted_by_sha[i]->oid))
				continue;

			count_distinct++;

			if (sorted_by_sha[i]->offset >> 31)
				nr_large_offset++;
			if (sorted_by_sha[i]->offset >> 32)
				large_offset_needed = 1;
		}
	} else {
		sorted_by_sha = NULL;
	}

	if (nr_packs) {
		for (i = 0; i < nr_packs; i++) {
			total_name_len += strlen(pack_names[i]) + 1;
		}
	}

	/* open temp file, or direct file if given */
	if (!midx_name) {
		struct strbuf tmp_file = STRBUF_INIT;
		strbuf_addstr(&tmp_file, pack_dir);
		strbuf_addstr(&tmp_file, "/tmp_midx_XXXXXX");

		fd = git_mkstemp_mode(tmp_file.buf, 0444);
		if (fd < 0)
			die_errno("unable to create '%s'", tmp_file.buf);

		midx_name = strbuf_detach(&tmp_file, NULL);
		rename_needed = 1;
	} else {
		unlink(midx_name);
		fd = open(midx_name, O_CREAT|O_EXCL|O_WRONLY, 0600);
		if (fd < 0)
			die_errno("unable to create '%s'", midx_name);
	}
	f = hashfd(fd, midx_name);

	/* fill header info */
	hdr.midx_signature = htonl(MIDX_SIGNATURE);
	hdr.midx_version = htonl(MIDX_VERSION);

	hdr.hash_version = MIDX_HASH_VERSION;
	hdr.hash_len = MIDX_HASH_LEN;
	hdr.num_base_midx = 0;
	hdr.num_packs = htonl(nr_packs);

	/*
	 * We expect the following chunks, which are required:
	 *
	 * Packfile Name Lookup
	 * Packfile Names
	 * OID Fanout
	 * OID Lookup
	 * Object Offsets
	 */
	hdr.num_chunks = large_offset_needed ? 6 : 5;

	/* write header to file */
	assert(sizeof(hdr) == 16);
	hashwrite(f, &hdr, sizeof(hdr));

	/*
	 * Fill initial chunk values using offsets
	 * relative to first chunk.
	 */
	chunk_offsets[0] = sizeof(hdr) + 12 * (hdr.num_chunks + 1);
	chunk_ids[0] = MIDX_CHUNKID_PACKLOOKUP;
	chunk_offsets[1] = chunk_offsets[0] + nr_packs * 4;
	chunk_ids[1] = MIDX_CHUNKID_OIDFANOUT;
	chunk_offsets[2] = chunk_offsets[1] + 256 * 4;
	chunk_ids[2] = MIDX_CHUNKID_OIDLOOKUP;
	chunk_offsets[3] = chunk_offsets[2] + (uint64_t)count_distinct
					    * (uint64_t)hdr.hash_len;
	chunk_ids[3] = MIDX_CHUNKID_OBJECTOFFSETS;
	chunk_offsets[4] = chunk_offsets[3] + 8 * (uint64_t)count_distinct;

	if (large_offset_needed) {
		chunk_ids[4] = MIDX_CHUNKID_LARGEOFFSETS;
		chunk_offsets[5] = chunk_offsets[4] + 8 * (uint64_t)nr_large_offset;
		chunk_ids[4] = MIDX_CHUNKID_PACKNAMES;
		chunk_offsets[6] = chunk_offsets[5] + total_name_len;
		chunk_ids[6] = 0;
	} else {
		chunk_ids[4] = MIDX_CHUNKID_PACKNAMES;
		chunk_offsets[5] = chunk_offsets[4] + total_name_len;
		chunk_ids[5] = 0;
	}

	for (i = 0; i <= hdr.num_chunks; i++) {
		uint32_t chunk_write[3];

		chunk_write[0] = htonl(chunk_ids[i]);
		chunk_write[1] = htonl(chunk_offsets[i] >> 32);
		chunk_write[2] = htonl(chunk_offsets[i] & 0xffffffff);
		hashwrite(f, chunk_write, 12);
	}

	for (chunk = 0; chunk < hdr.num_chunks; chunk++) {
		switch (chunk_ids[chunk]) {
		case MIDX_CHUNKID_PACKLOOKUP:
			write_midx_chunk_packlookup(f, pack_names, nr_packs);
			break;

		case MIDX_CHUNKID_PACKNAMES:
			write_midx_chunk_packnames(f, pack_names, nr_packs);
			break;

		case MIDX_CHUNKID_OIDFANOUT:
			write_midx_chunk_oidfanout(f, sorted_by_sha, nr_objects);
			break;

		case MIDX_CHUNKID_OIDLOOKUP:
			write_midx_chunk_oidlookup(f, hdr.hash_len, sorted_by_sha,
						   nr_objects);
			break;

		case MIDX_CHUNKID_OBJECTOFFSETS:
			write_midx_chunk_objectoffsets(f, large_offset_needed,
						       sorted_by_sha, nr_objects,
						       pack_perm);
			break;

		case MIDX_CHUNKID_LARGEOFFSETS:
			write_midx_chunk_largeoffsets(f, nr_large_offset,
						      sorted_by_sha, nr_objects);
			break;

		case 0:
			break;

		default:
			die("Unrecognized MIDX chunk id: %08x", chunk_ids[chunk]);
		}
	}

	hashclose(f, final_hash, CSUM_CLOSE);

	if (rename_needed)
	{
		struct strbuf final_name = STRBUF_INIT;

		final_hex = sha1_to_hex(final_hash);
		strbuf_addstr(&final_name, pack_dir);
		strbuf_addstr(&final_name, "/midx-");
		strbuf_addstr(&final_name, final_hex);
		strbuf_addstr(&final_name, ".midx");

		if (rename(midx_name, final_name.buf))
			die("Failed to rename %s to %s", midx_name, final_name.buf);

		strbuf_release(&final_name);
	} else {
		final_hex = midx_name;
	}

	return final_hex;
}
