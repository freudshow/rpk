/*
 * Copyright (C) 2017, ARM
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * This file implements parsing of Processor Properties Topology Table (PPTT)
 * which is optionally used to describe the processor and cache topology.
 * Due to the relative pointers used throughout the table, this doesn't
 * leverage the existing subtable parsing in the kernel.
 *
 * The PPTT structure is an inverted tree, with each node potentially
 * holding one or two inverted tree data structures describing
 * the caches available at that level. Each cache structure optionally
 * contains properties describing the cache at that level which can be
 * used to override hardware/probed values.
 */
#define pr_fmt(fmt) "ACPI PPTT: " fmt

#include <linux/acpi.h>
#include <linux/cacheinfo.h>
#include <acpi/processor.h>

/*
 * Given the PPTT table, find and verify that the subtable entry
 * is located within the table
 */
static struct acpi_subtable_header *fetch_pptt_subtable(
	struct acpi_table_header *table_hdr, u32 pptt_ref)
{
	struct acpi_subtable_header *entry;

	/* there isn't a subtable at reference 0 */
	if (pptt_ref < sizeof(struct acpi_subtable_header))
		return NULL;

	if (pptt_ref + sizeof(struct acpi_subtable_header) > table_hdr->length)
		return NULL;

	entry = ACPI_ADD_PTR(struct acpi_subtable_header, table_hdr, pptt_ref);

	if (pptt_ref + entry->length > table_hdr->length)
		return NULL;

	return entry;
}

static struct acpi_pptt_processor *fetch_pptt_node(
	struct acpi_table_header *table_hdr, u32 pptt_ref)
{
	return (struct acpi_pptt_processor *)fetch_pptt_subtable(table_hdr,
								 pptt_ref);
}

static struct acpi_pptt_cache *fetch_pptt_cache(
	struct acpi_table_header *table_hdr, u32 pptt_ref)
{
	return (struct acpi_pptt_cache *)fetch_pptt_subtable(table_hdr,
							     pptt_ref);
}

static struct acpi_subtable_header *acpi_get_pptt_resource(
	struct acpi_table_header *table_hdr,
	struct acpi_pptt_processor *node, int resource)
{
	u32 *ref;

	if (resource >= node->number_of_priv_resources)
		return NULL;

	ref = ACPI_ADD_PTR(u32, node, sizeof(struct acpi_pptt_processor));
	ref += resource;

	return fetch_pptt_subtable(table_hdr, *ref);
}

/*
 * Attempt to find a given cache level, while counting the max number
 * of cache levels for the cache node.
 *
 * Given a pptt resource, verify that it is a cache node, then walk
 * down each level of caches, counting how many levels are found
 * as well as checking the cache type (icache, dcache, unified). If a
 * level & type match, then we set found, and continue the search.
 * Once the entire cache branch has been walked return its max
 * depth.
 */
static int acpi_pptt_walk_cache(struct acpi_table_header *table_hdr,
				int local_level,
				struct acpi_subtable_header *res,
				struct acpi_pptt_cache **found,
				int level, int type)
{
	struct acpi_pptt_cache *cache;

	if (res->type != ACPI_PPTT_TYPE_CACHE)
		return 0;

	cache = (struct acpi_pptt_cache *) res;
	while (cache) {
		local_level++;

		if ((local_level == level) &&
		    (cache->flags & ACPI_PPTT_CACHE_TYPE_VALID) &&
		    ((cache->attributes & ACPI_PPTT_MASK_CACHE_TYPE) == type)) {
			if ((*found != NULL) && (cache != *found))
				pr_err("Found duplicate cache level/type unable to determine uniqueness\n");

			pr_debug("Found cache @ level %d\n", level);
			*found = cache;
			/*
			 * continue looking at this node's resource list
			 * to verify that we don't find a duplicate
			 * cache node.
			 */
		}
		cache = fetch_pptt_cache(table_hdr, cache->next_level_of_cache);
	}
	return local_level;
}

/*
 * Given a CPU node look for cache levels that exist at this level, and then
 * for each cache node, count how many levels exist below (logically above) it.
 * If a level and type are specified, and we find that level/type, abort
 * processing and return the acpi_pptt_cache structure.
 */
static struct acpi_pptt_cache *acpi_find_cache_level(
	struct acpi_table_header *table_hdr,
	struct acpi_pptt_processor *cpu_node,
	int *starting_level, int level, int type)
{
	struct acpi_subtable_header *res;
	int number_of_levels = *starting_level;
	int resource = 0;
	struct acpi_pptt_cache *ret = NULL;
	int local_level;

	/* walk down from processor node */
	while ((res = acpi_get_pptt_resource(table_hdr, cpu_node, resource))) {
		resource++;

		local_level = acpi_pptt_walk_cache(table_hdr, *starting_level,
						   res, &ret, level, type);
		/*
		 * we are looking for the max depth. Since its potentially
		 * possible for a given node to have resources with differing
		 * depths verify that the depth we have found is the largest.
		 */
		if (number_of_levels < local_level)
			number_of_levels = local_level;
	}
	if (number_of_levels > *starting_level)
		*starting_level = number_of_levels;

	return ret;
}

/*
 * Given a processor node containing a processing unit, walk into it and count
 * how many levels exist solely for it, and then walk up each level until we hit
 * the root node (ignore the package level because it may be possible to have
 * caches that exist across packages). Count the number of cache levels that
 * exist at each level on the way up.
 */
static int acpi_process_node(struct acpi_table_header *table_hdr,
			     struct acpi_pptt_processor *cpu_node)
{
	int total_levels = 0;

	do {
		acpi_find_cache_level(table_hdr, cpu_node, &total_levels, 0, 0);
		cpu_node = fetch_pptt_node(table_hdr, cpu_node->parent);
	} while (cpu_node);

	return total_levels;
}

/*
 * Determine if the *node parameter is a leaf node by iterating the
 * PPTT table, looking for nodes which reference it.
 * Return 0 if we find a node refrencing the passed node,
 * or 1 if we don't.
 */
static int acpi_pptt_leaf_node(struct acpi_table_header *table_hdr,
			       struct acpi_pptt_processor *node)
{
	struct acpi_subtable_header *entry;
	unsigned long table_end;
	u32 node_entry;
	struct acpi_pptt_processor *cpu_node;

	table_end = (unsigned long)table_hdr + table_hdr->length;
	node_entry = ACPI_PTR_DIFF(node, table_hdr);
	entry = ACPI_ADD_PTR(struct acpi_subtable_header, table_hdr,
			     sizeof(struct acpi_table_pptt));

	while ((unsigned long)(entry + 1) < table_end) {
		cpu_node = (struct acpi_pptt_processor *)entry;
		if ((entry->type == ACPI_PPTT_TYPE_PROCESSOR) &&
		    (cpu_node->parent == node_entry))
			return 0;
		entry = ACPI_ADD_PTR(struct acpi_subtable_header, entry,
				     entry->length);
	}
	return 1;
}

/*
 * Find the subtable entry describing the provided processor.
 * This is done by iterating the PPTT table looking for processor nodes
 * which have an acpi_processor_id that matches the acpi_cpu_id parameter
 * passed into the function. If we find a node that matches this criteria
 * we verify that its a leaf node in the topology rather than depending
 * on the valid flag, which doesn't need to be set for leaf nodes.
 */
static struct acpi_pptt_processor *acpi_find_processor_node(
	struct acpi_table_header *table_hdr,
	u32 acpi_cpu_id)
{
	struct acpi_subtable_header *entry;
	unsigned long table_end;
	struct acpi_pptt_processor *cpu_node;

	table_end = (unsigned long)table_hdr + table_hdr->length;
	entry = ACPI_ADD_PTR(struct acpi_subtable_header, table_hdr,
			     sizeof(struct acpi_table_pptt));

	/* find the processor structure associated with this cpuid */
	while ((unsigned long)(entry + 1) < table_end) {
		cpu_node = (struct acpi_pptt_processor *)entry;

		if (entry->length == 0) {
			pr_err("Invalid zero length subtable\n");
			break;
		}
		if ((entry->type == ACPI_PPTT_TYPE_PROCESSOR) &&
		    (acpi_cpu_id == cpu_node->acpi_processor_id) &&
		     acpi_pptt_leaf_node(table_hdr, cpu_node)) {
			return (struct acpi_pptt_processor *)entry;
		}

		entry = ACPI_ADD_PTR(struct acpi_subtable_header, entry,
				     entry->length);
	}

	return NULL;
}

static int acpi_find_cache_levels(struct acpi_table_header *table_hdr,
				  u32 acpi_cpu_id)
{
	int number_of_levels = 0;
	struct acpi_pptt_processor *cpu;

	cpu = acpi_find_processor_node(table_hdr, acpi_cpu_id);
	if (cpu)
		number_of_levels = acpi_process_node(table_hdr, cpu);

	return number_of_levels;
}

/* Convert the linux cache_type to a ACPI PPTT cache type value */
static u8 acpi_cache_type(enum cache_type type)
{
	switch (type) {
	case CACHE_TYPE_DATA:
		pr_debug("Looking for data cache\n");
		return ACPI_PPTT_CACHE_TYPE_DATA;
	case CACHE_TYPE_INST:
		pr_debug("Looking for instruction cache\n");
		return ACPI_PPTT_CACHE_TYPE_INSTR;
	default:
	case CACHE_TYPE_UNIFIED:
		pr_debug("Looking for unified cache\n");
		/*
		 * It is important that ACPI_PPTT_CACHE_TYPE_UNIFIED
		 * contains the bit pattern that will match both
		 * ACPI unified bit patterns because we use it later
		 * to match both cases.
		 */
		return ACPI_PPTT_CACHE_TYPE_UNIFIED;
	}
}

/* find the ACPI node describing the cache type/level for the given CPU */
static struct acpi_pptt_cache *acpi_find_cache_node(
	struct acpi_table_header *table_hdr, u32 acpi_cpu_id,
	enum cache_type type, unsigned int level,
	struct acpi_pptt_processor **node)
{
	int total_levels = 0;
	struct acpi_pptt_cache *found = NULL;
	struct acpi_pptt_processor *cpu_node;
	u8 acpi_type = acpi_cache_type(type);

	pr_debug("Looking for CPU %d's level %d cache type %d\n",
		 acpi_cpu_id, level, acpi_type);

	cpu_node = acpi_find_processor_node(table_hdr, acpi_cpu_id);

	while ((cpu_node) && (!found)) {
		found = acpi_find_cache_level(table_hdr, cpu_node,
					      &total_levels, level, acpi_type);
		*node = cpu_node;
		cpu_node = fetch_pptt_node(table_hdr, cpu_node->parent);
	}

	return found;
}

/*
 * The ACPI spec implies that the fields in the cache structures are used to
 * extend and correct the information probed from the hardware. In the case
 * of arm64 the CCSIDR probing has been removed because it might be incorrect.
 */
static void update_cache_properties(struct cacheinfo *this_leaf,
				    struct acpi_pptt_cache *found_cache,
				    struct acpi_pptt_processor *cpu_node)
{
	this_leaf->firmware_node = cpu_node;
	if (found_cache->flags & ACPI_PPTT_SIZE_PROPERTY_VALID)
		this_leaf->size = found_cache->size;
	if (found_cache->flags & ACPI_PPTT_LINE_SIZE_VALID)
		this_leaf->coherency_line_size = found_cache->line_size;
	if (found_cache->flags & ACPI_PPTT_NUMBER_OF_SETS_VALID)
		this_leaf->number_of_sets = found_cache->number_of_sets;
	if (found_cache->flags & ACPI_PPTT_ASSOCIATIVITY_VALID)
		this_leaf->ways_of_associativity = found_cache->associativity;
	if (found_cache->flags & ACPI_PPTT_WRITE_POLICY_VALID)
		switch (found_cache->attributes & ACPI_PPTT_MASK_WRITE_POLICY) {
		case ACPI_PPTT_CACHE_POLICY_WT:
			this_leaf->attributes = CACHE_WRITE_THROUGH;
			break;
		case ACPI_PPTT_CACHE_POLICY_WB:
			this_leaf->attributes = CACHE_WRITE_BACK;
			break;
		}
	if (found_cache->flags & ACPI_PPTT_ALLOCATION_TYPE_VALID)
		switch (found_cache->attributes & ACPI_PPTT_MASK_ALLOCATION_TYPE) {
		case ACPI_PPTT_CACHE_READ_ALLOCATE:
			this_leaf->attributes |= CACHE_READ_ALLOCATE;
			break;
		case ACPI_PPTT_CACHE_WRITE_ALLOCATE:
			this_leaf->attributes |= CACHE_WRITE_ALLOCATE;
			break;
		case ACPI_PPTT_CACHE_RW_ALLOCATE:
		case ACPI_PPTT_CACHE_RW_ALLOCATE_ALT:
			this_leaf->attributes |=
				CACHE_READ_ALLOCATE | CACHE_WRITE_ALLOCATE;
			break;
		}
}

/*
 * Update the kernel cache information for each level of cache
 * associated with the given acpi cpu.
 */
static void cache_setup_acpi_cpu(struct acpi_table_header *table,
				 unsigned int cpu)
{
	struct acpi_pptt_cache *found_cache;
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	u32 acpi_cpu_id = get_acpi_id_for_cpu(cpu);
	struct cacheinfo *this_leaf;
	unsigned int index = 0;
	struct acpi_pptt_processor *cpu_node = NULL;

	while (index < get_cpu_cacheinfo(cpu)->num_leaves) {
		this_leaf = this_cpu_ci->info_list + index;
		found_cache = acpi_find_cache_node(table, acpi_cpu_id,
						   this_leaf->type,
						   this_leaf->level,
						   &cpu_node);
		pr_debug("found = %p %p\n", found_cache, cpu_node);
		if (found_cache)
			update_cache_properties(this_leaf,
						found_cache,
						cpu_node);

		index++;
	}
}

/*
 * Passing level values greater than this will result in search termination
 */
#define PPTT_ABORT_PACKAGE 0xFF

/*
 * Given a acpi_pptt_processor node, walk up until we identify the
 * package that the node is associated with, or we run out of levels
 * to request or the search is terminated with a flag match
 * The level parameter also serves to limit possible loops within the tree.
 */
static struct acpi_pptt_processor *acpi_find_processor_package_id(
	struct acpi_table_header *table_hdr,
	struct acpi_pptt_processor *cpu,
	int level, int flag)
{
	struct acpi_pptt_processor *prev_node;

	while (cpu && level) {
		if (cpu->flags & flag)
			break;
		pr_debug("level %d\n", level);
		prev_node = fetch_pptt_node(table_hdr, cpu->parent);
		if (prev_node == NULL)
			break;
		cpu = prev_node;
		level--;
	}
	return cpu;
}

/*
 * Get a unique value given a cpu, and a topology level, that can be
 * matched to determine which cpus share common topological features
 * at that level.
 */
static int topology_get_acpi_cpu_tag(struct acpi_table_header *table,
				     unsigned int cpu, int level, int flag)
{
	struct acpi_pptt_processor *cpu_node;
	u32 acpi_cpu_id = get_acpi_id_for_cpu(cpu);

	cpu_node = acpi_find_processor_node(table, acpi_cpu_id);
	if (cpu_node) {
		cpu_node = acpi_find_processor_package_id(table, cpu_node,
							  level, flag);
		/* Only the first level has a guaranteed id */
		if (level == 0)
			return cpu_node->acpi_processor_id;
		return (int)((u8 *)cpu_node - (u8 *)table);
	}
	pr_err_once("PPTT table found, but unable to locate core for %d\n",
		    cpu);
	return -ENOENT;
}

static int find_acpi_cpu_topology_tag(unsigned int cpu, int level, int flag)
{
	struct acpi_table_header *table;
	acpi_status status;
	int retval;

	status = acpi_get_table(ACPI_SIG_PPTT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_err_once("No PPTT table found, cpu topology may be inaccurate\n");
		return -ENOENT;
	}
	retval = topology_get_acpi_cpu_tag(table, cpu, level, flag);
	pr_debug("Topology Setup ACPI cpu %d, level %d ret = %d\n",
		 cpu, level, retval);
	acpi_put_table(table);

	return retval;
}

/**
 * acpi_find_last_cache_level() - Determines the number of cache levels for a PE
 * @cpu: Kernel logical cpu number
 *
 * Given a logical cpu number, returns the number of levels of cache represented
 * in the PPTT. Errors caused by lack of a PPTT table, or otherwise, return 0
 * indicating we didn't find any cache levels.
 *
 * Return: Cache levels visible to this core.
 */
int acpi_find_last_cache_level(unsigned int cpu)
{
	u32 acpi_cpu_id;
	struct acpi_table_header *table;
	int number_of_levels = 0;
	acpi_status status;

	pr_debug("Cache Setup find last level cpu=%d\n", cpu);

	acpi_cpu_id = get_acpi_id_for_cpu(cpu);
	status = acpi_get_table(ACPI_SIG_PPTT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_err_once("No PPTT table found, cache topology may be inaccurate\n");
	} else {
		number_of_levels = acpi_find_cache_levels(table, acpi_cpu_id);
		acpi_put_table(table);
	}
	pr_debug("Cache Setup find last level level=%d\n", number_of_levels);

	return number_of_levels;
}

/**
 * cache_setup_acpi() - Override CPU cache topology with data from the PPTT
 * @cpu: Kernel logical cpu number
 *
 * Updates the global cache info provided by cpu_get_cacheinfo()
 * when there are valid properties in the acpi_pptt_cache nodes. A
 * successful parse may not result in any updates if none of the
 * cache levels have any valid flags set.  Futher, a unique value is
 * associated with each known CPU cache entry. This unique value
 * can be used to determine whether caches are shared between cpus.
 *
 * Return: -ENOENT on failure to find table, or 0 on success
 */
int cache_setup_acpi(unsigned int cpu)
{
	struct acpi_table_header *table;
	acpi_status status;

	pr_debug("Cache Setup ACPI cpu %d\n", cpu);

	status = acpi_get_table(ACPI_SIG_PPTT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_err_once("No PPTT table found, cache topology may be inaccurate\n");
		return -ENOENT;
	}

	cache_setup_acpi_cpu(table, cpu);
	acpi_put_table(table);

	return status;
}

/**
 * find_acpi_cpu_topology() - Determine a unique topology value for a given cpu
 * @cpu: Kernel logical cpu number
 * @level: The topological level for which we would like a unique ID
 *
 * Determine a topology unique ID for each thread/core/cluster/mc_grouping
 * /socket/etc. This ID can then be used to group peers, which will have
 * matching ids.
 *
 * The search terminates when either the requested level is found or
 * we reach a root node. Levels beyond the termination point will return the
 * same unique ID. The unique id for level 0 is the acpi processor id. All
 * other levels beyond this use a generated value to uniquely identify
 * a topological feature.
 *
 * Return: -ENOENT if the PPTT doesn't exist, or the cpu cannot be found.
 * Otherwise returns a value which represents a unique topological feature.
 */
int find_acpi_cpu_topology(unsigned int cpu, int level)
{
	return find_acpi_cpu_topology_tag(cpu, level, 0);
}

/**
 * find_acpi_cpu_topology_package() - Determine a unique cpu package value
 * @cpu: Kernel logical cpu number
 *
 * Determine a topology unique package ID for the given cpu.
 * This ID can then be used to group peers, which will have matching ids.
 *
 * The search terminates when either a level is found with the PHYSICAL_PACKAGE
 * flag set or we reach a root node.
 *
 * Return: -ENOENT if the PPTT doesn't exist, or the cpu cannot be found.
 * Otherwise returns a value which represents the package for this cpu.
 */
int find_acpi_cpu_topology_package(unsigned int cpu)
{
	return find_acpi_cpu_topology_tag(cpu, PPTT_ABORT_PACKAGE,
					  ACPI_PPTT_PHYSICAL_PACKAGE);
}
