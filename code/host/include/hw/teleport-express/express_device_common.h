#ifndef EXPRESS_DEVICE_COMMON_H
#define EXPRESS_DEVICE_COMMON_H

#include "hw/teleport-express/teleport_express.h"
#include "hw/teleport-express/express_log.h"

#define EXPRESS_CTRL_DEVICE_ID ((uint64_t)0)
#define EXPRESS_GPU_DEVICE_ID ((uint64_t)1)
#define EXPRESS_LOG_DEVICE_ID ((uint64_t)2)

#define EXPRESS_TOUCHSCREEN_DEVICE_ID ((uint64_t)3)
#define EXPRESS_KEYBOARD_DEVICE_ID ((uint64_t)4)
#define EXPRESS_BATTERY_DEVICE_ID ((uint64_t)5)
#define EXPRESS_ACCELEROMTETER_DEVICE_ID ((uint64_t)6)
#define EXPRESS_GYROSCOPE_DEVICE_ID ((uint64_t)7)
#define EXPRESS_GPS_DEVICE_ID ((uint64_t)8)
#define EXPRESS_MICROPHONE_DEVICE_ID ((uint64_t)9)
#define EXPRESS_DISPLAY_DEVICE_ID ((uint64_t)10)
#define EXPRESS_CAMERA_DEVICE_ID ((uint64_t)11)
#define EXPRESS_MODEM_DEVICE_ID ((uint64_t)12)
#define EXPRESS_CODEC_DEVICE_ID ((uint64_t)13)

#define EXPRESS_WIFI_DEVICE_ID ((u64)20)

#define EXPRESS_NET_DEVICE_ID ((u64)30)

#define EXPRESS_BRIDGE_DEVICE_ID ((uint64_t)40)

#define EXPRESS_SYNC_DEVICE_ID ((uint64_t)50)

#define EXPRESS_MEM_DEVICE_ID ((uint64_t)60)


#define EXPRESS_TERMINATE_FUN_ID (0)
#define EXPRESS_CLUSTER_FUN_ID (9999)
#define EXPRESS_REGISTER_BUFFER_FUN_ID (999999)
#define EXPRESS_IRQ_FUN_ID (1000000)
#define EXPRESS_GET_PROP_FUN_ID (1000001)
#define EXPRESS_RELEASE_IRQ_FUN_ID (1000002)



#define CALL_BUF_SIZE 512

#define INPUT_DEVICE_TYPE 1
#define OUTPUT_DEVICE_TYPE 2




#define GET_DEVICE_ID(id) ((uint32_t)((id) >> 32))
#define GET_FUN_ID(id) ((uint32_t)((id)&0xffffff))
#define FUN_NEED_SYNC(id) (((id) >> 24) & 0x1)
#define FUN_HAS_HOST_SYNC(id) (((id) >> 24) & 0x2)

#define SYNC_FUN_ID(id) ((1L << 24) | (uint64_t)id)
#define HOST_SYNC_FUN_ID(id) ((1L << 25) | (uint64_t)id)

#define DEVICE_FUN_ID(device_id, id) (((uint64_t)device_id << 32) | id)



#define EXPRESS_DEVICE_INIT(device_name, info)                                       \
    static void __attribute__((constructor)) express_thread_init_##device_name(void) \
    {                                                                                \
        express_device_init_common(info);                                            \
    }

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define swap(a, b, type) \
    {                    \
        type temp = a;   \
        a = b;           \
        b = temp;        \
    }

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#define THREAD_CONTROL_BEGIN \
dispatch_sync(dispatch_get_main_queue(), ^{ 
#define THREAD_CONTROL_END \
});
#else
#define THREAD_CONTROL_BEGIN
#define THREAD_CONTROL_END
#endif




//struct iovec {
//     void *iov_base;
//     size_t iov_len;
// };
typedef struct Scatter_Data
{
    unsigned char *data;
    size_t len;
} Scatter_Data;

typedef struct Guest_Mem
{
    Scatter_Data *scatter_data;
    int num;
    int all_len;
} Guest_Mem;

typedef struct Call_Para
{
    // int is_direct;
    Guest_Mem *data;
    size_t data_len;
} Call_Para;




/**

 *
 */
typedef struct Teleport_Express_Queue_Elem
{
    VirtQueueElement elem;


    void *para;


    size_t len;

    /*
     * Logical view used for the guest NULL sentinel. VirtQueueElement owns
     * the original iovec and needs it unchanged when virtqueue_push() unmaps
     * the descriptor, so NULL normalization must never edit elem.{in,out}_sg.
     */
    Scatter_Data logical_null_scatter;

    /* Bytes actually written by the device for virtqueue used-length. */
    size_t written_len;

    struct Teleport_Express_Queue_Elem *next;
} Teleport_Express_Queue_Elem;

static inline void teleport_express_use_logical_null(
    Teleport_Express_Queue_Elem *elem, Guest_Mem *guest_mem)
{
    elem->logical_null_scatter.data = NULL;
    elem->logical_null_scatter.len = 0;
    elem->len = 0;
    guest_mem->scatter_data = &elem->logical_null_scatter;
    guest_mem->num = 1;
    guest_mem->all_len = 0;
}


typedef struct Teleport_Express_Call
{


    uint64_t id;

    uint64_t thread_id;

    uint64_t process_id;

    uint64_t unique_id;

    gint64 spend_time;


    uint64_t para_num;

    Teleport_Express_Queue_Elem *elem_header;
    Teleport_Express_Queue_Elem *elem_tail;

    VirtQueue *vq;
    VirtIODevice *vdev;


    void (*callback)(struct Teleport_Express_Call *call, int notify);

    struct Teleport_Express_Call *next;

    int is_end;

} Teleport_Express_Call;

typedef void (*EXPRESS_DECODE_FUN)(void *, Teleport_Express_Call *);


typedef struct Thread_Context
{

    uint64_t device_id;


    Teleport_Express_Call *call_buf[CALL_BUF_SIZE + 2];


    volatile int read_loc;
    volatile int write_loc;

    // int atomic_event_lock;


// QemuEvent data_event;
#ifdef _WIN32
    HANDLE data_event;
#else
    void *data_event;
#endif


    int init;


    int thread_run;


    uint64_t thread_id;

    uint64_t unique_id;

    uint64_t process_id;


    QemuThread this_thread;


    VirtIODevice *teleport_express_device;


    void (*context_init)(struct Thread_Context *context);

    void (*context_destroy)(struct Thread_Context *context);


    void (*call_handle)(struct Thread_Context *context, Teleport_Express_Call *call);

} Thread_Context;

struct Express_Device_Info;

typedef struct Device_Context{
    bool irq_enabled;
    Teleport_Express_Call *irq_call;
    struct Express_Device_Info *device_info;
} Device_Context;

typedef struct Express_Device_Info
{

    int device_index;
    bool enable;


    bool enable_default;


    const char *name;


    const char *option_name;


    const char *driver_name;


    int device_id;


    int device_type;


    void (*init)(void);


    void (*context_init)(struct Thread_Context *context);
    void (*context_destroy)(struct Thread_Context *context);
    void (*call_handle)(struct Thread_Context *context, Teleport_Express_Call *call);


    Thread_Context *(*get_context)(uint64_t device_id, uint64_t thread_id, uint64_t process_id, uint64_t unique_id, struct Express_Device_Info *info);


    bool (*remove_context)(uint64_t device_id, uint64_t thread_id, uint64_t process_id, uint64_t unique_id, struct Express_Device_Info *info);


    void (*buffer_register)(Guest_Mem *data, uint64_t thread_id, uint64_t process_id, uint64_t unique_id);
    

    Device_Context *(*get_device_context)(uint64_t device_id, uint64_t thread_id, uint64_t process_id, uint64_t unique_id, struct Express_Device_Info *info);

    void (*irq_register)(Device_Context *context);

    void (*irq_release)(Device_Context *context);


    void *static_prop;
    int static_prop_size;

} Express_Device_Info;


extern Device_Log_Setting_Info express_device_log_setting_info;

extern bool express_gpu_gl_debug_enable;
extern bool express_gpu_independ_window_enable;
extern bool express_device_input_window_enable;
extern bool teleport_express_save_snapshot;

extern bool express_gpu_keep_window_scale;

extern int express_gpu_window_width;
extern int express_gpu_window_height;

extern int *express_touchscreen_size;

extern bool express_touchscreen_scroll_is_zoom;
extern bool express_touchscreen_right_click_is_two_finger;
extern int express_touchscreen_scroll_ratio;

extern bool express_keyboard_finger_replay;

extern char *kernel_load_express_driver_names;
extern int kernel_load_express_driver_num;

extern int express_display_pixel_width;
extern int express_display_pixel_height;
extern int express_display_refresh_rate;
extern uint64_t express_display_count;
extern char *express_display_options;

extern bool express_display_switch_open;

extern bool express_gpu_open_shader_binary;

extern char *express_ruim_file;

void express_device_init_common(Express_Device_Info *info);

Express_Device_Info *get_express_device_info(unsigned int device_id);

void cluster_decode_invoke(Teleport_Express_Call *call, void *context, EXPRESS_DECODE_FUN decode_fun);


#endif
