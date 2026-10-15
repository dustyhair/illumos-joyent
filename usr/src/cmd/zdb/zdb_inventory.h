#ifndef ZDB_INVENTORY_H
#define ZDB_INVENTORY_H
int zdb_inventory_discover(const char *, char **, int, nvlist_t **);
int zdb_inventory_import(const char *, nvlist_t *);
void zdb_inventory_verify_mode(void);
void zdb_inventory_pair_mode(void);
int zdb_inventory_begin(const char *);
int zdb_inventory_run(spa_t *);
int zdb_inventory_end(int);
#endif
