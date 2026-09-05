softmmu_ss.add(when: 'CONFIG_TELEPORT_EXPRESS_PCI', if_true: files('teleport_express_pci.c'))

teleport_express = ss.source_set()

teleport_express.add(when: 'CONFIG_TELEPORT_EXPRESS',
               if_true: files(
                   'express_log.c',
                   'teleport_express.c',
                   'teleport_express_call.c',
                   'teleport_express_distribute.c',
                   'teleport_express_register.c',
                   'express_handle_thread.c',
                   'express_cluster_preflight.c',
                   'express_device_ctrl.c',
               ))

# Kconfig device symbols are poisoned in C sources.  Select either the real
# FLIME lifecycle bridge or a no-op implementation here so TELEPORT_EXPRESS
# does not gain an unconditional link dependency on EXPRESS_GPU.
teleport_express.add(when: 'CONFIG_EXPRESS_GPU',
               if_true: files('teleport_express_flime.c'),
               if_false: files('teleport_express_flime_stubs.c'))

if targetos == 'windows'
    teleport_express.add(when: 'CONFIG_TELEPORT_EXPRESS',
                if_true: files(
                    'express_event_win32.c',
                ))
else
    teleport_express.add(when: 'CONFIG_TELEPORT_EXPRESS',
                if_true: files(
                    'express_event_posix.c',
                ))
endif

softmmu_ss.add_all(teleport_express)
