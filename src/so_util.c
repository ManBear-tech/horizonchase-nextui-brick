/*
 * so_util.c -- utils to load and hook .so modules
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "error.h"
#include "so_util.h"
#include "util.h"

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif

void *text_base, *text_virtbase;
size_t text_size;

void *data_base, *data_virtbase;
size_t data_size;

static void *load_base, *load_virtbase;
static size_t load_size;

static void *so_base;

static Elf64_Ehdr *elf_hdr;
static Elf64_Phdr *prog_hdr;
static Elf64_Shdr *sec_hdr;
static Elf64_Sym *syms;
static int num_syms;

static char *shstrtab;
static char *dynstrtab;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_make_text_writable(void) {
  const size_t text_asize = ALIGN_MEM(text_size, 0x1000);
  if (mprotect(text_virtbase, text_asize,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    debugPrintf("Warning: Could not make text segment writable\n");
  }
}

/* restaura a permissão REAL de cada PT_LOAD (não "tudo RX": o módulo tem RW depois
 * do RX no NDK novo, e deixá-los RX quebra qualquer escrita em .data/.bss) */
void so_make_text_executable(void) { so_finalize(); }

void so_flush_caches(void) {
  __builtin___clear_cache((char *)load_virtbase,
                          (char *)load_virtbase + load_size);
}

void so_free_temp(void) {
  free(so_base);
  so_base = NULL;
}

static inline size_t round_up(size_t x, size_t a) {
  return (x + a - 1) & ~(a - 1);
}

static int protect_range(void *start, size_t len, int prot) {
  int ps = getpagesize();
  if (ps <= 0)
    ps = 4096;
  uintptr_t addr = (uintptr_t)start;
  uintptr_t page_base = addr & ~((uintptr_t)ps - 1);
  size_t head = addr - page_base;
  size_t plen = round_up(len + head, (size_t)ps);
  if (mprotect((void *)page_base, plen, prot) != 0) {
    debugPrintf("mprotect(%p, %zu, 0x%x) failed: %s\n", (void *)page_base,
                plen, prot, strerror(errno));
    return -1;
  }
  return 0;
}

/* so_finalize: aplica a proteção de CADA PT_LOAD com os flags DELE — é o que o
 * linker do Android faz. O par fixo (text RX, data RW) só funcionava quando o .so
 * tinha exatamente 2 PT_LOAD com text primeiro; os .so de NDK novo (Horizon Chase)
 * têm 4 (R, RX, RW, RW) e o 2º RW ficava RX -> falha ao escrever. */
void so_finalize(void) {
  int n = 0;
  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type != PT_LOAD) continue;
    int prot = PROT_READ;
    if (prog_hdr[i].p_flags & PF_W) prot |= PROT_WRITE;
    if (prog_hdr[i].p_flags & PF_X) prot |= PROT_EXEC;
    void *seg = (void *)(prog_hdr[i].p_vaddr + (uintptr_t)load_virtbase);
    if (protect_range(seg, prog_hdr[i].p_memsz, prot) != 0)
      fatal_error("Error: mprotect PT_LOAD[%d] %p sz=%zu prot=0x%x",
                  i, seg, (size_t)prog_hdr[i].p_memsz, prot);
    n++;
  }
  if (!n) fatal_error("Error: nenhum PT_LOAD para proteger");
}

int so_load(const char *filename, void *base, size_t max_size) {
  int res = 0;
  size_t so_size = 0;
  int text_segno = -1;
  int data_segno = -1;

  debugPrintf("so_load: Opening %s\n", filename);
  FILE *fd = fopen(filename, "rb");
  if (fd == NULL) {
    debugPrintf("so_load: Failed to open file\n");
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);
  debugPrintf("so_load: File size: %zu bytes\n", so_size);

  so_base = malloc(so_size);
  if (!so_base) {
    fclose(fd);
    return -2;
  }

  if (fread(so_base, so_size, 1, fd) != 1) {
    fclose(fd);
    free(so_base);
    return -3;
  }
  fclose(fd);

  if (memcmp(so_base, ELFMAG, SELFMAG) != 0) {
    debugPrintf("so_load: Not a valid ELF file\n");
    res = -1;
    goto err_free_so;
  }

  elf_hdr = (Elf64_Ehdr *)so_base;

  if (elf_hdr->e_ident[EI_CLASS] != ELFCLASS64) {
    debugPrintf("so_load: Not a 64-bit ELF file\n");
    res = -1;
    goto err_free_so;
  }

  if (elf_hdr->e_machine != EM_AARCH64) {
    debugPrintf("so_load: Not an AArch64 ELF (machine=%d)\n",
                elf_hdr->e_machine);
    res = -1;
    goto err_free_so;
  }

  debugPrintf("so_load: ELF64 AArch64, %d program headers, %d section headers\n",
              elf_hdr->e_phnum, elf_hdr->e_shnum);

  if (elf_hdr->e_phoff + (elf_hdr->e_phnum * sizeof(Elf64_Phdr)) > so_size) {
    debugPrintf("so_load: Program headers extend beyond file\n");
    res = -1;
    goto err_free_so;
  }

  prog_hdr = (Elf64_Phdr *)((uintptr_t)so_base + elf_hdr->e_phoff);

  if (elf_hdr->e_shoff + (elf_hdr->e_shnum * sizeof(Elf64_Shdr)) > so_size) {
    debugPrintf("so_load: Section headers extend beyond file\n");
    res = -1;
    goto err_free_so;
  }

  sec_hdr = (Elf64_Shdr *)((uintptr_t)so_base + elf_hdr->e_shoff);

  if (elf_hdr->e_shstrndx >= elf_hdr->e_shnum) {
    debugPrintf("so_load: Invalid string table index\n");
    res = -1;
    goto err_free_so;
  }

  shstrtab =
      (char *)((uintptr_t)so_base + sec_hdr[elf_hdr->e_shstrndx].sh_offset);

  /* Scan dos PT_LOAD SEM assumir ordem. O linker do Android aceita qualquer
   * ordem/quantidade; os .so de NDK novo (Horizon Chase: libunity/libil2cpp)
   * começam com um PT_LOAD read-only, então "o primeiro é o text" é falso.
   * load_size = maior (vaddr+memsz) de TODOS os segmentos. */
  load_size = 0;
  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type != PT_LOAD) continue;
    const size_t end = prog_hdr[i].p_vaddr +
                       ALIGN_MEM(prog_hdr[i].p_memsz, prog_hdr[i].p_align);
    if (end > load_size) load_size = end;
    if ((prog_hdr[i].p_flags & PF_X) == PF_X) {
      if (text_segno < 0) text_segno = i;
    } else if ((prog_hdr[i].p_flags & PF_W) == PF_W) {
      if (data_segno < 0) data_segno = i;
    }
  }
  if (text_segno < 0 || data_segno < 0) {
    debugPrintf("so_load: PT_LOAD sem segmento %s\n",
                text_segno < 0 ? "executável" : "gravável");
    res = -1;
    goto err_free_so;
  }

  load_size = ALIGN_MEM(load_size, 0x1000);
  debugPrintf("so_load: Total load size: %zu bytes (max: %zu)\n", load_size,
              max_size);
  if (load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  load_base = base;
  if (!load_base)
    goto err_free_so;

  memset(load_base, 0, load_size);
  load_virtbase = load_base;

  debugPrintf("so_load: load base = %p\n", load_virtbase);

  /* Base do MÓDULO = load_base (vaddr 0). Todo o resto do so_util endereça por
   * text_base + sh_addr / + r_offset, que são vaddr do módulo — então text_base
   * TEM que ser a base, não o início do segmento executável (que no NDK novo
   * está em vaddr != 0). text_size cobre o módulo inteiro (range check de PC). */
  text_base = text_virtbase = load_base;
  text_size = load_size;
  data_size = prog_hdr[data_segno].p_memsz;
  data_virtbase = data_base =
      (void *)(prog_hdr[data_segno].p_vaddr + (Elf64_Addr)load_base);

  /* Copia TODOS os PT_LOAD no offset de vaddr deles — igual ao linker do Android.
   * (O .init_array/RELRO vive num segmento que o par text/data não cobria.) */
  for (int s = 0; s < elf_hdr->e_phnum; s++) {
    if (prog_hdr[s].p_type != PT_LOAD) continue;
    void *dst = (void *)(prog_hdr[s].p_vaddr + (Elf64_Addr)load_base);
    memcpy(dst, (void *)((uintptr_t)so_base + prog_hdr[s].p_offset),
           prog_hdr[s].p_filesz);
    debugPrintf("so_load: PT_LOAD[%d] vaddr=0x%lx filesz=0x%lx flags=%c%c%c\n", s,
                (unsigned long)prog_hdr[s].p_vaddr,
                (unsigned long)prog_hdr[s].p_filesz,
                (prog_hdr[s].p_flags & PF_R) ? 'R' : '-',
                (prog_hdr[s].p_flags & PF_W) ? 'W' : '-',
                (prog_hdr[s].p_flags & PF_X) ? 'X' : '-');
  }

  syms = NULL;
  dynstrtab = NULL;

  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      syms = (Elf64_Sym *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      num_syms = sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      dynstrtab = (char *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
    }
  }

  if (syms == NULL || dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  debugPrintf("so_load: %d dynamic symbols found\n", num_syms);
  return 0;

err_free_load:
  free(load_base);
err_free_so:
  free(so_base);
  return res;
}

int so_relocate(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        uintptr_t *ptr =
            (uintptr_t *)((uintptr_t)text_base + rels[j].r_offset);
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_AARCH64_ABS64:
          /* import (UNDEF): NAO e' base+0 — e' um simbolo externo (ex malloc@LIBC).
             Deixa o slot p/ so_resolve preencher (senao vira il2cpp_base+0 -> br
             p/ o header ELF -> SIGILL). Simbolos LOCAIS (vtables/RTTI) seguem normal. */
          if (sym->st_shndx == SHN_UNDEF)
            break;
          *ptr = (uintptr_t)text_virtbase + sym->st_value + rels[j].r_addend;
          break;
        case R_AARCH64_RELATIVE:
          *ptr = (uintptr_t)text_virtbase + rels[j].r_addend;
          break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
          if (sym->st_shndx != SHN_UNDEF)
            *ptr =
                (uintptr_t)text_virtbase + sym->st_value + rels[j].r_addend;
          break;
        default:
          fatal_error("Error: unknown relocation type: %x\n", type);
          break;
        }
      }
    }
    /* RELR (.relr.dyn / ANDROID_RELR): formato compacto de relocations RELATIVE
     * que clang/NDK moderno usa p/ vtables/ponteiros internos. SEM isso, função-
     * ptrs ficam = offset cru (não text_base+offset) -> blr p/ endereço errado. */
    else if (strcmp(sh_name, ".relr.dyn") == 0) {
      uintptr_t *relr = (uintptr_t *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      size_t n = sec_hdr[i].sh_size / sizeof(uintptr_t);
      uintptr_t base = (uintptr_t)text_virtbase;
      uintptr_t *where = NULL;
      for (size_t k = 0; k < n; k++) {
        uintptr_t e = relr[k];
        if ((e & 1) == 0) {            /* endereço: aplica 1 reloc + avança */
          where = (uintptr_t *)(base + e);
          *where++ += base;
        } else {                      /* bitmap dos próximos 63 words */
          for (int b = 0; (e >>= 1) != 0; b++)
            if (e & 1) where[b] += base;
          where += 63;
        }
      }
    }
  }
  return 0;
}

/* registra o .eh_frame do módulo ATIVO com o runtime de exceções C++ (libgcc).
 * Nossos módulos são MAPEADOS MANUALMENTE (não via dlopen) -> o dl_iterate_phdr
 * do unwinder NÃO os vê -> exceção C++ não acha o landing pad -> std::terminate
 * -> abort (visto no asset loading do il2cpp). __register_frame(.eh_frame) resolve
 * (mesma técnica de JIT/código gerado em runtime). */
extern void __register_frame(void *begin);
int so_register_eh_frame(void) {
  if (!getenv("CUP_EHREG")) return -1;   /* __register_frame crasha (formato); usar dl_iterate_phdr */
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    if (!strcmp(shstrtab + sec_hdr[i].sh_name, ".eh_frame") && sec_hdr[i].sh_size) {
      void *eh = (void *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      __register_frame(eh);
      return 0;
    }
  }
  return -1;
}

/* ---- snapshot dos phdr de cada módulo p/ o dl_iterate_phdr custom ----
 * O unwinder C++ (libgcc) acha o .eh_frame via dl_iterate_phdr, que só enxerga
 * libs do dynamic linker. Nossos módulos são mapeados à mão -> invisíveis. Salvamos
 * uma CÓPIA estável dos phdrs (com p_vaddr ORIGINAL: so_load modifica text/data p/
 * absoluto -> recuperamos subtraindo a base) + a base (dlpi_addr=load_virtbase). */
#define SO_MAX_PH 20
struct so_phdr_mod g_so_mods[4];
int g_so_nmods = 0;
void so_record_phdr(const char *name) {
  if (g_so_nmods >= 4) return;
  struct so_phdr_mod *m = &g_so_mods[g_so_nmods++];
  uintptr_t lb = (uintptr_t)load_virtbase;
  m->base = lb;
  int n = elf_hdr->e_phnum; if (n > SO_MAX_PH) n = SO_MAX_PH;
  m->phnum = n;
  for (int i = 0; i < n; i++) {
    m->ph[i] = prog_hdr[i];
    /* so_load setou p_vaddr de text/data p/ o ENDEREÇO ABSOLUTO -> volta p/ offset */
    if (m->ph[i].p_vaddr >= lb) m->ph[i].p_vaddr -= lb;
  }
  for (int i = 0; i < 31 && name[i]; i++) m->name[i] = name[i];
  m->name[31] = 0;
}

int so_resolve(DynLibFunction *funcs, int num_funcs,
               int taint_missing_imports) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        uintptr_t *ptr =
            (uintptr_t *)((uintptr_t)text_base + rels[j].r_offset);
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
        /* ABS64 p/ simbolo UNDEF = import (ex malloc@LIBC numa tabela de thunks do
           il2cpp). so_relocate pulou (senao vira base+0). Resolve aqui igual GOT,
           somando o addend. */
        case R_AARCH64_ABS64: {
          if (sym->st_shndx == SHN_UNDEF) {
            if (taint_missing_imports)
              *ptr = rels[j].r_offset;

            char *name = dynstrtab + sym->st_name;
            uintptr_t add = (type == R_AARCH64_ABS64) ? rels[j].r_addend : 0;
            int found = 0;
            for (int k = 0; k < num_funcs; k++) {
              if (strcmp(name, funcs[k].symbol) == 0) {
                *ptr = funcs[k].func + add;
                found = 1;
                break;
              }
            }
            if (!found) {
              /* fallback: libc/libm/zlib/socket/dl padrão resolvem no glibc real do device */
              void *real = dlsym(RTLD_DEFAULT, name);
              if (real) { *ptr = (uintptr_t)real + add; found = 1; }
            }
            if (!found)
              fprintf(stderr, "*** UNRESOLVED import: \"%s\" (GOT offset 0x%lx) ***\n",
                      name, (unsigned long)rels[j].r_offset);
          }
          break;
        }
        default:
          break;
        }
      }
    }
  }
  return 0;
}

void so_execute_init_array(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int (**init_array)() =
          (void *)((uintptr_t)text_virtbase + sec_hdr[i].sh_addr);
      int cnt = (int)(sec_hdr[i].sh_size / 8);
      for (int j = 0; j < cnt; j++) {
        uintptr_t fn = (uintptr_t)init_array[j];
        if (j < 6)
          debugPrintf("init_array[%d/%d] = %p (off 0x%lx)\n", j, cnt, (void *)fn,
                      (unsigned long)(fn - (uintptr_t)text_virtbase));
        if (fn == 0 || fn == (uintptr_t)text_virtbase) {
          debugPrintf("init_array[%d] = base+0/NULL -> PULANDO\n", j);
          continue;
        }
        init_array[j]();
      }
    }
  }
}

uintptr_t so_find_addr(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)text_base + syms[i].st_value;
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_addr_safe(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0) {
      /* import (UNDEF, st_value=0): dlsym real devolve NULL; sem este guard
         devolviamos text_base+0 = base do modulo -> ponteiro lixo no Unity */
      if (syms[i].st_shndx == SHN_UNDEF)
        continue;
      return (uintptr_t)text_base + syms[i].st_value;
    }
  }
  return 0;
}

uintptr_t so_find_addr_rx(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)text_virtbase + syms[i].st_value;
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_rel_addr(const char *symbol) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);
        if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
          char *name = dynstrtab + sym->st_name;
          if (strcmp(name, symbol) == 0)
            return (uintptr_t)text_base + rels[j].r_offset;
        }
      }
    }
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

/* patcheia TODOS os slots da GOT (GLOB_DAT + JUMP_SLOT) de um simbolo.
 * so_find_rel_addr_safe so' devolve o 1o; o PLT pode usar outro. Retorna a contagem. */
int so_patch_got(const char *symbol, uintptr_t val) {
  int n = 0;
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);
        if ((type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) &&
            strcmp(dynstrtab + sym->st_name, symbol) == 0) {
          *(uintptr_t *)((uintptr_t)text_base + rels[j].r_offset) = val;
          n++;
        }
      }
    }
  }
  return n;
}

uintptr_t so_find_rel_addr_safe(const char *symbol) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)text_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);
        if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
          char *name = dynstrtab + sym->st_name;
          if (strcmp(name, symbol) == 0)
            return (uintptr_t)text_base + rels[j].r_offset;
        }
      }
    }
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(void) {
  if (load_base == NULL)
    return -1;
  if (so_base)
    so_free_temp();
  if (munmap(load_base, load_size) != 0)
    fatal_error("Error: could not unmap library memory");
  return 0;
}

/* ===== multi-módulo (libunity + libil2cpp): salva/restaura o estado do loader ===== */
struct so_module {
  void *text_base, *text_virtbase, *data_base, *data_virtbase;
  void *load_base, *load_virtbase, *so_base;
  size_t text_size, data_size, load_size;
  Elf64_Ehdr *elf_hdr; Elf64_Phdr *prog_hdr; Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms; int num_syms; char *shstrtab, *dynstrtab;
};
so_module *so_save(void) {
  so_module *m = (so_module *)malloc(sizeof(so_module));
  m->text_base=text_base; m->text_virtbase=text_virtbase;
  m->data_base=data_base; m->data_virtbase=data_virtbase;
  m->load_base=load_base; m->load_virtbase=load_virtbase; m->so_base=so_base;
  m->text_size=text_size; m->data_size=data_size; m->load_size=load_size;
  m->elf_hdr=elf_hdr; m->prog_hdr=prog_hdr; m->sec_hdr=sec_hdr;
  m->syms=syms; m->num_syms=num_syms; m->shstrtab=shstrtab; m->dynstrtab=dynstrtab;
  return m;
}
void so_use(so_module *m) {
  text_base=m->text_base; text_virtbase=m->text_virtbase;
  data_base=m->data_base; data_virtbase=m->data_virtbase;
  load_base=m->load_base; load_virtbase=m->load_virtbase; so_base=m->so_base;
  text_size=m->text_size; data_size=m->data_size; load_size=m->load_size;
  elf_hdr=m->elf_hdr; prog_hdr=m->prog_hdr; sec_hdr=m->sec_hdr;
  syms=m->syms; num_syms=m->num_syms; shstrtab=m->shstrtab; dynstrtab=m->dynstrtab;
}

/*
 * so_load needs the complete ELF file image while relocations and GOT patches
 * inspect section headers. Once a module's final patch has been installed, its
 * executable/data image is self-contained and only dynsym/dynstr (inside the
 * mapped PT_LOAD) remain necessary for runtime dlsym.
 *
 * Keep the saved and active module states in sync so a later so_use cannot
 * resurrect a dangling pointer and accidentally free the same 25/48 MiB image
 * twice.
 */
void so_release_module_temp(so_module *m) {
  if (!m || !m->so_base)
    return;
  void *temp = m->so_base;
  m->so_base = NULL;
  if (so_base == temp)
    so_base = NULL;
  free(temp);
}
