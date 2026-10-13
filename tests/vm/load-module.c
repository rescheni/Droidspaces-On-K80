// SPDX-License-Identifier: GPL-2.0-only
/* Test-VM equivalent of ksud's kallsyms relocation, without vermagic rewriting. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(const char *reason)
{
	fprintf(stderr, "load-module: %s\n", reason);
	exit(1);
}

static void bounds(uint64_t offset, uint64_t size, size_t total)
{
	if (offset > total || size > total - offset)
		fail("ELF range is outside the file");
}

int main(int argc, char **argv)
{
	struct stat st;
	FILE *file, *kallsyms;
	unsigned char *data;
	Elf64_Ehdr eh;
	Elf64_Shdr *sections, *symtab = NULL, *strtab;
	Elf64_Sym *symbols;
	size_t size, count, i;
	char *line = NULL;
	size_t capacity = 0;
	int missing = 0;

	if (argc != 2 && argc != 3)
		fail("usage: load-module module.ko [parameters]");
	file = fopen(argv[1], "rb");
	if (!file || fstat(fileno(file), &st) || st.st_size < (off_t)sizeof(eh))
		fail("cannot read module");
	if (st.st_size > 64 * 1024 * 1024)
		fail("module too large for test loader");
	size = (size_t)st.st_size;
	data = malloc(size);
	if (!data || fread(data, 1, size, file) != size)
		fail("cannot read entire module");
	fclose(file);
	memcpy(&eh, data, sizeof(eh));
	if (memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh.e_machine != EM_AARCH64 || eh.e_type != ET_REL ||
	    eh.e_shentsize != sizeof(Elf64_Shdr) || !eh.e_shnum ||
	    eh.e_shoff % _Alignof(Elf64_Shdr))
		fail("unsupported module ELF");
	bounds(eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr), size);
	sections = (void *)(data + eh.e_shoff);
	for (i = 0; i < eh.e_shnum; i++) {
		if (sections[i].sh_type == SHT_SYMTAB) {
			if (symtab)
				fail("multiple symbol tables");
			symtab = &sections[i];
		}
	}
	if (!symtab || symtab->sh_link >= eh.e_shnum ||
	    symtab->sh_entsize != sizeof(Elf64_Sym) ||
	    symtab->sh_size % sizeof(Elf64_Sym) ||
	    symtab->sh_offset % _Alignof(Elf64_Sym))
		fail("invalid symbol table");
	strtab = &sections[symtab->sh_link];
	if (strtab->sh_type != SHT_STRTAB)
		fail("invalid string table");
	bounds(symtab->sh_offset, symtab->sh_size, size);
	bounds(strtab->sh_offset, strtab->sh_size, size);
	symbols = (void *)(data + symtab->sh_offset);
	count = symtab->sh_size / sizeof(Elf64_Sym);
	for (i = 1; i < count; i++) {
		if (symbols[i].st_name >= strtab->sh_size ||
		    !memchr(data + strtab->sh_offset + symbols[i].st_name, 0,
			    strtab->sh_size - symbols[i].st_name))
			fail("unterminated symbol name");
	}
	kallsyms = fopen("/proc/kallsyms", "r");
	if (!kallsyms)
		fail("cannot open kallsyms");
	while (getline(&line, &capacity, kallsyms) >= 0) {
		unsigned long long address;
		char type, name[512], owner[512];

		if (sscanf(line, "%llx %c %511s %511s", &address, &type, name, owner) != 3 ||
		    !address)
			continue;
		for (i = 1; i < count; i++) {
			const char *wanted = (char *)data + strtab->sh_offset + symbols[i].st_name;
			if (symbols[i].st_shndx == SHN_UNDEF && !strcmp(wanted, name)) {
				symbols[i].st_value = address;
				symbols[i].st_shndx = SHN_ABS;
			}
		}
	}
	fclose(kallsyms);
	free(line);
	for (i = 1; i < count; i++) {
		if (symbols[i].st_shndx == SHN_UNDEF) {
			fprintf(stderr, "unresolved: %s\n",
				data + strtab->sh_offset + symbols[i].st_name);
			missing++;
		}
	}
	if (missing)
		fail("unresolved symbols");
	if (syscall(SYS_init_module, data, size, argc == 3 ? argv[2] : "")) {
		perror("init_module");
		free(data);
		return 1;
	}
	free(data);
	return 0;
}
