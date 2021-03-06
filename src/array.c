/*
 * array.c
 * Sparkling, a lightweight C-style scripting language
 *
 * Created by Árpád Goretity on 02/05/2013
 * Licensed under the 2-clause BSD License
 *
 * Arrays: fast associative array implementation
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "array.h"

#if UINT_MAX <= 0xffff
#define SPN_LOW_MEMORY_PLATFORM 1
#else
#define SPN_LOW_MEMORY_PLATFORM 0
#endif


#if SPN_LOW_MEMORY_PLATFORM
#define HASH_NSIZES	12
#define ARRAY_MAXSIZE	0x8000
#else
#define HASH_NSIZES	24
#define ARRAY_MAXSIZE	0x4000000
#endif


/* 
 * The usual trick for dynamically sized arrays holds for the hash table
 * part -- expand storage exponentially:
 * 
 * 	if (size + 1 > allocation_size) {
 * 		allocation_size *= 2;
 * 	}
 *
 * However, we don't want the size to be a power of two, since that would
 * simply truncate the hash to its least significant bits, decreasing entropy
 * (the key for a particular value is taken modulo the size of the array).
 * Instead, the sizes are prime numbers close to consecutive powers of two,
 * from 2^3 to 2^25, inclusive.
 */
static unsigned long nthsize(int idx)
{
	static const unsigned long szprims[HASH_NSIZES] = {
		0,		7,		17,		29,
		67,		131,		257,		509,
		1031,		2053,		4099,		8209,
#if !SPN_LOW_MEMORY_PLATFORM
		16381,		32771,		65539,		131063,
		262139,		524287,		1048583,	2097169,
		4194301,	8388617,	16777213,	33554467
#endif
	};

	return szprims[idx];
}

typedef struct KVPair {
	SpnValue key;
	SpnValue val;
} KVPair;

typedef struct TList {
	KVPair pair;
	struct TList *next;
} TList;

struct SpnArray {
	SpnObject	  base;		/* for being a valid object		*/
	SpnValue	  nilval;	/* for returning nil			*/

	SpnValue	 *arr;		/* the array part			*/
	size_t		  arrcnt;	/* logical size				*/
	size_t		  arrallsz;	/* allocation (actual) size		*/

	TList		**buckets;	/* the hash table part			*/
	size_t		  hashcnt;	/* logical size				*/
	int		  hashszidx;	/* index of allocation (actual) size	*/
};

struct SpnIterator {
	SpnArray	 *arr;		/* weak reference to owning array	*/
	size_t		  idx;		/* ordinal number of key-value pair	*/
	size_t		  cursor;	/* the essence				*/
	TList		 *list;		/* linked list entry for the hash part	*/
	int		  inarray;	/* helper flag for traversing hash part	*/
};


static void free_array(void *obj);


static const SpnClass spn_class_array = {
	"array",
	sizeof(SpnArray),
	NULL,
	NULL,
	NULL,
	free_array
};


static TList *list_prepend(TList *head, SpnValue *key, SpnValue *val);
static KVPair *list_find(TList *head, const SpnValue *key);
static TList *list_delete_releasing(TList *head, const SpnValue *key);
static void list_free_releasing(TList *head);


static void expand_array_if_needed(SpnArray *arr, unsigned long idx);
static void insert_and_update_count_array(SpnArray *arr, unsigned long idx, SpnValue *val);
static void expand_hash(SpnArray *arr);
static void insert_and_update_count_hash(SpnArray *arr, SpnValue *key, SpnValue *val);

SpnArray *spn_array_new()
{
	SpnArray *arr = spn_object_new(&spn_class_array);

	arr->nilval.t = SPN_TYPE_NIL;
	arr->nilval.f = 0;

	arr->arr = NULL;
	arr->arrcnt = 0;
	arr->arrallsz = 0;

	arr->buckets = NULL;
	arr->hashcnt = 0;
	arr->hashszidx = 0;

	return arr;
}

static void free_array(void *obj)
{
	SpnArray *arr = obj;
	size_t i;

	for (i = 0; i < arr->arrallsz; i++) {
		spn_value_release(&arr->arr[i]);
	}

	free(arr->arr);

	for (i = 0; i < nthsize(arr->hashszidx); i++) {
		list_free_releasing(arr->buckets[i]);
	}

	free(arr->buckets);
	free(arr);
}

size_t spn_array_count(SpnArray *arr)
{
	return arr->arrcnt + arr->hashcnt;
}

/* getter and setter */

SpnValue *spn_array_get(SpnArray *arr, const SpnValue *key)
{
	unsigned long hash;
	TList *list;
	KVPair *hit;
	
	/* integer key of which the value fits into the array part */
	if (key->t == SPN_TYPE_NUMBER) {
		if (key->f & SPN_TFLG_FLOAT) {
			long i = key->v.fltv;
			if (key->v.fltv == i /* int disguised as a float */
			 && 0 <= i
			 && i < ARRAY_MAXSIZE) {
				unsigned long uidx = i; /* silent the -Wsign-compare warning */
			 	return uidx < arr->arrallsz ? &arr->arr[i] : &arr->nilval;
			}
		} else {
			long i = key->v.intv;
			if (0 <= i && i < ARRAY_MAXSIZE) {
				unsigned long uidx = i; /* silent the -Wsign-compare warning */
				return uidx < arr->arrallsz ? &arr->arr[i] : &arr->nilval;
			}
		}
	}

	/* else: the value goes to/comes from the hash table part */

	/* if the hash table is empty, it cannot contain any value at all */
	if (nthsize(arr->hashszidx) == 0) {
		return &arr->nilval;
	}

	/* else return what the hash part can find */
	hash = spn_hash_value(key) % nthsize(arr->hashszidx);

	/* return what's found in the link list at the computed index
	 * (behaves correctly if the list is empty/NULL)
	 */
	list = arr->buckets[hash];
	hit = list_find(list, key);
	return hit != NULL ? &hit->val : &arr->nilval;
}

void spn_array_set(SpnArray *arr, SpnValue *key, SpnValue *val)
{
	/* integer key of which the value fits into the array part */
	if (key->t == SPN_TYPE_NUMBER) {
		if (key->f & SPN_TFLG_FLOAT) {
			long i = key->v.fltv;	/* truncate */
			if (key->v.fltv == i	/* is it actually an integer? */
			 && 0 <= i			/* fits into array part? */
			 && i < ARRAY_MAXSIZE) {
				insert_and_update_count_array(arr, i, val);
				return;
			}
		} else {
			long i = key->v.intv;
			if (0 <= i && i < ARRAY_MAXSIZE) { /* fits into array part? */
				insert_and_update_count_array(arr, i, val);
				return;
			}
		}
	}

	/* if they key is neither an integer that fits into the array part,
	 * nor an integer disguised as a float which fits into the array part
	 * (i. e. it's negative, too large, nil, boolean, a function
	 * or an object), then it's inserted into the hash table part instead.
	 */

	insert_and_update_count_hash(arr, key, val);
}

void spn_array_remove(SpnArray *arr, SpnValue *key)
{
	SpnValue nilval = { { 0 }, SPN_TYPE_NIL, 0 };
	spn_array_set(arr, key, &nilval);
}

/* iterators */

SpnIterator *spn_iter_new(SpnArray *arr)
{
	SpnIterator *it = malloc(sizeof(*it));
	if (it == NULL) {
		abort();
	}

	it->arr = arr;
	it->idx = 0;
	it->cursor = 0;
	it->list = NULL;
	it->inarray = 1;

	return it;
}

SpnArray *spn_iter_getarray(SpnIterator *it)
{
	return it->arr;
}

void spn_iter_free(SpnIterator *it)
{
	free(it);
}

size_t spn_iter_next(SpnIterator *it, SpnValue *key, SpnValue *val)
{
	SpnArray *arr = it->arr;
	size_t hashsz, i;

	/* search the array part first: cursor in [0...arraysize) (*) */
	if (it->inarray) {
		for (i = it->cursor; i < arr->arrallsz; i++) {
			if (arr->arr[i].t != SPN_TYPE_NIL) {
				/* set up key */
				key->t = SPN_TYPE_NUMBER;
				key->f = 0;
				key->v.intv = i;

				/* set up value */
				*val = arr->arr[i];

				/* advance cursor to next base index */
				it->cursor = i + 1;

				return it->idx++;
			}
		}

		/* if not found, search hash part: cursor in [0...hashsize) */
		it->cursor = 0; /* reset to point to the beginning */
		it->inarray = 0; /* indicate having finished with the array part */
	} else {
		if (it->list != NULL) {
			*key = it->list->pair.key;
			*val = it->list->pair.val;
			it->list = it->list->next;
			return it->idx++;
		}

		/* if the node is NULL, it means we finished the traversal of
		 * the linked list, so we fall through in order to search for
		 * the next bucket
		 */
	}

	hashsz = nthsize(arr->hashszidx);
	for (i = it->cursor; i < hashsz; i++) {
		if (arr->buckets[i] != NULL) {
			it->list = arr->buckets[i];
			it->cursor = i + 1;
			/* sorry for having been clever here, but this is more
			 * convenient with one level of recursion and it
			 * avoids redundancy as well
			 */
			return spn_iter_next(it, key, val);
		}
	}

	/* if not found, there are no more entries */
	assert(it->idx == spn_array_count(arr)); /* sanity check */
	return it->idx;
}

/* 
 * Linked lists for collision resolution with separate chaining
 *
 * -----
 *
 * prepend and not append, so that we can write something like
 * 
 * 	head = list_prepend(head, key_val_pair);
 * 
 * and have O(1) running time, instead of appending to the end
 * by traversing the whole list each time a collision occurs, which is O(n)
 *
 * This function does not retain the key and the value, because
 * this very same function is used when performing the rehash upon
 * expansion, and there no change is needed in the ownership.
 * Thus, when a new non-nil value is actually inserted, then we
 * do the retaining manually in spn_array_set().
 */
static TList *list_prepend(TList *head, SpnValue *key, SpnValue *val)
{
	TList *node = malloc(sizeof(*node));
	if (node == NULL) {
		abort();
	}

	node->pair.key = *key;
	node->pair.val = *val;
	node->next = head;
	return node;
}

static KVPair *list_find(TList *head, const SpnValue *key)
{
	while (head != NULL) {
		KVPair *pair = &head->pair;
		SpnValue *pkey = &pair->key;

		if (spn_value_equal(key, pkey)) {
			return pair;
		}

		head = head->next;
	}

	return NULL;
}

static TList *list_delete_releasing(TList *head, const SpnValue *key)
{
	if (head == NULL) {
		return NULL;
	}

	if (spn_value_equal(&head->pair.key, key)) {
		TList *tmp = head->next;

		spn_value_release(&head->pair.key);
		spn_value_release(&head->pair.val);

		free(head);
		return tmp;
	}

	head->next = list_delete_releasing(head->next, key);
	return head;
}

static void list_free_releasing(TList *node)
{
	while (node != NULL) {
		TList *tmp = node->next;

		spn_value_release(&node->pair.key);
		spn_value_release(&node->pair.val);

		free(node);
		node = tmp;
	}
}

/* Low-level array and hash table helpers */
static void expand_array_if_needed(SpnArray *arr, unsigned long idx)
{
	size_t prevsz = arr->arrallsz;
	size_t i;

	if (idx < arr->arrallsz) {
		return;			/* there's enough room, nothing to do	*/
	}

	if (arr->arrallsz == 0) {	/* if it was empty, allocate space	*/
		arr->arrallsz = 0x4;
	}

	while (idx >= arr->arrallsz) {	/* expand until index fits in vector	*/
		arr->arrallsz <<= 1;
	}

	/* ask the OS to do its job */
	arr->arr = realloc(arr->arr, sizeof(arr->arr[0]) * arr->arrallsz);
	if (arr->arr == NULL) {
		abort();
	}

	/* and fill all the not-yet-existent fields with nil */
	for (i = prevsz; i < arr->arrallsz; i++) {
		arr->arr[i].t = SPN_TYPE_NIL;
		arr->arr[i].f = 0;
	}
}

static void insert_and_update_count_array(SpnArray *arr, unsigned long idx, SpnValue *val)
{
	/* check if the array needs to expand beyond its current size */
	expand_array_if_needed(arr, idx);

	/* then check if a previously nonexistent object is inserted... */
	if (arr->arr[idx].t == SPN_TYPE_NIL
	 && val->t != SPN_TYPE_NIL) {
		arr->arrcnt++;
	}

	/* ...or conversely, a previously existent object is deleted */
	if (arr->arr[idx].t != SPN_TYPE_NIL
	 && val->t == SPN_TYPE_NIL) {
		arr->arrcnt--;
	}

	/* then actually update the value in the array */

	/* RBR idiom: retain first... */
	spn_value_retain(val);

	/* and release only then, in case the old and the new object is
	 * the same and the only reference to the object is held by the array.
	 * In that case, if we released it first, then it would be deallocated,
	 * and the retain function would operate on garbage...
	 */
	spn_value_release(&arr->arr[idx]);		
	arr->arr[idx] = *val;
}

static void expand_hash(SpnArray *arr)
{
	size_t newsz, i, oldsz = nthsize(arr->hashszidx);
	TList **new_buckets;

	/* we must perform a complete re-hash upon expansion,
	 * since the hash values depend on the size of the bucket array.
	 * 
	 * If the hash table reached its maximal capacity,
	 * it cannot be expanded further.
	 */
	if (++arr->hashszidx >= HASH_NSIZES) {
		/* error, array too large */
		fputs("Sparkling: requested array size is too large\n", stderr);
		abort();
	}

	/* expand the bucket vector */
	newsz = nthsize(arr->hashszidx);
	new_buckets = malloc(sizeof(new_buckets[0]) * newsz);
	if (new_buckets == NULL) {
		abort();
	}

	for (i = 0; i < newsz; i++) {
		new_buckets[i] = NULL;
	}

	/* and do a complete rehash */
	for (i = 0; i < oldsz; i++) {
		TList *list = arr->buckets[i];

		while (list != NULL) {
			TList *tmp;
			KVPair *pair = &list->pair;
			unsigned long hash = spn_hash_value(&pair->key) % newsz;
			new_buckets[hash] = list_prepend(new_buckets[hash], &pair->key, &pair->val);

			/* free old list in-place */
			tmp = list->next;
			free(list);
			list = tmp;
		}
	}

	free(arr->buckets);
	arr->buckets = new_buckets;
}

static void insert_and_update_count_hash(SpnArray *arr, SpnValue *key, SpnValue *val)
{
	unsigned long hash, thash;
	KVPair *pair;

	/* if the hash table is empty, then it "contains nil values only"
	 * (i. e., for any key, the getter function will return nil).
	 * As a consequence, if the value to be inserted is nil, then
	 * we can just do nothing, else the hash part must be expanded
	 * in order it to be able to hold the first (non-nil) value.
	 */
	if (nthsize(arr->hashszidx) == 0) {
		if (val->t == SPN_TYPE_NIL) {
			return;
		}

		expand_hash(arr);
	}

	thash = spn_hash_value(key);
	hash = thash % nthsize(arr->hashszidx);

	/* new element? */
	pair = list_find(arr->buckets[hash], key);
	if (pair == NULL) {
		/* element not yet in hash table */

		/* if this is a new element, then `val` is non-nil, so we
		 * increment the count of the hash part
		 * check if the load factor is greater than 1/2, and if so,
		 * double the size to minimize collisions
		 */
		if (val->t != SPN_TYPE_NIL) {
			/* check load factor */
			if (arr->hashcnt > nthsize(arr->hashszidx) / 2) {
				expand_hash(arr);
				/* the hash changes after resizing! */
				hash = thash % nthsize(arr->hashszidx);
			}
			
			/* increment hash count, retain key and value, then
			 * insert them at the beginning of the link list
			 */
			arr->hashcnt++;

			spn_value_retain(key);
			spn_value_retain(val);

			arr->buckets[hash] = list_prepend(arr->buckets[hash], key, val);
		}

		/* else: if the type of the new value is nil, and it wasn't
		 * found in the hash table either, then we're setting nil to nil
		 * -- that's effectively a no-op.
		 */
	} else {
		/* The key-value pair is already in the hash table. Check the
		 * value to be inserted for nil. If it is, then delete
		 * the key and the value and decrease the count.
		 * If it isn't nil, then just change the old *value*
		 * to the new one and do nothing with the key and count.
		 */
		if (val->t == SPN_TYPE_NIL) {
			arr->buckets[hash] = list_delete_releasing(arr->buckets[hash], key);
			arr->hashcnt--;
		} else {
			/* retain first, then release (RBR idiom) */
			spn_value_retain(val);
			spn_value_release(&pair->val);
			pair->val = *val;
		}
	}
}

