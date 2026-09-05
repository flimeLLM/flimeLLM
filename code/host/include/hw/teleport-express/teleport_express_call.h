#ifndef QEMU_TELEPORT_EXPRESS_CALL_H
#define QEMU_TELEPORT_EXPRESS_CALL_H
#include "hw/teleport-express/teleport_express.h"
#include "hw/teleport-express/express_device_common.h"



#define DIRECT_PARA 1
#define COPY_PARA 2
#define RET_PARA 4


#define MAX_PARA_NUM 32

/* VirtQueue itself cannot legally expose more descriptors than this. */
#define TELEPORT_EXPRESS_MAX_SCATTER_SEGMENTS 1024u


/**

 * 
 */
#define TELEPORT_EXPRESS_QUEUE_ELEMS_FREE(header_ptr)               \
    for (Teleport_Express_Queue_Elem *a = (header_ptr); a != NULL;) \
    {                                                             \
        Teleport_Express_Queue_Elem *b = a;                         \
        a = a->next;                                              \
        if (b->para != NULL)                                      \
        {                                                         \
            release_one_guest_mem(b->para);                       \
        }                                                         \
        g_free(b);                                                \
    }

/**

 * 
 */
#define VIRTIO_ELEM_PUSH_ALL(vq, elem, header_ptr, num, next)    \
    for (elem *a = (elem *)(header_ptr); a != NULL; a = a->next) \
    {                                                            \
        virtqueue_push(vq, (VirtQueueElement *)a, num);          \
    }




// #define GET_DEVICE_ID(id) ((id) >> 32)
// #define GET_FUN_ID(id) ((id)&0xffffff)
// #define FUN_NEED_SYNC(id) (((id) >> 24) & 0x1)
// #define FUN_HAS_HOST_SYNC(id) (((id) >> 24) & 0x2)





typedef struct Teleport_Express_Flag_Buf
{

    volatile uint64_t flag;

    volatile int64_t mem_spend_time;

    volatile uint64_t ret_data;


    uint64_t id;


    uint64_t para_num;

    uint64_t thread_id;

    uint64_t process_id;

    uint64_t unique_id;

    // uint64_t  num_free;


    // volatile uint64_t ret;



} Teleport_Express_Flag_Buf;




Teleport_Express_Call *alloc_one_call(void);
void release_one_call(Teleport_Express_Call *call, bool notify);
Guest_Mem *alloc_one_guest_mem(void);
void release_one_guest_mem(Guest_Mem *mem);

/**
 * Get a pointer to the guest memory region para is pointing to.
 * If need_free is non-zero, the caller is responsible for freeing the memory (using g_free).
*/
void *call_para_to_ptr(Call_Para para, int *need_free);
void *get_direct_ptr(Guest_Mem *guest_mem, int *flag);
void read_from_guest_mem(Guest_Mem *guest, void *host, size_t start_loc, size_t length);
void write_to_guest_mem(Guest_Mem *guest, void *host, size_t start_loc, size_t length);
void host_guest_buffer_exchange(Scatter_Data *guest_data, unsigned char *host_data, size_t start_loc, size_t length, int is_guest_to_host);

/*
 * Checked helpers for protocol trust boundaries.  They first validate that
 * the entire Guest_Mem layout exactly matches the transport-declared byte
 * count, then copy an exact prefix (or fail without doing a partial copy).
 */
bool teleport_express_guest_mem_layout_exact(const Guest_Mem *guest,
                                             size_t declared_bytes);
bool teleport_express_guest_mem_read_checked(const Guest_Mem *guest,
                                             size_t declared_bytes,
                                             void *host,
                                             size_t copy_bytes);
bool teleport_express_guest_mem_write_checked(const Guest_Mem *guest,
                                              size_t declared_bytes,
                                              const void *host,
                                              size_t copy_bytes);


int fill_teleport_express_queue_elem(Teleport_Express_Queue_Elem *elem, unsigned long long *id, unsigned long long *thread_id, unsigned long long *process_id, unsigned long long *unique_id, unsigned long long *num);
Teleport_Express_Call *pack_call_from_queue(VirtQueue *vq, int index);



int get_para_from_call(Teleport_Express_Call *call, Call_Para *call_para, unsigned long para_num);


Guest_Mem *copy_guest_mem_from_call(Teleport_Express_Call *call, int index);

void free_copied_guest_mem(Guest_Mem *mem);


void guest_null_ptr_init(VirtQueue *vq);

void common_call_callback(Teleport_Express_Call *call);

bool call_is_interrupt(Teleport_Express_Call *call);

#endif
