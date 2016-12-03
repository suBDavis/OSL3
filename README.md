OSL3
=======================
Third lab for COMP530 Fall 2016


Exercise 5 Locking Protocol
-----------------------
Fine-grained locking could only be implemented on was implemented only insert/search.

Generally, recursive functions are called with node already locked, and it is the responsibility of the recursive call to unlock itself before returning.

It was necessary to modify the trie struct to include a pointer to the parent's mutex such that parent_mutex could be unlocked from node's depth in the stack before descending further.

### Insert

Used fairly standard hand-over-hand locking

* On recursive _insert:
 * lock the node mutex and either right_mutex or child_mutex depending on direction.
* On return:
 * unlock the node_mutex, parent_mutex, and left_mutex

### Search

Also used standard hand-over-hand, as search is unaffected by deletes happening above the leaf or in different parts of the trie.

### Delete

Uses a variant of coarse-grained locking in which lockes are maintained all the way down the call stack, and released as each stack frame returns.

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


Extra credit attempted:
-----------------------
* Improved print function