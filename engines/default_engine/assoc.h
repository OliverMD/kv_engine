#ifndef ASSOC_H
#define ASSOC_H

/* associative array */
ENGINE_ERROR_CODE assoc_init(struct default_engine *engine);
void assoc_destroy(void);
hash_item *assoc_find(uint32_t hash, const hash_key* key);
int assoc_insert(uint32_t hash, hash_item *item);
void assoc_delete(uint32_t hash, const hash_key* key);

#endif
