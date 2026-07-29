/*
 * so_util.h -- utils to load and hook .so modules
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

extern void *text_base, *data_base;
extern size_t text_size, data_size;

void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_make_text_writable(void);
void so_make_text_executable(void);
void so_flush_caches(void);
void so_free_temp(void);
int so_load(const char *filename, void *base, size_t max_size);
int so_relocate(void);
int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
int so_register_eh_frame(void);  /* registra .eh_frame do módulo ativo (exceções C++) */
void so_record_phdr(const char *name);  /* salva phdrs p/ o dl_iterate_phdr custom */
struct so_phdr_mod { uintptr_t base; Elf64_Phdr ph[20]; int phnum; char name[32]; };
extern struct so_phdr_mod g_so_mods[4];
extern int g_so_nmods;
void so_execute_init_array(void);
uintptr_t so_find_addr(const char *symbol);
uintptr_t so_find_addr_safe(const char *symbol);
uintptr_t so_find_addr_rx(const char *symbol);
uintptr_t so_find_rel_addr(const char *symbol);
uintptr_t so_find_rel_addr_safe(const char *symbol);
int so_patch_got(const char *symbol, uintptr_t val);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name);
void so_finalize(void);
int so_unload(void);

typedef struct so_module so_module;
so_module *so_save(void);
void so_use(so_module *m);
void so_release_module_temp(so_module *m);

#endif
