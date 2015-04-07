#ifndef __AUTOMEM_H
#define __AUTOMEM_H

#ifdef __cplusplus
extern "C" {
#endif

struct automem_s
{
	unsigned int size;
	unsigned int buffersize;
	unsigned char* pdata;
};

typedef struct automem_s automem_t;


void automem_init(automem_t* pmem, unsigned int initBufferSize);
void automem_uninit(automem_t* pmem);
void automem_clean(automem_t* pmen, int newsize);

void automem_ensure_newspace(automem_t* pmem, unsigned int len);
void automem_attach(automem_t* pmem, void* pdata, unsigned int len);
void* automem_detach(automem_t* pmem, unsigned int* plen);
int automem_append_voidp(automem_t* pmem, const void* p, unsigned int len);
void automem_append_int(automem_t* pmem, int n);

void automem_append_char(automem_t* pmem, char c);
void automem_append_pchar(automem_t* pmem, char* n);
void automem_append_byte(automem_t* pmem, unsigned char c);
void automem_init_by_ptr(automem_t* pmem, void* pdata, unsigned int len);


void * automem_alloc(automem_t * pmem, int size);

void automem_reset(automem_t* pmem);

int automem_erase(automem_t* pmem, unsigned int size);
int automem_erase_ex(automem_t* pmem, unsigned int size,unsigned int limit);


#ifdef __cplusplus
}
#endif

#endif
