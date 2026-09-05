#include "hw/teleport-express/teleport_express_call.h"
#include "hw/teleport-express/express_device_common.h"
#include "hw/teleport-express/express_cluster_preflight.h"
#include "hw/teleport-express/express_log.h"

#include "hw/teleport-express/express_device_ctrl.h"

#if MAX_PARA_NUM != EXPRESS_CLUSTER_MAX_PARAMETERS
#error "cluster preflight and Teleport Express parameter limits disagree"
#endif

int create_call_from_cluster(uint64_t *send_buf, unsigned char *save_buf, Teleport_Express_Call *pre_call, Teleport_Express_Queue_Elem *pre_elem, Guest_Mem *pre_guest_mem, Scatter_Data *pre_scatter_data);
void release_call_none(Teleport_Express_Call *call, int notify);

void express_device_ctrl_invoke(Teleport_Express_Call *call)
{

    Call_Para all_para[MAX_PARA_NUM];
    express_printf("get ctrl invoke %llx\n", call->id);
    switch (call->id)
    {

    case FUNID_getExpressDeviceNum:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int temp_len = all_para[0].data_len;
        if (temp_len != 8)
        {
            printf("error len %d para_num %d FUNID_getExpressDeviceNum\n", temp_len, para_num);
            break;
        }
        uint64_t ret_data = (((uint64_t)kernel_load_express_driver_num) << 32) + (uint64_t)(strlen(kernel_load_express_driver_names) + 1);
        write_to_guest_mem(all_para[0].data, &ret_data, 0, 8);
    }
    break;

    case FUNID_getExpressDeviceNames:
    {
        if (kernel_load_express_driver_num == 0)
        {
            break;
        }
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int temp_len = all_para[0].data_len;
        int name_len = strlen(kernel_load_express_driver_names) + 1;
        if (temp_len < name_len)
        {
            printf("error len %d need len %d para_num %d FUNID_getExpressDeviceNames\n", temp_len, name_len, para_num);
            break;
        }
        write_to_guest_mem(all_para[0].data, (void *)kernel_load_express_driver_names, 0, name_len);
    }
    break;

    case FUNID_getExpressDeviceLogSettingInfo:
    {
        if (kernel_load_express_driver_num == 0)
        {
            break;
        }
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int temp_len = all_para[0].data_len;
        if (temp_len > sizeof(express_device_log_setting_info))
        {
            printf("error len %d need len %d para_num %d FUNID_getExpressDeviceLogSettingInfo\n", temp_len, (int)sizeof(express_device_log_setting_info), para_num);
            break;
        }
        write_to_guest_mem(all_para[0].data, (void *)&express_device_log_setting_info, 0, temp_len);
    }
    break;

    default:
    {
        printf("unknow funid %llx with device-id 0\n", call->id);
    }
    break;
    }

    call->callback(call, true);

    return;
}

/**

 *
 * @param context
 * @param call
 */
void cluster_decode_invoke(Teleport_Express_Call *call, void *context, EXPRESS_DECODE_FUN real_decode_fun)
{
    Call_Para all_para[MAX_PARA_NUM];

    unsigned char *send_async_buf;
    size_t send_async_buf_len;

    unsigned char *save_buf;

    // unsigned char temp_buf[1024];



    int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
    if (para_num != 2 && para_num != 3)
    {
        call->callback(call, 0);
        return;
    }

    size_t temp_len = 0;

    temp_len = all_para[0].data_len;
    send_async_buf_len = temp_len;

    if (!express_cluster_envelope_within_limits(all_para[0].data_len,
                                                all_para[1].data_len) ||
        (para_num == 3 &&
         !express_cluster_flime_envelope_within_limits(
             all_para[0].data_len, all_para[1].data_len)) ||
        temp_len % 8 != 0 ||
        !teleport_express_guest_mem_layout_exact(
            all_para[0].data, all_para[0].data_len) ||
        !teleport_express_guest_mem_layout_exact(
            all_para[1].data, all_para[1].data_len) ||
        (para_num == 3 &&
         (all_para[2].data_len != all_para[1].data_len ||
          !teleport_express_guest_mem_layout_exact(
              all_para[2].data, all_para[2].data_len))))
    {
        call->callback(call, 0);
        return;
    }

    send_async_buf = g_try_malloc(temp_len);
    if (send_async_buf == NULL)
    {
        call->callback(call, 0);
        return;
    }

    if (!teleport_express_guest_mem_read_checked(
            all_para[0].data, all_para[0].data_len, send_async_buf,
            all_para[0].data_len))
    {
        call->callback(call, 0);
        g_free(send_async_buf);
        return;
    }

    temp_len = all_para[1].data_len;

    save_buf = g_try_malloc(temp_len);
    if (save_buf == NULL)
    {
        call->callback(call, 0);
        g_free(send_async_buf);
        return;
    }

    if (!teleport_express_guest_mem_read_checked(
            all_para[1].data, all_para[1].data_len, save_buf,
            all_para[1].data_len))
    {
        call->callback(call, 0);
        g_free(send_async_buf);
        g_free(save_buf);
        return;
    }

    /*
     * Validate the complete call stream before dispatching any member.  This
     * prevents a malformed tail from committing an earlier Vulkan operation.
     */
    ExpressClusterPreflightStatus preflight_status;
    ExpressClusterPreflightInfo preflight_info;
    size_t error_offset = 0;
    if (!express_cluster_preflight_with_info(
            send_async_buf, send_async_buf_len, all_para[1].data_len,
            &preflight_info, &preflight_status, &error_offset) ||
        (preflight_info.has_flime_route &&
         (para_num != 3 || !FUN_NEED_SYNC(call->id))))
    {
        if (preflight_info.has_flime_route &&
            (para_num != 3 || !FUN_NEED_SYNC(call->id))) {
            LOGE("Host: clustered FLIME route requires synchronous outer call and third writable buffer");
        } else {
            LOGE("Host: cluster preflight rejected at offset %zu: %s",
                 error_offset,
                 express_cluster_preflight_status_string(preflight_status));
        }
        call->callback(call, 0);
        g_free(send_async_buf);
        g_free(save_buf);
        return;
    }

    Teleport_Express_Call unpack_call;
    unpack_call.vq = NULL;
    unpack_call.vdev = NULL;
    unpack_call.callback = release_call_none;
    unpack_call.is_end = 0;

    unpack_call.spend_time = 0;
    unpack_call.next = NULL;

    Teleport_Express_Queue_Elem pre_elem[MAX_PARA_NUM + 1];
    Guest_Mem pre_mem[MAX_PARA_NUM + 1];
    Scatter_Data pre_s_data[MAX_PARA_NUM + 1];


    size_t buf_loc = 0;
    int create_ret;
    while (buf_loc < send_async_buf_len)
    {
        memset(pre_elem, 0, sizeof(pre_elem));
        memset(pre_mem, 0, sizeof(pre_mem));
        memset(pre_s_data, 0, sizeof(pre_s_data));
        create_ret = create_call_from_cluster((uint64_t *)(send_async_buf + buf_loc), save_buf, &unpack_call, pre_elem, pre_mem, pre_s_data);
        if (create_ret == 0)
        {
            g_assert_not_reached();
        }

        unpack_call.thread_id = call->thread_id;
        unpack_call.process_id = call->process_id;
        unpack_call.unique_id = call->unique_id;

        buf_loc += ((size_t)unpack_call.para_num * 2 + 2) *
                   sizeof(uint64_t);

        real_decode_fun(context, &unpack_call);
    }
    /*
     * Inner writable parameters alias the private save_buf copy.  A
     * route-bearing three-parameter envelope supplies a separate writable
     * output image; copy the complete image there once every inner call has
     * completed.  The legacy two-parameter envelope remains input-only.
     * FLIME control is rejected by preflight because it retains storage.
     */
    if (para_num == 3 &&
        !teleport_express_guest_mem_write_checked(
            all_para[2].data, all_para[2].data_len, save_buf,
            all_para[2].data_len))
    {
        LOGE("Host: cluster save-buffer copyback was incomplete");
        call->callback(call, 0);
        g_free(send_async_buf);
        g_free(save_buf);
        return;
    }
    if (para_num == 3) {
        call->elem_tail->written_len = all_para[2].data_len;
    }

    call->callback(call, 1);

    g_free(send_async_buf);
    g_free(save_buf);
    return;
}

/**

 *


 * @return
 */
int create_call_from_cluster(uint64_t *send_buf, unsigned char *save_buf, Teleport_Express_Call *pre_call, Teleport_Express_Queue_Elem *pre_elem, Guest_Mem *pre_guest_mem, Scatter_Data *pre_scatter_data)
{

    pre_call->id = send_buf[0];


    if (GET_FUN_ID(pre_call->id) == 9999)
    {
        return 0;
    }

    pre_call->para_num = send_buf[1];
    pre_call->elem_header = NULL;
    // assert(pre_call->para_num < 10);

    // Teleport_Express_Queue_Elem *elem = g_malloc(sizeof(Teleport_Express_Queue_Elem));
    pre_call->elem_header = &(pre_elem[0]);
    Teleport_Express_Queue_Elem *last_elem = &(pre_elem[0]);
    for (int i = 0; i < pre_call->para_num; i++)
    {


        // Guest_Mem *guest_mem = g_malloc(sizeof(Guest_Mem));
        // Scatter_Data *scatter_data = g_malloc(sizeof(Scatter_Data));

        if (send_buf[i * 2 + 2 + 1] != 0)
        {
            pre_scatter_data[i].len = send_buf[i * 2 + 2];
            pre_scatter_data[i].data = save_buf + send_buf[i * 2 + 2 + 1];
        }
        else
        {
            pre_scatter_data[i].len = 0;
            pre_scatter_data[i].data = NULL;
        }

        pre_guest_mem[i].scatter_data = &(pre_scatter_data[i]);
        pre_guest_mem[i].num = 1;
        pre_guest_mem[i].all_len = pre_scatter_data[i].len;

        pre_elem[i + 1].para = &(pre_guest_mem[i]);
        pre_elem[i + 1].len = send_buf[i * 2 + 2];
        pre_elem[i + 1].next = NULL;

        last_elem->next = &(pre_elem[i + 1]);
        last_elem = &(pre_elem[i + 1]);
    }
    pre_call->elem_tail = &(pre_elem[pre_call->para_num]);




    return 1;
}

/**

 *
 * @param call
 * @param notify
 */
void release_call_none(Teleport_Express_Call *call, int notify)
{
    return;
}
