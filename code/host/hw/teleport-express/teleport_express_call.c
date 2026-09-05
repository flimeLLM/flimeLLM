/**
 * @file teleport_express_call.c
 * @brief
 * @version 0.1
 * @date 2022-11-06
 *
 * @copyright Copyright (c) 2022
 *
 */
#include "hw/teleport-express/express_log.h"
#include "hw/teleport-express/express_device_common.h"

#include "hw/teleport-express/teleport_express_call.h"
#include "exec/cpu-common.h"

#define TELEPORT_EXPRESS_FLIME_CONTROL_FUN_ID UINT32_C(1906)
#define TELEPORT_EXPRESS_FLIME_ROUTE_FUN_ID UINT32_C(1907)
#define TELEPORT_EXPRESS_CLUSTER_FUN_ID UINT32_C(9999)


// static Teleport_Express_Call pre_alloc_call[CALL_BUF_SIZE * 2];
// static bool pre_alloc_call_flag[CALL_BUF_SIZE * 2];

// static volatile int pre_alloc_call_loc = 0;

// static Guest_Mem pre_guest_mem[CALL_BUF_SIZE * 2 * MAX_PARA_NUM];
// static bool pre_guest_mem_flag[CALL_BUF_SIZE * 2 * MAX_PARA_NUM];

// static volatile int pre_guest_mem_loc = 0;

#define INIT_CACHE_SIZE 128

typedef struct Fast_Alloc_Date_Cache
{
    char **data;
    bool **flag;
    bool **need_release_flag;
    int alloc_loc;
    int remain_num;
    int num;
    int per_size;
    volatile int atomic_locker;
} Fast_Alloc_Date_Cache;

static Teleport_Express_Call *volatile packaging_call[2] = {NULL, NULL};
static int remain_elem_num[2] = {0, 0};

static void *guest_null_ptr = NULL;

static Fast_Alloc_Date_Cache *call_cache = NULL;
static Fast_Alloc_Date_Cache *guest_mem_cache = NULL;

static Fast_Alloc_Date_Cache *pre_alloc_cache_init(int per_size)
{
    Fast_Alloc_Date_Cache *cache = g_malloc0(sizeof(Fast_Alloc_Date_Cache));

    cache->data = g_malloc0(sizeof(char *) * INIT_CACHE_SIZE);

    char *real_data = g_malloc0((per_size + sizeof(int)) * INIT_CACHE_SIZE);
    for (int i = 0; i < INIT_CACHE_SIZE; i++)
    {
        cache->data[i] = real_data + (per_size + sizeof(int)) * i;
    }

    cache->flag = g_malloc0(sizeof(bool *) * INIT_CACHE_SIZE);

    bool *real_flag = g_malloc0(sizeof(bool) * INIT_CACHE_SIZE);
    for (int i = 0; i < INIT_CACHE_SIZE; i++)
    {
        cache->flag[i] = real_flag + sizeof(bool) * i;
    }

    cache->need_release_flag = NULL;
    cache->per_size = per_size;
    cache->num = INIT_CACHE_SIZE;
    cache->remain_num = INIT_CACHE_SIZE;
    cache->alloc_loc = 0;
    return cache;
}

static void *get_one_cache(Fast_Alloc_Date_Cache *cache)
{
    // return g_malloc0(cache->per_size);
    if (unlikely(cache->remain_num == 0))
    {
        while (qatomic_cmpxchg(&(cache->atomic_locker), 0, 1) != 0)
            ;
        if (cache->remain_num == 0)
        {

            char **temp_data = g_malloc0(sizeof(char *) * cache->num * 2);
            memcpy(temp_data, cache->data, sizeof(char *) * cache->num);

            char *real_data = g_malloc0((cache->per_size + sizeof(int)) * cache->num);
            for (int i = cache->num; i < cache->num * 2; i++)
            {
                temp_data[i] = real_data + (cache->per_size + sizeof(int)) * (i - cache->num);
            }

            bool **temp_flag = g_malloc0(sizeof(bool *) * cache->num * 2);
            memcpy(temp_flag, cache->flag, sizeof(bool *) * cache->num);

            bool *real_flag = g_malloc0(sizeof(bool) * cache->num);
            for (int i = cache->num; i < cache->num * 2; i++)
            {
                temp_flag[i] = real_flag + sizeof(bool) * (i - cache->num);
            }

            LOGD("cache size bigger %d", cache->num);
            g_free(cache->data);
            cache->data = temp_data;


            if (cache->need_release_flag != NULL)
            {
                g_free(cache->need_release_flag);
                cache->need_release_flag = cache->flag;
            }
            cache->flag = temp_flag;

            qatomic_add(&(cache->remain_num), cache->num);
            cache->num *= 2;
        }
        qatomic_set(&(cache->atomic_locker), 0);
    }

    int now_loc = cache->alloc_loc;

    while (qatomic_cmpxchg(cache->flag[now_loc], false, true) == true)
    {
        now_loc = (now_loc + 1) % (cache->num);
    }

    int *ret_ptr = (int *)cache->data[now_loc];
    ret_ptr[0] = now_loc;
    qatomic_set(&(cache->alloc_loc), now_loc);

    qatomic_add(&(cache->remain_num), -1);

    return (void *)(ret_ptr + 1);
}

static void release_one_cache(Fast_Alloc_Date_Cache *cache, void *data)
{
    // g_free(data);
    // return;
    int *i_data = (int *)data;
    int index = *(i_data - 1);

    memset(i_data, 0, cache->per_size);

    qatomic_set(cache->flag[index], false);

    qatomic_add(&(cache->remain_num), 1);
}

Teleport_Express_Call *alloc_one_call(void)
{

    if (unlikely(call_cache == NULL))
    {
        call_cache = pre_alloc_cache_init(sizeof(Teleport_Express_Call));
    }
    return (Teleport_Express_Call *)get_one_cache(call_cache);
}

void release_one_call(Teleport_Express_Call *call, bool notify)
{
    Teleport_Express_Queue_Elem *elem;

    LOGD("going to release call of id %lld", call->unique_id);
    VirtQueue *vq = call->vq;
    for (elem = call->elem_header; elem != NULL; elem = elem->next) {
        g_assert(elem->written_len <= INT_MAX);
        virtqueue_push(vq, &elem->elem, elem->written_len);
    }
    TELEPORT_EXPRESS_QUEUE_ELEMS_FREE(call->elem_header);
    if (notify)
    {
        virtio_notify(VIRTIO_DEVICE(call->vdev), call->vq);
    }

    release_one_cache(call_cache, call);
}

Guest_Mem *alloc_one_guest_mem(void)
{
    if (unlikely(guest_mem_cache == NULL))
    {
        guest_mem_cache = pre_alloc_cache_init(sizeof(Guest_Mem));
    }
    return (Guest_Mem *)get_one_cache(guest_mem_cache);
}

void release_one_guest_mem(Guest_Mem *mem)
{
    release_one_cache(guest_mem_cache, mem);
}

/**

 *
 * @param guest_mem
 * @param flag
 * @return void*
 */
void *get_direct_ptr(Guest_Mem *guest_mem, int *flag)
{
    if (likely(guest_mem->num == 1))
    {
        Scatter_Data *guest_data = guest_mem->scatter_data;
        *flag = 1;

        return guest_data->data;
    }
    *flag = 0;
    return NULL;
}

/**

 *




 */
void read_from_guest_mem(Guest_Mem *guest, void *host, size_t start_loc, size_t length)
{
    if (unlikely(guest == NULL || guest->all_len == 0 || length == 0))
    {
        return;
    }
    express_printf("read_from_guest_mem length %llu all_len %d\n", length, guest->all_len);
    Scatter_Data *guest_data = guest->scatter_data;
    if (unlikely(host == NULL || length > guest->all_len))
    {
        LOGE("read_from_guest_mem error host %p len %d %lld %d", host, guest->all_len, length, (host==NULL));
        return;
    }

    host_guest_buffer_exchange(guest_data, (unsigned char *)host, start_loc, length, 1);
}

/**

 *




 */
void write_to_guest_mem(Guest_Mem *guest, void *host, size_t start_loc, size_t length)
{
    if (unlikely(guest == NULL))
    {
        LOGE("write_to_guest_mem guest_mem is null!");
        return;
    }
    express_printf("write_to_guest_mem start_loc %llu length %llu all_len %d\n", start_loc, length, guest->all_len);

    Scatter_Data *guest_data = guest->scatter_data;
    if (unlikely(length == 0 || host == NULL || length > guest->all_len))
    {
        LOGE("write_to_guest_mem error host %llx len %d %lld", (uint64_t)host, guest->all_len, length);
        return;
    }
    host_guest_buffer_exchange(guest_data, (unsigned char *)host, start_loc, length, 0);
}

/**

 *




 * @param is_guest_to_host
 */
void host_guest_buffer_exchange(Scatter_Data *guest_data, unsigned char *host_data, size_t start_loc, size_t length, int is_guest_to_host)
{

    // int walk_loc = 0;
    if (unlikely(guest_data == NULL || host_data == NULL))
    {
        return;
    }

    size_t remain_len = length;
    // express_printf("memcpy data len %llu,%d\n",length,remain_len);
    int guest_loc = start_loc;
    int host_loc = 0;
    // int cpy_len = 0;
    int guest_index = 0;
    unsigned char *last_data = NULL;
    while (remain_len > 0 && remain_len < 100000000000)
    {
        if (unlikely(guest_data[guest_index].len == 0 || guest_data[guest_index].data == NULL))
        {
            break;
        }
        if (guest_data[guest_index].len > guest_loc)
        {

            if (remain_len < guest_data[guest_index].len - guest_loc)
            {
                if (is_guest_to_host)
                {
                    memcpy(host_data + host_loc, guest_data[guest_index].data + guest_loc, remain_len);
                }
                else
                {

                    express_printf("memcpy data %lx index %d loc %d host %lx loc %d remain %llu\n", guest_data[guest_index].data, guest_index, guest_loc, host_data, host_loc, remain_len);
                    memcpy(guest_data[guest_index].data + guest_loc, host_data + host_loc, remain_len);
                }
                break;
            }
            else
            {
                if (is_guest_to_host)
                {
                    memcpy(host_data + host_loc, guest_data[guest_index].data + guest_loc, guest_data[guest_index].len - guest_loc);
                }
                else
                {

                    express_printf("memcpy data %lx index %d loc %d len %llu, host %lx loc %d remain %llu\n", guest_data[guest_index].data, guest_index, guest_loc, guest_data[guest_index].len, host_data, host_loc, remain_len);

                    if (last_data != guest_data[guest_index].data)
                    {
                        last_data = guest_data[guest_index].data;
                    }
                    else
                    {
                        LOGE("error map data! same scatter data pointer");
                    }

                    memcpy(guest_data[guest_index].data + guest_loc, host_data + host_loc, guest_data[guest_index].len - guest_loc);
                }
                host_loc += guest_data[guest_index].len - guest_loc;
                remain_len -= guest_data[guest_index].len - guest_loc;
            }

            guest_loc = 0;
        }
        else
        {
            guest_loc -= guest_data[guest_index].len;
        }
        guest_index++;
    }
}

bool teleport_express_guest_mem_layout_exact(const Guest_Mem *guest,
                                             size_t declared_bytes)
{
    size_t total = 0;
    int i;

    if (guest == NULL || declared_bytes == 0 || declared_bytes > INT_MAX ||
        guest->all_len < 0 || (size_t)guest->all_len != declared_bytes ||
        guest->num <= 0 ||
        (unsigned int)guest->num >
            TELEPORT_EXPRESS_MAX_SCATTER_SEGMENTS ||
        guest->scatter_data == NULL) {
        return false;
    }
    for (i = 0; i < guest->num; i++) {
        const Scatter_Data *segment = &guest->scatter_data[i];

        if (segment->data == NULL || segment->len == 0 ||
            segment->len > declared_bytes - total) {
            return false;
        }
        total += segment->len;
    }
    return total == declared_bytes;
}

static bool teleport_express_guest_mem_copy_checked(
    const Guest_Mem *guest, size_t declared_bytes, void *host,
    size_t copy_bytes, bool guest_to_host)
{
    size_t copied = 0;
    int i;

    if (!teleport_express_guest_mem_layout_exact(guest, declared_bytes) ||
        copy_bytes > declared_bytes || (copy_bytes != 0 && host == NULL)) {
        return false;
    }
    for (i = 0; i < guest->num && copied < copy_bytes; i++) {
        size_t amount = MIN(guest->scatter_data[i].len,
                            copy_bytes - copied);

        if (guest_to_host) {
            memcpy((uint8_t *)host + copied,
                   guest->scatter_data[i].data, amount);
        } else {
            memcpy(guest->scatter_data[i].data,
                   (const uint8_t *)host + copied, amount);
        }
        copied += amount;
    }
    return copied == copy_bytes;
}

bool teleport_express_guest_mem_read_checked(const Guest_Mem *guest,
                                             size_t declared_bytes,
                                             void *host,
                                             size_t copy_bytes)
{
    return teleport_express_guest_mem_copy_checked(
        guest, declared_bytes, host, copy_bytes, true);
}

bool teleport_express_guest_mem_write_checked(const Guest_Mem *guest,
                                              size_t declared_bytes,
                                              const void *host,
                                              size_t copy_bytes)
{
    return teleport_express_guest_mem_copy_checked(
        guest, declared_bytes, (void *)host, copy_bytes, false);
}

static unsigned int teleport_express_next_parameter_index(
    const Teleport_Express_Call *call)
{
    const Teleport_Express_Queue_Elem *elem;
    unsigned int index = 0;

    if (call == NULL || call->elem_header == NULL) {
        return 0;
    }
    for (elem = call->elem_header->next; elem != NULL; elem = elem->next) {
        index++;
    }
    return index;
}

static bool teleport_express_parameter_direction_valid(
    const Teleport_Express_Call *call,
    const Teleport_Express_Queue_Elem *elem)
{
    uint32_t function_id;
    unsigned int parameter_index;
    bool writable_parameter;

    if (call == NULL || elem == NULL) {
        return false;
    }
    function_id = GET_FUN_ID(call->id);
    parameter_index = teleport_express_next_parameter_index(call);
    writable_parameter = parameter_index == 1 &&
        (function_id == TELEPORT_EXPRESS_FLIME_CONTROL_FUN_ID ||
         function_id == TELEPORT_EXPRESS_FLIME_ROUTE_FUN_ID);
    writable_parameter |=
        parameter_index == 2 &&
        function_id == TELEPORT_EXPRESS_CLUSTER_FUN_ID;

    if (writable_parameter) {
        return elem->elem.in_num != 0 && elem->elem.out_num == 0;
    }
    return elem->elem.in_num == 0 && elem->elem.out_num != 0;
}

static void teleport_express_recycle_unlinked_element(
    VirtQueue *vq, Teleport_Express_Queue_Elem *elem)
{
    if (elem == NULL) {
        return;
    }
    virtqueue_push(vq, &elem->elem, 0);
    if (elem->para != NULL) {
        release_one_guest_mem(elem->para);
    }
    g_free(elem);
}

/**


 *







 */
int fill_teleport_express_queue_elem(Teleport_Express_Queue_Elem *elem, unsigned long long *id, unsigned long long *thread_id, unsigned long long *process_id, unsigned long long *unique_id, unsigned long long *num)
{
    VirtQueueElement *v_elem = &elem->elem;
    // printf("fill elem num %u %u\n",v_elem->out_num,v_elem->in_num);
    if ((v_elem->out_num != 0 && v_elem->in_num != 0) || (v_elem->out_num == 0 && v_elem->in_num == 0))
    {
        return 0;
    }
    elem->para = NULL;
    elem->written_len = 0;
    elem->next = NULL;

    Guest_Mem *guest_mem = alloc_one_guest_mem();

    if (unlikely(guest_mem == NULL))
    {
        LOGE("error! guest_mem alloc return NULL!");
        return 0;
    }

    if (v_elem->out_num != 0)
    {
        guest_mem->scatter_data = (Scatter_Data *)v_elem->out_sg;
        guest_mem->num = v_elem->out_num;
    }

    if (v_elem->in_num != 0)
    {
        guest_mem->scatter_data = (Scatter_Data *)v_elem->in_sg;
        guest_mem->num = v_elem->in_num;
    }

    size_t buf_len = 0;
    for (int i = 0; i < guest_mem->num; i++)
    {
        if (guest_mem->scatter_data[i].len == 4 && guest_mem->scatter_data[i].data == guest_null_ptr && v_elem->out_num == 1 && v_elem->in_num == 0)
        {
            LOGI("find null ptr!!!");
            teleport_express_use_logical_null(elem, guest_mem);
        }
        if (guest_mem->scatter_data[i].len > INT_MAX - buf_len) {
            release_one_guest_mem(guest_mem);
            return 0;
        }
        buf_len += guest_mem->scatter_data[i].len;

        // express_printf("guest_mem %d i %d len %d now %d\n",num,i, guest_mem->scatter_data[i].len, buf_len);
    }

    guest_mem->all_len = (int)buf_len;
    elem->len = buf_len;

    elem->para = guest_mem;

    if (id != NULL && num != NULL && thread_id != NULL && process_id != NULL && unique_id != NULL)
    {


        if (unlikely(v_elem->in_num != 1 || v_elem->out_num != 0))
        {
            return 0;
        }

        // Teleport_Express_Flag_Buf *flag_buf = (Teleport_Express_Flag_Buf *)guest_mem->scatter_data->data;

        Teleport_Express_Flag_Buf flag_buf;

        if (buf_len < sizeof(flag_buf) ||
            !teleport_express_guest_mem_read_checked(
                guest_mem, buf_len, &flag_buf, sizeof(flag_buf))) {
            LOGE("error! malformed flag_buf");
            return 0;
        }
        *id = flag_buf.id;
        *process_id = flag_buf.process_id;
        *thread_id = flag_buf.thread_id;
        *num = flag_buf.para_num;
        *unique_id = flag_buf.unique_id;
    }
    return 1;
}

/**

 *
 * @param vq

 */
Teleport_Express_Call *pack_call_from_queue(VirtQueue *vq, int index)
{

    // static int pack_cnt = 0;
    Teleport_Express_Queue_Elem *elem;

    Teleport_Express_Call *call;

    unsigned long long para_num;
    unsigned long long fun_id;
    unsigned long long thread_id;
    unsigned long long process_id;
    unsigned long long unique_id;

    elem = virtqueue_pop(vq, sizeof(Teleport_Express_Queue_Elem));
    if (elem != NULL) {
        elem->para = NULL;
        elem->written_len = 0;
        elem->next = NULL;
    }
    while (elem)
    {

        if (packaging_call[index] != NULL)
        {
            call = packaging_call[index];
            packaging_call[index] = NULL;
            para_num = remain_elem_num[index] - 1;
            remain_elem_num[index] = 0;
            // printf("continue null elem reamin %d\n", para_num + 1);

            if (unlikely(!teleport_express_parameter_direction_valid(call, elem) || fill_teleport_express_queue_elem(elem, NULL, NULL, NULL, NULL, NULL) == 0))
            {


                // VIRTIO_ELEM_PUSH_ALL(vq, Teleport_Express_Queue_Elem, call->elem_header, 1, next);
                // TELEPORT_EXPRESS_QUEUE_ELEMS_FREE(call->elem_header);
                release_one_call(call, false);
                call = NULL;
                LOGW("fill para error first elem %u,%u remain_elem_num %llu index %d", elem->elem.in_num, elem->elem.out_num, para_num, index);
                teleport_express_recycle_unlinked_element(vq, elem);
                break;
            }
            call->elem_tail->next = elem;
            call->elem_tail = elem;
        }
        else
        {
            if (unlikely(fill_teleport_express_queue_elem(elem, &fun_id, &thread_id, &process_id, &unique_id, &para_num) == 0))
            {

                LOGE("fill error %u %u", elem->elem.in_num, elem->elem.out_num);
                teleport_express_recycle_unlinked_element(vq, elem);
                return NULL;
            }

            call = alloc_one_call();
            if (call == NULL)
            {
                LOGE("error! alloc call return NULL!");
            }
            call->elem_header = elem;
            call->elem_tail = elem;
            call->vq = vq;

            call->para_num = para_num;
            call->id = fun_id;
            call->thread_id = thread_id;
            call->process_id = process_id;
            call->unique_id = unique_id;
            call->spend_time = 0;
            call->next = NULL;
        }
        // gint64 start_time=g_get_real_time();


        for (int i = 0; i < para_num; i++)
        {




            int cnt_timeout = 0;

            elem = virtqueue_pop(vq, sizeof(Teleport_Express_Queue_Elem));
            if (elem != NULL) {
                elem->para = NULL;
                elem->written_len = 0;
                elem->next = NULL;
            }

            if (unlikely(elem == NULL))
            {
                packaging_call[index] = call;
                remain_elem_num[index] = para_num - i;
                // printf("find null elem\n");
                return NULL;
            }

            // while (elem == NULL)
            // {
            //     // t_int = g_get_real_time();
            //     // start_time = g_get_real_time();

            //     elem = virtqueue_pop(vq, sizeof(Teleport_Express_Queue_Elem));
            //     cnt_timeout++;
            //     if(teleport_express_should_stop)
            //     {
            //         return NULL;
            //     }
            // }

            if (unlikely(elem == NULL || !teleport_express_parameter_direction_valid(call, elem) || fill_teleport_express_queue_elem(elem, NULL, NULL, NULL, NULL, NULL) == 0))
            {


                // VIRTIO_ELEM_PUSH_ALL(vq, Teleport_Express_Queue_Elem, call->elem_header, 1, next);
                // TELEPORT_EXPRESS_QUEUE_ELEMS_FREE(call->elem_header);
                release_one_call(call, false);
                call = NULL;
                if (elem == NULL)
                {
                    LOGW("fill para error NULL %d index %d", cnt_timeout, index);
                }
                else
                {
                    LOGW("fill para error %u,%u index %d", elem->elem.in_num, elem->elem.out_num, index);
                }
                teleport_express_recycle_unlinked_element(vq, elem);
                break;
            }
            call->elem_tail->next = elem;
            call->elem_tail = elem;
        }


        if (call == NULL)
        {
            continue;
        }


        return call;
    }

    return NULL;
}

/**

 *



 * @return int
 */
int get_para_from_call(Teleport_Express_Call *call, Call_Para *call_para, unsigned long max_para_num)
{
    // LOGD("in get para from call %lld", call->unique_id);
    Teleport_Express_Queue_Elem *header = call->elem_header;
    if (max_para_num < call->para_num || unlikely(header == NULL))
    {
        return 0;
    }

    Teleport_Express_Queue_Elem *now_elem = header->next;
    call->spend_time = g_get_real_time();

    for (int i = 0; i < call->para_num; i++)
    {
        if (unlikely(now_elem == NULL))
        {
            return 0;
        }
        // Guest_Mem *mem=now_elem->para;
        call_para[i].data = now_elem->para;

        call_para[i].data_len = now_elem->len;
        now_elem = now_elem->next;
    }
    // LOGI("get params %d %lld", call->para_num, call->unique_id);
    return call->para_num;
}

Guest_Mem *copy_guest_mem_from_call(Teleport_Express_Call *call, int index)
{
    Call_Para para[MAX_PARA_NUM];
    int para_num = get_para_from_call(call, para, MAX_PARA_NUM);
    if (para_num >= index)
    {
        Guest_Mem *save_mem = g_malloc(sizeof(Guest_Mem));
        Guest_Mem *old_mem = para[index - 1].data;

        // void* real_guest_mem = (void *)qemu_ram_addr_from_host((void*)old_mem->scatter_data->data);

        // hwaddr len = old_mem->all_len;
        // hwaddr xlat;

        // MemoryRegion *mr = address_space_translate(&address_space_memory,
        //     (hwaddr)real_guest_mem,
        //     &xlat, &len, false,
        //     MEMTXATTRS_UNSPECIFIED);

        // // void *hva = cpu_physical_memory_map((hwaddr)real_guest_mem, &len, false);
        // void *hva;
        // if (mr) {
        //     hva = qemu_map_ram_ptr(mr->ram_block, xlat);
        //     LOGI("GPA 0x%lx corresponds to HVA %p", real_guest_mem, hva);
        // } 

        // LOGI("get old mem %d %d %lld real mem %lld hva %lld", old_mem->num, old_mem->all_len, (uint64_t)old_mem->scatter_data->data, (uint64_t)real_guest_mem, (uint64_t)hva);

        save_mem->num = old_mem->num;
        save_mem->all_len = old_mem->all_len;
        save_mem->scatter_data = g_malloc(save_mem->num * sizeof(Scatter_Data));
        memcpy(save_mem->scatter_data, old_mem->scatter_data, save_mem->num * sizeof(Scatter_Data));

        return save_mem;
    }
    LOGE("cpoy_guest_mem_from_call supplied para %d greater than total paras %d!", index, para_num);
    return NULL;
}

void free_copied_guest_mem(Guest_Mem *mem)
{
    if(mem!=NULL){
        if(mem->scatter_data!=NULL){
            g_free(mem->scatter_data);
        }
        g_free(mem);
    }
}

void guest_null_ptr_init(VirtQueue *vq)
{
    VirtQueueElement *elem;

    express_printf("wait for pop\n");
    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    while (elem == NULL)
    {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        express_printf("error elem is NULL\n");
    }
    // LOGI("get first one ptr %llu %llu %llu", elem->out_sg->iov_len, elem->out_num, elem->in_num);

    if (elem->out_sg->iov_len == 4 && elem->out_num == 1 && elem->in_num == 0)
    {
        guest_null_ptr = elem->out_sg->iov_base;

        LOGD("null ptr %llu", (uint64_t)guest_null_ptr);


        char *temp1 = g_malloc(1024 * 1024 * 24);
        char *temp2 = g_malloc(1024 * 1024 * 24);
        memset(temp1, 0, 1024 * 1024 * 24);
        // memset(temp2,1,1024*1024*24);
        gint64 t_start = g_get_real_time();
        memcpy(temp1, temp2, 1024 * 1024 * 24);
        uint32_t t_spend = (uint32_t)(g_get_real_time() - t_start);
        uint32_t mem_speed = 1024 * 1024 * 24 / t_spend;

        express_printf("mem cpy speed %u\n", mem_speed);

        *(uint32_t *)guest_null_ptr = mem_speed;

        g_free(temp1);
        g_free(temp2);
    }
    else
    {
        LOGE("error! null ptr cannot be init!");
    }
    virtqueue_push(vq, elem, 1);
}

void common_call_callback(Teleport_Express_Call *call)
{
    if (call->spend_time != 0)
    {
        call->spend_time = g_get_real_time() - call->spend_time;
    }


    Guest_Mem *mem = call->elem_header->para;

    LOGD("in release commom call mem %lld", (uint64_t)mem);

    unsigned long long t_flag = 1;
    write_to_guest_mem(mem, &t_flag, __builtin_offsetof(Teleport_Express_Flag_Buf, flag), 8);
    write_to_guest_mem(mem, &(call->spend_time), __builtin_offsetof(Teleport_Express_Flag_Buf, mem_spend_time), 8);
    call->elem_header->written_len =
        MAX(call->elem_header->written_len,
            __builtin_offsetof(Teleport_Express_Flag_Buf, mem_spend_time) +
                sizeof(call->spend_time));

    // read_from_guest_mem(mem, &t_flag, __builtin_offsetof(Teleport_Express_Flag_Buf, id), 8);
    // printf("write flag id %llu %llu\n", t_flag, call->thread_id);
}


bool call_is_interrupt(Teleport_Express_Call *call)
{


    Guest_Mem *mem = call->elem_header->para;

    unsigned long long t_flag = 0;
    read_from_guest_mem(mem, &t_flag, __builtin_offsetof(Teleport_Express_Flag_Buf, flag), 8);

    return t_flag == 2;
    // read_from_guest_mem(mem, &t_flag, __builtin_offsetof(Teleport_Express_Flag_Buf, id), 8);
    // printf("write flag id %llu %llu\n", t_flag, call->thread_id);
}

void *call_para_to_ptr(Call_Para para, int *need_free) {
    size_t ptr_len = 0;
    unsigned char *ptr = NULL;

    ptr_len = para.data_len;

    int null_flag = 0;
    ptr = get_direct_ptr(para.data, &null_flag);
    if (unlikely(ptr == NULL)) {
        if (ptr_len != 0 && null_flag == 0) {
            ptr = g_malloc(ptr_len);
            *need_free = 1;
            read_from_guest_mem(para.data, ptr, 0, para.data_len);
        }
    }

    return ptr;
}
