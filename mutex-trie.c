/* A simple, (reverse) trie.  Only for use with 1 thread. */

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "trie.h"
#include <unistd.h>

struct trie_node {
    struct trie_node *next;  /* parent list */
    unsigned int strlen; /* Length of the key */
    int32_t ip4_address; /* 4 octets */
    struct trie_node *children; /* Sorted list of children */
    char key[64]; /* Up to 64 chars */
};

static struct trie_node * root = NULL;
static int node_count = 0;
static int max_count = 100;  //Try to stay under 100 nodes
static pthread_mutex_t mutex;
static pthread_cond_t delete_cond = PTHREAD_COND_INITIALIZER;

struct trie_node * new_leaf (const char *string, size_t strlen, int32_t ip4_address) {
    struct trie_node *new_node = malloc(sizeof(struct trie_node));
    node_count++;
    if (!new_node) {
        printf ("WARNING: Node memory allocation failed.  Results may be bogus.\n");
        return NULL;
    }
    assert(strlen < 64);
    assert(strlen > 0);
    new_node->next = NULL;
    new_node->strlen = strlen;
    strncpy(new_node->key, string, strlen);
    new_node->key[strlen] = '\0';
    new_node->ip4_address = ip4_address;
    new_node->children = NULL;

    return new_node;
}

int compare_keys (const char *string1, int len1, const char *string2, int len2, int *pKeylen) {
    int keylen, offset;
    char scratch[64];
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
    return strncmp(string1, string2, keylen);
}

int compare_keys_substring (const char *string1, int len1, const char *string2, int len2, int *pKeylen) {
    int keylen, offset1, offset2;
    keylen = len1 < len2 ? len1 : len2;
    offset1 = len1 - keylen;
    offset2 = len2 - keylen;
    assert (keylen > 0);
    if (pKeylen)
        *pKeylen = keylen;
    return strncmp(&string1[offset1], &string2[offset2], keylen);
}

void init(int numthreads) {
    int err = pthread_mutex_init(&mutex, NULL);
    if (err)
        printf("Failed to initialize mutex: %d\n", err);
    root = NULL;
}

void shutdown_delete_thread() {
    pthread_cond_signal(&delete_cond);
    return;
}

/* Recursive helper function.
 * Returns a pointer to the node if found.
 * Stores an optional pointer to the 
 * parent, or what should be the parent if not found.
 * 
 */
struct trie_node * 
_search (struct trie_node *node, const char *string, size_t strlen) {

    int keylen, cmp;

    // First things first, check if we are NULL 
    if (node == NULL) return NULL;

    assert(node->strlen < 64);

    // See if this key is a substring of the string passed in
    cmp = compare_keys_substring(node->key, node->strlen, string, strlen, &keylen);
    if (cmp == 0) {
        // Yes, either quit, or recur on the children

        // If this key is longer than our search string, the key isn't here
        if (node->strlen > keylen) {
            return NULL;
        } else if (strlen > keylen) {
            // Recur on children list
            return _search(node->children, string, strlen - keylen);
        } else {
            assert (strlen == keylen);

            return node;
        }

    } else {
        cmp = compare_keys(node->key, node->strlen, string, strlen, &keylen);
        if (cmp < 0) {
            // No, look right (the node's key is "less" than the search key)
            return _search(node->next, string, strlen);
        } else {
            // Quit early
            return 0;
        }
    }
}



int search  (const char *string, size_t strlen, int32_t *ip4_address) {
    struct trie_node *found;

    // Skip strings of length 0
    if (strlen == 0)
        return 0;

    pthread_mutex_lock(&mutex);
    found = _search(root, string, strlen);

    if (found && ip4_address)
        *ip4_address = found->ip4_address;
    pthread_mutex_unlock(&mutex);
    return (found != NULL);
}


/* Recursive helper function */
int _insert (const char *string, size_t strlen, int32_t ip4_address, 
        struct trie_node *node, struct trie_node *parent, struct trie_node *left) {

    int cmp, keylen;

    // First things first, check if we are NULL 
    assert (node != NULL);
    assert (node->strlen < 64);

    // Take the minimum of the two lengths
    cmp = compare_keys_substring (node->key, node->strlen, string, strlen, &keylen);
    if (cmp == 0) {
        // Yes, either quit, or recur on the children

        // If this key is longer than our search string, we need to insert
        // "above" this node
        if (node->strlen > keylen) {
            struct trie_node *new_node;

            assert(keylen == strlen);
            assert((!parent) || parent->children == node);

            new_node = new_leaf (string, strlen, ip4_address);
            node->strlen -= keylen;
            new_node->children = node;

            assert ((!parent) || (!left));

            if (parent) {
                parent->children = new_node;
            } else if (left) {
                left->next = new_node;
            } else if ((!parent) || (!left)) {
                root = new_node;
            }
            return 1;

        } else if (strlen > keylen) {

            if (node->children == NULL) {
                // Insert leaf here
                struct trie_node *new_node = new_leaf (string, strlen - keylen, ip4_address);
                node->children = new_node;
                return 1;
            } else {
                // Recur on children list, store "parent" (loosely defined)
                return _insert(string, strlen - keylen, ip4_address,
                        node->children, node, NULL);
            }
        } else {
            assert (strlen == keylen);
            if (node->ip4_address == 0) {
                node->ip4_address = ip4_address;
                return 1;
            } else {
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
            struct trie_node *new_node = new_leaf (&string[offset], keylen2, 0);
            assert ((node->strlen - keylen2) > 0);
            node->strlen -= keylen2;
            new_node->children = node;
            new_node->next = node->next;
            node->next = NULL;
            assert ((!parent) || (!left));

            if (node == root) {
                root = new_node;
            } else if (parent) {
                assert(parent->children == node);
                parent->children = new_node;
            } else if (left) {
                left->next = new_node;
            } else if ((!parent) && (!left)) {
                root = new_node;
            }

            return _insert(string, offset, ip4_address,
                    node, new_node, NULL);
        } else {
            cmp = compare_keys (node->key, node->strlen, string, strlen, &keylen);
            if (cmp < 0) {
                // No, recur right (the node's key is "less" than  the search key)
                if (node->next)
                    return _insert(string, strlen, ip4_address, node->next, NULL, node);
                else {
                    // Insert here
                    struct trie_node *new_node = new_leaf (string, strlen, ip4_address);
                    node->next = new_node;
                    return 1;
                }
            } else {
                // Insert here
                struct trie_node *new_node = new_leaf (string, strlen, ip4_address);
                new_node->next = node;
                if (node == root)
                    root = new_node;
                else if (parent && parent->children == node)
                    parent->children = new_node;
                else if (left && left->next == node)
                    left->next = new_node;
            }
        }
        return 1;
    }
}


int insert (const char *string, size_t strlen, int32_t ip4_address) {
    // Skip strings of length 0
    if (strlen == 0)
        return 0;

    pthread_mutex_lock(&mutex);
    int insert_res;

    /* Edge case: root is null */
    if (root == NULL) {
        root = new_leaf(string, strlen, ip4_address);
        insert_res = 1;
    } else insert_res = _insert(string, strlen, ip4_address, root, NULL, NULL);
    if (node_count > max_count)
        pthread_cond_signal(&delete_cond);
    pthread_mutex_unlock(&mutex);
    return insert_res;
}


/* Recursive helper function.
 * Returns a pointer to the node if found.
 * Stores an optional pointer to the 
 * parent, or what should be the parent if not found.
 * 
 */
struct trie_node * 
_delete (struct trie_node *node, const char *string, 
        size_t strlen) {
    int keylen, cmp;

    // First things first, check if we are NULL 
    if (node == NULL) return NULL;

    assert(node->strlen < 64);

    // See if this key is a substring of the string passed in
    cmp = compare_keys_substring (node->key, node->strlen, string, strlen, &keylen);
    if (cmp == 0) {
        // Yes, either quit, or recur on the children

        // If this key is longer than our search string, the key isn't here
        if (node->strlen > keylen) {
            return NULL;
        } else if (strlen > keylen) {
            struct trie_node *found =  _delete(node->children, string, strlen - keylen);
            if (found) {
                /* If the node doesn't have children, delete it.
                 * Otherwise, keep it around to find the kids */
                if (found->children == NULL && found->ip4_address == 0) {
                    assert(node->children == found);
                    node->children = found->next;
                    free(found);
                    node_count--;
                }

                /* Delete the root node if we empty the tree */
                if (node == root && node->children == NULL && node->ip4_address == 0) {
                    root = node->next;
                    free(node);
                    node_count--;
                }

                return node; /* Recursively delete needless interior nodes */
            } else 
                return NULL;
        } else {
            assert (strlen == keylen);

            /* We found it! Clear the ip4 address and return. */
            if (node->ip4_address) {
                node->ip4_address = 0;

                /* Delete the root node if we empty the tree */
                if (node == root && node->children == NULL && node->ip4_address == 0) {
                    root = node->next;
                    free(node);
                    node_count--;
                    return (struct trie_node *) 0x100100; /* XXX: Don't use this pointer for anything except 
                                                           * comparison with NULL, since the memory is freed.
                                                           * Return a "poison" pointer that will probably 
                                                           * segfault if used.
                                                           */
                }
                return node;
            } else {
                /* Just an interior node with no value */
                return NULL;
            }
        }

    } else {
        cmp = compare_keys (node->key, node->strlen, string, strlen, &keylen);
        if (cmp < 0) {
            // No, look right (the node's key is "less" than  the search key)
            struct trie_node *found = _delete(node->next, string, strlen);
            if (found) {
                /* If the node doesn't have children, delete it.
                 * Otherwise, keep it around to find the kids */
                if (found->children == NULL && found->ip4_address == 0) {
                    assert(node->next == found);
                    node->next = found->next;
                    free(found);
                    node_count--;
                }       

                return node; /* Recursively delete needless interior nodes */
            }
            return NULL;
        } else {
            // Quit early
            return NULL;
        }
    }
}



int delete  (const char *string, size_t strlen) {
    // Skip strings of length 0
    if (strlen == 0)
        return 0;

    pthread_mutex_lock(&mutex);
    struct trie_node *delete_result = _delete(root, string, strlen);
    pthread_mutex_unlock(&mutex);
    return (NULL != delete_result);
}


/* Find one node to remove from the tree. 
 * Use any policy you like to select the node.
 */
int drop_one_node() {
    puts("test");
    struct trie_node *node = root;
    int size = 0;

    // Find the end of some string.
    do {
        size += node->strlen;
    } while ((node = node->children));
    node = root;
    assert(node->key != NULL);
    char *key = malloc(size + 1);
    key[size] = '\0';
    do {
        assert(node->key != NULL);
        size -= node->strlen;
        memcpy(key + size, node->key, node->strlen);
    } while ((node = node->children));

    assert(node == NULL);
    int res = (_delete(root, key, strlen(key)) != NULL);

    free(key);
    return res;
}



/* Check the total node count; see if we have exceeded a the max.
*/
void check_max_nodes() {
    pthread_mutex_lock(&mutex);
    pthread_cond_wait(&delete_cond, &mutex);
    while (node_count > max_count)
        assert(drop_one_node());
    pthread_mutex_unlock(&mutex);
}


void _print (struct trie_node *node, int depth, char lines[100]) {
    printf("%s", lines);
    if (!node->next)
        printf("└");
    else
        printf("├");
    printf ("%.*s, IP %d, This %p, Next %p, Children %p\n",
            node->strlen, node->key, node->ip4_address, node, node->next, node->children);
    if (node->children) {
        if (node->next)
            strcat(lines, "| ");
        else strcat(lines, "  ");
        _print(node->children, depth+1, lines);
        lines[2*depth] = '\0';
    }
    if (node->next)
        _print(node->next, depth, lines);
}


void print() {
    printf ("Root is at %p\n", root);
    char lines[100];
    lines[0] = '\0';
    if (root)
        _print(root, 0, lines);
}
