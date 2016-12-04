OSL3
=======================
Third lab for COMP530 Fall 2016


Exercise 5 Locking Protocol
-----------------------
True fine-grained locking could only be implemented on was implemented only insert/search.

Generally, recursive functions are called with node already locked, and it is the responsibility of the recursive call to unlock itself before returning.

A trie-wide lock is necessary when checking if root is null to maintain order of operations, for example running an insert followed by a delete of the same string. Without the lock, it would be possible for delete to check if the root is null and return before the string is inserted. It's also necessary in fine-grained locking when swapping root with another node.


### Insert

Used fairly standard hand-over-hand locking

* On recursive _insert:
 * lock the node mutex and either right_mutex or child_mutex depending on direction.
* On return:
 * unlock the node_mutex, parent_mutex, and left_mutex

### Search

Also used standard hand-over-hand, as search is unaffected by deletes happening above the leaf or in different parts of the trie.

### Delete

Uses a variant of coarse-grained locking in which locks are maintained all the way down the call stack, and released as each stack frame returns.

This was necessary because of cases where insert and delete cause conflicting changes to the trie.  For example.

```
fig 1.      |  fig. 2
            | 
 A          |  
 |          |  
 S -> BC    |  -> BC
 |          |
*D          |
 |          |
*F          |

```

If ASDF were deleted from the above trie (and locks are denoted by `*`) at the same time as ABC were being inserted, it is possible that the trie structure of fig 1. could be swept out from under BC, leaving a NULL root and an orphaned BC.

### check_max_nodes and drop_one_node

Due to the nature of the drop_one_node implementation, the complete traversal path must remain locked in order to avoid a similar issue to the one in Delete. Additionally, the lock needs to remain between building the string and calling \_delete, so there are effectively no benefits to a hand-over-hand locking of drop_one_node. Therefore, check_max_nodes is locked in the same fashion as in ex3 using the trie-wide mutex.


Note on rw-trie and delete thread
-----------------------
As RW locks do not support conditions, it is not possible to have a delete thread with rw-trie.


Extra credit attempted:
-----------------------
* Improved print function
