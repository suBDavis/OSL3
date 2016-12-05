/* A simple, (reverse) trie.  Only for use with 1 thread. */

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "trie.h"

struct trie_node {
    struct trie_node *next;  /* parent list */
    unsigned int strlen; /* Length of the key */
    int32_t ip4_address; /* 4 octets */
    struct trie_node *children; /* Sorted list of children */
    char key[MAX_KEY]; /* Up to MAX_KEY chars */
    pthread_mutex_t mutex;
};

static struct trie_node * root = NULL;
static int node_count = 0;
static int max_count = 100;  //Try to stay under 100 nodes
static pthread_mutex_t delete_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t root_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t node_count_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t delete_cond = PTHREAD_COND_INITIALIZER;
extern int separate_delete_thread;

struct trie_node * new_leaf (const char *string, size_t strlen, int32_t ip4_address) {
    struct trie_node *new_node = malloc(sizeof(struct trie_node));
    pthread_mutex_lock(&node_count_mutex);
    node_count++;
    pthread_mutex_unlock(&node_count_mutex);
    if (!new_node) {
        printf ("WARNING: Node memory allocation failed.  Results may be bogus.\n");
        return NULL;
    }
    assert(strlen < MAX_KEY);
    assert(strlen > 0);
    new_node->next = NULL;
    new_node->strlen = strlen;
    strncpy(new_node->key, string, strlen);
    new_node->key[strlen] = '\0';
    new_node->ip4_address = ip4_address;
    new_node->children = NULL;
    int err = pthread_mutex_init(&(new_node->mutex), NULL);
    if (err)
        printf("Failed to initialize mutex: %d\n", err);
    return new_node;
}

// Compare strings backward.  Unlike strncmp, we assume
// that we will not stop at a null termination, only after
// n chars (or a difference).  Base code borrowed from musl
int reverse_strncmp(const char *left, const char *right, size_t n) {
    const unsigned char *l= (const unsigned char *) &left[n-1];
    const unsigned char *r= (const unsigned char *) &right[n-1];
    if (!n--) return 0;
    for (; *l && *r && n && *l == *r ; l--, r--, n--);
    return *l - *r;
}

int compare_keys (const char *string1, int len1, const char *string2, int len2, int *pKeylen) {
    int keylen, offset;
    char scratch[MAX_KEY];
    assert (len1 > 0);
    assert (len2 > 0);
    // Take the max of the two keys, treating the front as if it were 
    // filled with spaces, just to ensure a total order on keys.
    if (len1 < len2) {
        keylen = len2;
        offset = keylen - len1;
        memset(scratch, ' ', offset);
        memcpy(&scratch[offset], string1, len1);
        string1 = scratch;
    } else if (len2 < len1) {
        keylen = len1;
        offset = keylen - len2;
        memset(scratch, ' ', offset);
        memcpy(&scratch[offset], string2, len2);
        string2 = scratch;
    } else
        keylen = len1; // == len2

    assert (keylen > 0);
    if (pKeylen)
        *pKeylen = keylen;
    return reverse_strncmp(string1, string2, keylen);
}

int compare_keys_substring (const char *string1, int len1, const char *string2, int len2, int *pKeylen) {
    int keylen, offset1, offset2;
    keylen = len1 < len2 ? len1 : len2;
    offset1 = len1 - keylen;
    offset2 = len2 - keylen;
    assert (keylen > 0);
    if (pKeylen)
        *pKeylen = keylen;
    return reverse_strncmp(&string1[offset1], &string2[offset2], keylen);
}

void init(int numthreads) {
    root = NULL;
}

void shutdown_delete_thread() {
    if (separate_delete_thread) {
        pthread_mutex_lock(&delete_mutex);
        pthread_cond_signal(&delete_cond);
        pthread_mutex_unlock(&delete_mutex);
    }
    return;
}

/* Recursive helper function.
 * Returns a pointer to the node if found.
 * Stores an optional pointer to the 
 * parent, or what should be the parent if not found.
 * 
 */
struct trie_node * 
_search (struct trie_node *node, const char *string, size_t strlen, struct trie_node *prev_node) {

    int keylen, cmp;

    // First things first, check if we are NULL 
    if (node == NULL) {
        if (!prev_node)
            pthread_mutex_unlock(&root_mutex);
        else
            pthread_mutex_unlock(&(prev_node->mutex));
        return NULL;
    }

    if (node->strlen >= MAX_KEY)
        printf("%d\n", node->strlen);

    assert(node->strlen < MAX_KEY);

    // Check if nodes are locked
    assert(pthread_mutex_trylock(&(node->mutex)));
    if (prev_node)
        assert(pthread_mutex_trylock(&(prev_node->mutex)));

    // See if this key is a substring of the string passed in
    cmp = compare_keys_substring(node->key, node->strlen, string, strlen, &keylen);
    if (cmp == 0) {
        // Yes, either quit, or recur on the children

        // If this key is longer than our search string, the key isn't here
        if (node->strlen > keylen) {
            pthread_mutex_unlock(&(node->mutex));
            if (!prev_node)
                pthread_mutex_unlock(&root_mutex);
            else
                pthread_mutex_unlock(&(prev_node->mutex));
            return NULL;
        } else if (strlen > keylen) {
            // Recur on children list
            if (!node->children) {
                pthread_mutex_unlock(&(node->mutex));
                if (!prev_node)
                    pthread_mutex_unlock(&root_mutex);
                else
                    pthread_mutex_unlock(&(prev_node->mutex));
                return NULL;
            }
            pthread_mutex_lock(&(node->children->mutex));
            if (!prev_node)
                pthread_mutex_unlock(&root_mutex);
            else
                pthread_mutex_unlock(&(prev_node->mutex));
            return _search(node->children, string, strlen - keylen, node);
        } else {
            assert (strlen == keylen);
            pthread_mutex_unlock(&(node->mutex));
            if (!prev_node)
                pthread_mutex_unlock(&root_mutex);
            else
                pthread_mutex_unlock(&(prev_node->mutex));
            return node;
        }

    } else {
        cmp = compare_keys(node->key, node->strlen, string, strlen, &keylen);
        if (cmp < 0) {
            // No, look right (the node's key is "less" than the search key)
            if (!node->next) {
                pthread_mutex_unlock(&(node->mutex));
                if (!prev_node)
                    pthread_mutex_unlock(&root_mutex);
                else
                    pthread_mutex_unlock(&(prev_node->mutex));
                return NULL;
            }
            pthread_mutex_lock(&(node->next->mutex));
            if (!prev_node)
                pthread_mutex_unlock(&root_mutex);
            else
                pthread_mutex_unlock(&(prev_node->mutex));
            return _search(node->next, string, strlen, node);
        } else {
            // Quit early
            pthread_mutex_unlock(&(node->mutex));
            if (!prev_node)
                pthread_mutex_unlock(&root_mutex);
            else
                pthread_mutex_unlock(&(prev_node->mutex));
            return 0;
        }
    }
}

int search  (const char *string, size_t strlen, int32_t *ip4_address) {
    struct trie_node *found;

    // Skip strings of length 0
    if (strlen == 0)
        return 0;

    pthread_mutex_lock(&delete_mutex);
    pthread_mutex_lock(&root_mutex);
    pthread_mutex_unlock(&delete_mutex);
    if (!root) {
        pthread_mutex_unlock(&root_mutex);
        return 0;
    }
    pthread_mutex_lock(&(root->mutex));
    found = _search(root, string, strlen, NULL);

    if (found && ip4_address)
        *ip4_address = found->ip4_address;

    return (found != NULL);
}

/* Recursive helper function */
int _insert (const char *string, size_t strlen, int32_t ip4_address, 
        struct trie_node *node, struct trie_node *parent, struct trie_node *left) {

    int cmp, keylen;
    struct trie_node *new_node = NULL;

    // First things first, check if we are NULL 
    assert (node != NULL);
    assert (node->strlen < MAX_KEY);
    assert ((!parent) || (!left));

    // Check that parent, left, and node are locked
    if (parent)
        assert(pthread_mutex_trylock(&(parent->mutex)));
    if (left)
        assert(pthread_mutex_trylock(&(left->mutex)));
    assert(pthread_mutex_trylock(&(node->mutex)));

    // Take the minimum of the two lengths
    cmp = compare_keys_substring (node->key, node->strlen, string, strlen, &keylen);
    if (cmp == 0) {
        // Yes, either quit, or recur on the children

        // If this key is longer than our search string, we need to insert
        // "above" this node
        if (node->strlen > keylen) {

            assert(keylen == strlen);
            assert((!parent) || parent->children == node);

            new_node = new_leaf (string, strlen, ip4_address);
            pthread_mutex_lock(&(new_node->mutex));
            node->strlen -= keylen;
            new_node->children = node;
            new_node->next = node->next;
            node->next = NULL;

            assert ((!parent) || (!left));

            if (parent) {
                parent->children = new_node;
                pthread_mutex_unlock(&(parent->mutex));
            } else if (left) {
                left->next = new_node;
                pthread_mutex_unlock(&(left->mutex));
            } else if ((!parent) || (!left))
                root = new_node;
            pthread_mutex_unlock(&(new_node->mutex));
            pthread_mutex_unlock(&(node->mutex));
            if (!parent && !left)
                pthread_mutex_unlock(&root_mutex);
            return 1;

        } else if (strlen > keylen) {
            if (!parent && !left)
                pthread_mutex_unlock(&root_mutex);
            if (node->children == NULL) {
                // Insert leaf here
                new_node = new_leaf (string, strlen - keylen, ip4_address);
                pthread_mutex_lock(&(new_node->mutex));
                node->children = new_node;
                if (parent)
                    pthread_mutex_unlock(&(parent->mutex));
                if (left)
                    pthread_mutex_unlock(&(left->mutex));
                pthread_mutex_unlock(&(node->mutex));
                pthread_mutex_unlock(&(new_node->mutex));
                return 1;
            } else {
                // Recur on children list, store "parent" (loosely defined)
                pthread_mutex_lock(&(node->children->mutex));
                if (parent)
                    pthread_mutex_unlock(&(parent->mutex));
                if (left)
                    pthread_mutex_unlock(&(left->mutex));
                return _insert(string, strlen - keylen, ip4_address,
                        node->children, node, NULL);
            }
        } else {
            if (!parent && !left)
                pthread_mutex_unlock(&root_mutex);
            assert (strlen == keylen);
            if (node->ip4_address == 0) {
                node->ip4_address = ip4_address;
                if (parent)
                    pthread_mutex_unlock(&(parent->mutex));
                if (left)
                    pthread_mutex_unlock(&(left->mutex));
                pthread_mutex_unlock(&(node->mutex));
                return 1;
            } else {
                if (parent)
                    pthread_mutex_unlock(&(parent->mutex));
                if (left)
                    pthread_mutex_unlock(&(left->mutex));
                pthread_mutex_unlock(&(node->mutex));
                return 0;
            }
        }

    } else {
        /* Is there any common substring? */
        int i, cmp2, keylen2, overlap = 0;
        for (i = 1; i < keylen; i++) {
            cmp2 = compare_keys_substring (&node->key[i], node->strlen - i, 
                    &string[i], strlen - i, &keylen2);
            assert (keylen2 > 0);
            if (cmp2 == 0) {
                overlap = 1;
                break;
            }
        }

        if (overlap) {
            // Insert a common parent, recur
            int offset = strlen - keylen2;
            new_node = new_leaf (&string[offset], keylen2, 0);
            pthread_mutex_lock(&(new_node->mutex));
            assert ((node->strlen - keylen2) > 0);
            node->strlen -= keylen2;
            new_node->children = node;
            new_node->next = node->next;
            node->next = NULL;
            assert ((!parent) || (!left));

            if (parent)
                assert(pthread_mutex_trylock(&(parent->mutex)));
            if (left)
                assert(pthread_mutex_trylock(&(left->mutex)));
            assert(pthread_mutex_trylock(&(node->mutex)));

            if (node == root) {
                root = new_node;
            } else if (parent) {
                assert(parent->children == node);
                parent->children = new_node;
                pthread_mutex_unlock(&(parent->mutex));
            } else if (left) {
                assert(left->next == node);
                left->next = new_node;
                pthread_mutex_unlock(&(left->mutex));
            } else if ((!parent) && (!left))
                root = new_node;
            if (!parent && !left)
                pthread_mutex_unlock(&root_mutex);
            return _insert(string, offset, ip4_address, node, new_node, NULL);
        } else {
            cmp = compare_keys (node->key, node->strlen, string, strlen, &keylen);
            if (cmp < 0) {
                // No, recur right (the node's key is "less" than  the search key)
                if (!parent && !left)
                    pthread_mutex_unlock(&root_mutex);
                if (node->next) {
                    pthread_mutex_lock(&(node->next->mutex));
                    if (parent)
                        pthread_mutex_unlock(&(parent->mutex));
                    if (left)
                        pthread_mutex_unlock(&(left->mutex));
                    return _insert(string, strlen, ip4_address, node->next, NULL, node);
                } else {
                    // Insert here
                    new_node = new_leaf (string, strlen, ip4_address);
                    pthread_mutex_lock(&(new_node->mutex));
                    node->next = new_node;
                    if (parent)
                        pthread_mutex_unlock(&(parent->mutex));
                    if (left)
                        pthread_mutex_unlock(&(left->mutex));
                    pthread_mutex_unlock(&(node->mutex));
                    pthread_mutex_unlock(&(new_node->mutex));
                    return 1;
                }
            } else {
                // Insert here
                new_node = new_leaf (string, strlen, ip4_address);
                pthread_mutex_lock(&(new_node->mutex));
                new_node->next = node;
                if (node == root) {
                    root = new_node;
                } else if (parent && parent->children == node)
                    parent->children = new_node;
                else if (left && left->next == node)
                    left->next = new_node;
                pthread_mutex_unlock(&(new_node->mutex));
                if (parent)
                    pthread_mutex_unlock(&(parent->mutex));
                if (left)
                    pthread_mutex_unlock(&(left->mutex));
                pthread_mutex_unlock(&(node->mutex));
                if (!parent && !left)
                    pthread_mutex_unlock(&root_mutex);
                return 1;
            }
        }
    }
}

void assert_invariants();

int insert (const char *string, size_t strlen, int32_t ip4_address) {
    // Skip strings of length 0
    if (strlen == 0)
        return 0;

    pthread_mutex_lock(&delete_mutex);
    pthread_mutex_lock(&root_mutex);
    pthread_mutex_unlock(&delete_mutex);
    int res;
    /* Edge case: root is null */
    if (root == NULL) {
        root = new_leaf (string, strlen, ip4_address);
        pthread_mutex_unlock(&root_mutex);
        return 1;
    }
    pthread_mutex_lock(&(root->mutex));
    res = _insert (string, strlen, ip4_address, root, NULL, NULL);
    //assert_invariants();
    pthread_mutex_lock(&delete_mutex);
    if (node_count >= max_count  && separate_delete_thread)
        pthread_cond_signal(&delete_cond);
    pthread_mutex_unlock(&delete_mutex);
    return res;
}

/* Recursive helper function.
 * Returns a pointer to the node if found.
 * Stores an optional pointer to the 
 * parent, or what should be the parent if not found.
 * 
 */
struct trie_node * 
_delete (struct trie_node *node, const char *string, 
        size_t strlen, int unlock_root) {
    int keylen, cmp;

    /* Locking note:
     * When _delete is called, node->mutex should ALREADY BE LOCKED 
     */

    // First things first, check if we are NULL 
    if (node == NULL) return NULL;

    assert(node->strlen < MAX_KEY);

    // Check if node is locked
    assert(pthread_mutex_trylock(&(node->mutex)));


    // See if this key is a substring of the string passed in
    cmp = compare_keys_substring (node->key, node->strlen, string, strlen, &keylen);
    if (cmp == 0) {
        // Yes, either quit, or recur on the children

        // If this key is longer than our search string, the key isn't here
        if (node->strlen > keylen) {
            pthread_mutex_unlock(&(node->mutex));
            if (unlock_root)
                pthread_mutex_unlock(&root_mutex);
            return NULL;
        } else if (strlen > keylen) {

            /* Locking note:
             * 1. set child's parent mutex to current node mutex.
             * 2. lock the child's mutex.
             */

            if (node->children)
                pthread_mutex_lock(&(node->children->mutex));

            struct trie_node *found =  _delete(node->children, string, strlen - keylen, 0);
            /* After the above returns, the lock on node->children should be free again. */

            if (found) {
                /* If the node doesn't have children, delete it.
                 * Otherwise, keep it around to find the kids */
                if (found->children == NULL && found->ip4_address == 0) {

                    assert(node->children == found);
                    node->children = found->next;

                    /* Locking note:
                     * Since we are freeing the current node, the parent must be locked.
                     * That's why unlocking is safe here.
                     */
                    free(found);
                    pthread_mutex_lock(&node_count_mutex);
                    node_count--;
                    pthread_mutex_unlock(&node_count_mutex);
                }

                /* Delete the root node if we empty the tree */
                if (node == root && node->children == NULL && node->ip4_address == 0) {
                    /* Locking note:
                     * Since we are changing the root, we must aquire the lock on root->next
                     */
                    if (node->next)
                        pthread_mutex_lock(&(node->next->mutex));

                    root = node->next;
                    pthread_mutex_unlock(&(node->mutex));
                    free(node);
                    pthread_mutex_lock(&node_count_mutex);
                    node_count--;
                    pthread_mutex_unlock(&node_count_mutex);

                    /* It's safe to release the root lock now */
                    if (root)
                        pthread_mutex_unlock(&(root->mutex));

                    /* No locks held right now. That's probably fine. */
                } else
                    pthread_mutex_unlock(&(node->mutex));
                if (unlock_root)
                    pthread_mutex_unlock(&root_mutex);
                return node; /* Recursively delete needless interior nodes */
            } else pthread_mutex_unlock(&(node->mutex));
            if (unlock_root)
                pthread_mutex_unlock(&root_mutex);
            return NULL;
        } else {
            assert (strlen == keylen);

            /* We found it! Clear the ip4 address and return. */
            if (node->ip4_address) {
                node->ip4_address = 0;

                /* Delete the root node if we empty the tree */
                if (node == root && node->children == NULL && node->ip4_address == 0) {

                    /* to change the root, aquire a lock first */
                    if (node->next)
                        pthread_mutex_lock(&(node->next->mutex));
                    root = node->next;
                    /* Release the old root lock */
                    pthread_mutex_unlock(&(node->mutex));
                    free(node);
                    node = NULL;
                    pthread_mutex_lock(&node_count_mutex);
                    node_count--;
                    pthread_mutex_unlock(&node_count_mutex);
                    /* unlock the root */
                    if (root)
                        pthread_mutex_unlock(&(root->mutex));
                    if (unlock_root)
                        pthread_mutex_unlock(&root_mutex);
                    return (struct trie_node *) 0x100100; /* XXX: Don't use this pointer for anything except 
                                                           * comparison with NULL, since the memory is freed.
                                                           * Return a "poison" pointer that will probably 
                                                           * segfault if used.
                                                           */
                } else {
                    if (node)
                        pthread_mutex_unlock(&(node->mutex));
                    if (unlock_root)
                        pthread_mutex_unlock(&root_mutex);
                    return node;
                }
            } else {
                /* Just an interior node with no value */
                if (node)
                    pthread_mutex_unlock(&(node->mutex));
                if (unlock_root)
                    pthread_mutex_unlock(&root_mutex);
                return NULL;
            }
        }

    } else {
        cmp = compare_keys (node->key, node->strlen, string, strlen, &keylen);
        if (cmp < 0) {
            // No, look right (the node's key is "less" than  the search key)
            /* Lock note:
             * We must lock the next node
             */
            if (node->next)
                pthread_mutex_lock(&(node->next->mutex));

            struct trie_node *found = _delete(node->next, string, strlen, 0);
            if (found) {
                /* If the node doesn't have children, delete it.
                 * Otherwise, keep it around to find the kids */
                if (found->children == NULL && found->ip4_address == 0) {
                    assert(node->next == found);
                    node->next = found->next;
                    free(found);
                    pthread_mutex_lock(&node_count_mutex);
                    node_count--;
                    pthread_mutex_unlock(&node_count_mutex);
                }

                pthread_mutex_unlock(&(node->mutex));
                if (unlock_root)
                    pthread_mutex_unlock(&root_mutex);
                return node; /* Recursively delete needless interior nodes */
            }
            pthread_mutex_unlock(&(node->mutex));
            if (unlock_root)
                pthread_mutex_unlock(&root_mutex);
            return NULL;
        } else {
            // Quit early
            pthread_mutex_unlock(&(node->mutex));
            if (unlock_root)
                pthread_mutex_unlock(&root_mutex);
            return NULL;
        }
    }
}

int delete  (const char *string, size_t strlen) {
    // Skip strings of length 0
    pthread_mutex_lock(&delete_mutex);
    pthread_mutex_lock(&root_mutex);
    pthread_mutex_unlock(&delete_mutex);
    if (strlen == 0 || !root) {
        pthread_mutex_unlock(&root_mutex);
        return 0;
    }
    pthread_mutex_lock(&(root->mutex));
    int res = (NULL != _delete(root, string, strlen, 1));
    //assert_invariants();
    return res;
}

/* Find one node to remove from the tree. 
 * Use any policy you like to select the node.
 */
int drop_one_node() {
    // keep root node locked while traversing to maintain path
    pthread_mutex_lock(&(root->mutex));
    struct trie_node *node = root;
    assert(node->key != NULL);
    int size = MAX_KEY-1;
    char key[size+1];
    key[size] = '\0';
    do {
        assert(node->key != NULL);
        size -= node->strlen;
        assert(size >= 0);
        memcpy(&key[size], node->key, node->strlen);
        if (node->children)
            pthread_mutex_lock(&(node->children->mutex));
        if (node != root)
            pthread_mutex_unlock(&(node->mutex));
    } while ((node = node->children));
    assert(node == NULL);
    return (_delete(root, &key[size], strlen(&key[size]), 1) != NULL);
}


/* Check the total node count; see if we have exceeded a the max. */
void check_max_nodes() {
    pthread_mutex_lock(&delete_mutex);
    if (separate_delete_thread)
        pthread_cond_wait(&delete_cond, &delete_mutex);
    while (node_count > max_count) {
        pthread_mutex_lock(&root_mutex);
        assert(drop_one_node());
    }
    pthread_mutex_unlock(&delete_mutex);
}

void delete_all_nodes() {
    pthread_mutex_lock(&delete_mutex);
    pthread_mutex_lock(&root_mutex);

    while (node_count)
        drop_one_node();
    assert(node_count == 0);
    pthread_mutex_unlock(&root_mutex);
    pthread_mutex_unlock(&delete_mutex);
}

int _print(struct trie_node *node, int depth, char lines[100], int count) {
    printf("%s", lines);
    if (!node->next)
        printf("└");
    else
        printf("├");
    printf ("%.*s, IP %d, This %p, Next %p, Children %p\n",
            node->strlen, node->key, node->ip4_address, node, node->next, node->children);
    if (node->children) {
        pthread_mutex_lock(&(node->children->mutex));
        if (node->next)
            strcat(lines, "| ");
        else strcat(lines, "  ");
        count = _print(node->children, depth+1, lines, count+1);
        lines[2*depth] = '\0';
    }
    if (node->next) {
        pthread_mutex_lock(&(node->next->mutex));
        count = _print(node->next, depth, lines, count+1);
    }
    pthread_mutex_unlock(&(node->mutex));
    return count;
}

void print() {
    pthread_mutex_lock(&delete_mutex);
    pthread_mutex_lock(&root_mutex);
    pthread_mutex_unlock(&delete_mutex);
    printf ("Root is at %p\n", root);
    char lines[100];
    lines[0] = '\0';
    int count = 0;
    if (root)
        pthread_mutex_lock(&root->mutex);
    if (root)
        count = _print(root, 0, lines, 1);
    pthread_mutex_unlock(&root_mutex);
#ifdef DEBUG
    printf("node_count: %d\nActual node count: %d\n", node_count, count);
#endif
    assert(count == node_count);
}

int num_nodes() {
    return node_count;
}

int _assert_invariants (struct trie_node *node, int prefix_length, int *error) {
    int count = 1;

    int len = prefix_length + node->strlen;
    assert(!pthread_mutex_trylock(&(node->mutex)));
    pthread_mutex_unlock(&(node->mutex));
    if (len > MAX_KEY) {
        printf("key too long at node %p.  Key %.*s (%d), IP %d.  Next %p, Children %p\n", 
                node, node->strlen, node->key, node->strlen, node->ip4_address, node->next, node->children);
        *error = 1;
        return count;
    }

    if (node->children) {
        count += _assert_invariants(node->children, len, error);
        if (*error) {
            printf("Unwinding tree on error: node %p.  Key %.*s (%d), IP %d.  Next %p, Children %p\n", 
                    node, node->strlen, node->key, node->strlen, node->ip4_address, node->next, node->children);
            return count;
        }
    }

    if (node->next) {
        count += _assert_invariants(node->next, prefix_length, error);
    }

    return count;
}

void assert_invariants () {
#ifdef DEBUG
    int err = 0;
    if (root) {
        int count = _assert_invariants(root, 0, &err);
        if (err) print();
        assert(count == node_count);
    }
#endif // DEBUG
}

