#pragma once
#ifndef SELVA_MODIFY_HIERARCHY
#define SELVA_MODIFY_HIERARCHY

#include "linker_set.h"
#include "selva.h"
#include "svector.h"
#include "tree.h"
#include "trx.h"

/**
 * Default Redis key name for Selva hierarchy.
 */
#define HIERARCHY_DEFAULT_KEY "___selva_hierarchy"

struct SelvaModify_Hierarchy;
typedef struct SelvaModify_Hierarchy SelvaModify_Hierarchy;

/* Forward declarations for metadata */
/* ... */
/* End of forward declarations for metadata */

/**
 * Hierarchy node metadata.
 * This structure should contain primitive data types or pointers to forward
 * declared structures.
 */
struct SelvaModify_HierarchyMetaData {
    struct SVector sub_markers;
};

typedef void SelvaModify_HierarchyMetadataHook(Selva_NodeId id, struct SelvaModify_HierarchyMetaData *metadata);

#define SELVA_MODIFY_HIERARCHY_METADATA_CONSTRUCTOR(fun) \
    DATA_SET(selva_HMCtor, fun)

#define SELVA_MODIFY_HIERARCHY_METADATA_DESTRUCTOR(fun) \
    DATA_SET(selva_HMDtor, fun)

struct Selva_Subscription;

RB_HEAD(hierarchy_index_tree, SelvaModify_HierarchyNode);
RB_HEAD(hierarchy_subscriptions_tree, Selva_Subscription);

struct SelvaModify_Hierarchy {
    /**
     * Current transaction timestamp.
     * Set before traversal begins and is used for marking visited nodes. Due to the
     * marking being a timestamp it's not necessary to clear it afterwards, which
     * could be a costly operation itself.
     */
    Trx current_trx;

    struct hierarchy_index_tree index_head;

    /**
     * Orphan nodes aka heads of the hierarchy.
     */
    SVector heads;

    /**
     * A tree of all subscriptions applying to this tree.
     */
    struct hierarchy_subscriptions_tree subs_head;
};

/**
 * Hierarchy traversal order.
 * Used by SelvaModify_TraverseHierarchy().
 */
enum SelvaModify_HierarchyTraversal {
    SELVA_HIERARCHY_TRAVERSAL_NONE, /* DO NOT USE */
    SELVA_HIERARCHY_TRAVERSAL_NODE,
    SELVA_HIERARCHY_TRAVERSAL_BFS_ANCESTORS,
    SELVA_HIERARCHY_TRAVERSAL_BFS_DESCENDANTS,
    SELVA_HIERARCHY_TRAVERSAL_DFS_ANCESTORS,
    SELVA_HIERARCHY_TRAVERSAL_DFS_DESCENDANTS,
    SELVA_HIERARCHY_TRAVERSAL_DFS_FULL,
};

/**
 * Called for each node found during a traversal.
 * @returns 0 to continue the traversal; 1 to interrupt the traversal.
 */
typedef int (*SelvaModify_HierarchyCallback)(Selva_NodeId id, void *arg, struct SelvaModify_HierarchyMetaData *metadata);

struct SelvaModify_HierarchyCallback {
    SelvaModify_HierarchyCallback node_cb;
    void * node_arg;
};

struct RedisModuleCtx;
struct RedisModuleString;

/**
 * Create a new hierarchy.
 */
SelvaModify_Hierarchy *SelvaModify_NewHierarchy(struct RedisModuleCtx *ctx);

/**
 * Free a hierarchy.
 */
void SelvaModify_DestroyHierarchy(SelvaModify_Hierarchy *hierarchy);

/**
 * Open a hierarchy key.
 */
SelvaModify_Hierarchy *SelvaModify_OpenHierarchy(struct RedisModuleCtx *ctx, struct RedisModuleString *key_name, int mode);

int SelvaModify_HierarchyNodeExists(SelvaModify_Hierarchy *hierarchy, const Selva_NodeId id);

struct SelvaModify_HierarchyMetaData *SelvaModify_HierarchyGetNodeMetadata(
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id);

ssize_t SelvaModify_GetHierarchyDepth(SelvaModify_Hierarchy *hierarchy, const Selva_NodeId id);

int SelvaModify_DelHierarchyChildren(
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id);

int SelvaModify_DelHierarchyParents(
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id);

/**
 * Set node relationships relative to other existing nodes.
 * Previously existing connections to and from other nodes are be removed.
 * If a node with id doesn't exist it will be created.
 * @param parents   Sets these nodes and only these nodes as parents of this node.
 * @param children  Sets these nodes and only these nodes as children of this node.
 */
int SelvaModify_SetHierarchy(
        struct RedisModuleCtx *ctx,
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id,
        size_t nr_parents,
        const Selva_NodeId *parents,
        size_t nr_children,
        const Selva_NodeId *children);

/**
 * Set parents of an existing node.
 * @param parents   Sets these nodes and only these nodes as parents of this node.
 */
int SelvaModify_SetHierarchyParents(
        struct RedisModuleCtx *ctx,
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id,
        size_t nr_parents,
        const Selva_NodeId *parents);

/**
 * Set children of an existing node.
 * @param children  Sets these nodes and only these nodes as children of this node.
 */
int SelvaModify_SetHierarchyChildren(
        struct RedisModuleCtx *ctx,
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id,
        size_t nr_children,
        const Selva_NodeId *children);

/**
 * Add new relationships relative to other existing nodes.
 * Previously existing connections to and from other nodes are be preserved.
 * If a node with id doesn't exist it will be created.
 * @param parents   Sets these nodes as parents to this node,
 *                  while keeping the existing parents.
 * @param children  Sets these nodes as children to this node,
 *                  while keeping the existing children.
 */
int SelvaModify_AddHierarchy(
        struct RedisModuleCtx *ctx,
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id,
        size_t nr_parents,
        const Selva_NodeId *parents,
        size_t nr_children,
        const Selva_NodeId *children);

/**
 * Remove relationship relative to other existing nodes.
 * @param parents   Removes the child relationship between this node and
 *                  the listed parents.
 * @param children  Removes the parent relationship between this node and
 *                  the listed children.
 */
int SelvaModify_DelHierarchy(
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id,
        size_t nr_parents,
        const Selva_NodeId *parents,
        size_t nr_children,
        const Selva_NodeId *children);

/**
 * Delete a node from the hierarchy.
 */
int SelvaModify_DelHierarchyNode(
        struct RedisModuleCtx *ctx,
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id);

/**
 * Get orphan head nodes of the given hierarchy.
 */
ssize_t SelvaModify_GetHierarchyHeads(SelvaModify_Hierarchy *hierarchy, Selva_NodeId **res);

/**
 * Get an unsorted list of ancestors fo a given node.
 */
ssize_t SelvaModify_FindAncestors(SelvaModify_Hierarchy *hierarchy, const Selva_NodeId id, Selva_NodeId **ancestors);

/**
 * Get an unsorted list of descendants of a given node.
 */
ssize_t SelvaModify_FindDescendants(SelvaModify_Hierarchy *hierarchy, const Selva_NodeId id, Selva_NodeId **descendants);

const char *SelvaModify_HierarchyDir2str(enum SelvaModify_HierarchyTraversal dir);
int SelvaModify_TraverseHierarchy(
        SelvaModify_Hierarchy *hierarchy,
        const Selva_NodeId id,
        enum SelvaModify_HierarchyTraversal dir,
        struct SelvaModify_HierarchyCallback *cb);

/*
 * hierarchy_events.c
 */
void SelvaModify_PublishDescendants(struct SelvaModify_Hierarchy *hierarchy, const char *id_str);

#endif /* SELVA_MODIFY_HIERARCHY */