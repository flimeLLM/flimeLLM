#include "qemu/osdep.h"

#include "hw/teleport-express/teleport_express_flime.h"
#include "hw/express-gpu/express_vk_flime_bridge.h"
#include "hw/express-gpu/vk_trans.h"

void teleport_express_flime_reset_transport(void)
{
    g_mutex_lock(&g_express_vk_transaction_lock);
    express_vk_flime_bridge_reset_transport();
    g_mutex_unlock(&g_express_vk_transaction_lock);
}

void teleport_express_flime_shutdown(void)
{
    g_mutex_lock(&g_express_vk_transaction_lock);
    express_vk_flime_bridge_shutdown();
    g_mutex_unlock(&g_express_vk_transaction_lock);
}
