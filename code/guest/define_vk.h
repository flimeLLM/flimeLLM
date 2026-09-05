

// #ifndef DEFINE_VK_H
// #define DEFINE_VK_H

#ifndef EXPRESS_GPU_DEVICE_ID
#define EXPRESS_GPU_DEVICE_ID ((uint64_t)1)
#endif

#define EXPRESS_GPU_FUN_ID ((uint64_t)1)

#define EXPRESS_GPU_NAME "/dev/express_gpu"
#define MAX_OUT_BUF_LEN 4096
// #define FUNID_vk_CreateInstance ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 1001)

#define FUNID_vkCreateInstance 1000
#define FUNID_vkAcquireNextImage2KHR 1011
#define FUNID_vkAcquireNextImageKHR 1012
#define FUNID_vkAllocateCommandBuffers 1013
#define FUNID_vkAllocateDescriptorSets 1014
#define FUNID_vkAllocateMemory 1015
#define FUNID_vkBeginCommandBuffer 1016
#define FUNID_vkBindBufferMemory 1017
#define FUNID_vkBindBufferMemory2 1018
#define FUNID_vkBindImageMemory 1019
#define FUNID_vkBindImageMemory2 1020
#define FUNID_vkCmdBeginQuery 1021
#define FUNID_vkCmdBeginRendering 1022
#define FUNID_vkCmdBeginRenderPass 1023
#define FUNID_vkCmdBeginRenderPass2 1024
#define FUNID_vkCmdBindDescriptorSets 1025
#define FUNID_vkCmdBindIndexBuffer 1026
#define FUNID_vkCmdBindPipeline 1027
#define FUNID_vkCmdBindVertexBuffers 1028
#define FUNID_vkCmdBindVertexBuffers2 1029
#define FUNID_vkCmdBlitImage 1030
#define FUNID_vkCmdBlitImage2 1031
#define FUNID_vkCmdClearAttachments 1032
#define FUNID_vkCmdClearColorImage 1033
#define FUNID_vkCmdClearDepthStencilImage 1034
#define FUNID_vkCmdCopyBuffer 1035
#define FUNID_vkCmdCopyBuffer2 1036
#define FUNID_vkCmdCopyBufferToImage 1037
#define FUNID_vkCmdCopyBufferToImage2 1038
#define FUNID_vkCmdCopyImage 1039
#define FUNID_vkCmdCopyImage2 1040
#define FUNID_vkCmdCopyImageToBuffer 1041
#define FUNID_vkCmdCopyImageToBuffer2 1042
#define FUNID_vkCmdCopyQueryPoolResults 1043
#define FUNID_vkCmdDispatch 1044
#define FUNID_vkCmdDispatchBase 1045
#define FUNID_vkCmdDispatchIndirect 1046
#define FUNID_vkCmdDraw 1047
#define FUNID_vkCmdDrawIndexed 1048
#define FUNID_vkCmdDrawIndexedIndirect 1049
#define FUNID_vkCmdDrawIndexedIndirectCount 1050
#define FUNID_vkCmdDrawIndirect 1051
#define FUNID_vkCmdDrawIndirectCount 1052
#define FUNID_vkCmdEndQuery 1053
#define FUNID_vkCmdEndRendering 1054
#define FUNID_vkCmdEndRenderPass 1055
#define FUNID_vkCmdEndRenderPass2 1056
#define FUNID_vkCmdExecuteCommands 1057
#define FUNID_vkCmdFillBuffer 1058
#define FUNID_vkCmdNextSubpass 1059
#define FUNID_vkCmdNextSubpass2 1060
#define FUNID_vkCmdPipelineBarrier 1061
#define FUNID_vkCmdPipelineBarrier2 1062
#define FUNID_vkCmdPushConstants 1063
#define FUNID_vkCmdResetEvent 1064
#define FUNID_vkCmdResetEvent2 1065
#define FUNID_vkCmdResetQueryPool 1066
#define FUNID_vkCmdResolveImage 1067
#define FUNID_vkCmdResolveImage2 1068
#define FUNID_vkCmdSetBlendConstants 1069
#define FUNID_vkCmdSetCullMode 1070
#define FUNID_vkCmdSetDepthBias 1071
#define FUNID_vkCmdSetDepthBiasEnable 1072
#define FUNID_vkCmdSetDepthBounds 1073
#define FUNID_vkCmdSetDepthBoundsTestEnable 1074
#define FUNID_vkCmdSetDepthCompareOp 1075
#define FUNID_vkCmdSetDepthTestEnable 1076
#define FUNID_vkCmdSetDepthWriteEnable 1077
#define FUNID_vkCmdSetDeviceMask 1078
#define FUNID_vkCmdSetEvent 1079
#define FUNID_vkCmdSetEvent2 1080
#define FUNID_vkCmdSetFrontFace 1081
#define FUNID_vkCmdSetLineWidth 1082
#define FUNID_vkCmdSetPrimitiveRestartEnable 1083
#define FUNID_vkCmdSetPrimitiveTopology 1084
#define FUNID_vkCmdSetRasterizerDiscardEnable 1085
#define FUNID_vkCmdSetScissor 1086
#define FUNID_vkCmdSetScissorWithCount 1087
#define FUNID_vkCmdSetStencilCompareMask 1088
#define FUNID_vkCmdSetStencilOp 1089
#define FUNID_vkCmdSetStencilReference 1090
#define FUNID_vkCmdSetStencilTestEnable 1091
#define FUNID_vkCmdSetStencilWriteMask 1092
#define FUNID_vkCmdSetViewport 1093
#define FUNID_vkCmdSetViewportWithCount 1094
#define FUNID_vkCmdUpdateBuffer 1095
#define FUNID_vkCmdWaitEvents 1096
#define FUNID_vkCmdWaitEvents2 1097
#define FUNID_vkCmdWriteTimestamp 1098
#define FUNID_vkCmdWriteTimestamp2 1099
#define FUNID_vkCreateBuffer                              1100
#define FUNID_vkCreateBufferView                          1101
#define FUNID_vkCreateCommandPool                         1102
#define FUNID_vkCreateComputePipelines                    1103
#define FUNID_vkCreateDescriptorPool                      1104
#define FUNID_vkCreateDescriptorSetLayout                 1105
#define FUNID_vkCreateDescriptorUpdateTemplate            1106
#define FUNID_vkCreateDevice                              1107
#define FUNID_vkCreateDisplayModeKHR                      1108
#define FUNID_vkCreateDisplayPlaneSurfaceKHR              1109
#define FUNID_vkCreateEvent                               1110
#define FUNID_vkCreateFence                               1111
#define FUNID_vkCreateFramebuffer                         1112
#define FUNID_vkCreateGraphicsPipelines                   1113
#define FUNID_vkCreateImage                               1114
#define FUNID_vkCreateImageView                           1115
#define FUNID_vkCreatePipelineCache                       1116
#define FUNID_vkCreatePipelineLayout                      1117
#define FUNID_vkCreatePrivateDataSlot                     1118
#define FUNID_vkCreateQueryPool                           1119
#define FUNID_vkCreateRenderPass                          1120
#define FUNID_vkCreateRenderPass2                         1121
#define FUNID_vkCreateSampler                             1122
#define FUNID_vkCreateSamplerYcbcrConversion              1123
#define FUNID_vkCreateSemaphore                           1124
#define FUNID_vkCreateShaderModule                        1125
#define FUNID_vkCreateSharedSwapchainsKHR                 1126
#define FUNID_vkCreateSwapchainKHR                        1127
#define FUNID_vkDestroyBuffer                             1128
#define FUNID_vkDestroyBufferView                         1129
#define FUNID_vkDestroyCommandPool                        1130
#define FUNID_vkDestroyDescriptorPool                     1131
#define FUNID_vkDestroyDescriptorSetLayout                1132
#define FUNID_vkDestroyDescriptorUpdateTemplate           1133
#define FUNID_vkDestroyDevice                             1134
#define FUNID_vkDestroyEvent                              1135
#define FUNID_vkDestroyFence                              1136
#define FUNID_vkDestroyFramebuffer                        1137
#define FUNID_vkDestroyImage                              1138
#define FUNID_vkDestroyImageView                          1139
#define FUNID_vkDestroyInstance                           1140
#define FUNID_vkDestroyPipeline                           1141
#define FUNID_vkDestroyPipelineCache                      1142
#define FUNID_vkDestroyPipelineLayout                     1143
#define FUNID_vkDestroyPrivateDataSlot                    1144
#define FUNID_vkDestroyQueryPool                          1145
#define FUNID_vkDestroyRenderPass                         1146
#define FUNID_vkDestroySampler                            1147
#define FUNID_vkDestroySamplerYcbcrConversion             1148
#define FUNID_vkDestroySemaphore                          1149
#define FUNID_vkDestroyShaderModule                       1150
#define FUNID_vkDestroySurfaceKHR                         1151
#define FUNID_vkDestroySwapchainKHR                       1152
#define FUNID_vkDeviceWaitIdle                            1153
#define FUNID_vkEndCommandBuffer                          1154
#define FUNID_vkEnumerateDeviceExtensionProperties        1155
#define FUNID_vkEnumerateDeviceLayerProperties            1156
#define FUNID_vkEnumerateInstanceExtensionProperties      1157
#define FUNID_vkEnumerateInstanceLayerProperties          1158
#define FUNID_vkEnumerateInstanceVersion                  1159
#define FUNID_vkEnumeratePhysicalDeviceGroups             1160
#define FUNID_vkEnumeratePhysicalDevices                  1161
#define FUNID_vkFlushMappedMemoryRanges                   1162
#define FUNID_vkFreeCommandBuffers                        1163
#define FUNID_vkFreeDescriptorSets                        1164
#define FUNID_vkFreeMemory                                1165
#define FUNID_vkGetBufferDeviceAddress                    1166
#define FUNID_vkGetBufferMemoryRequirements               1167
#define FUNID_vkGetBufferMemoryRequirements2              1168
#define FUNID_vkGetBufferOpaqueCaptureAddress             1169
#define FUNID_vkGetDescriptorSetLayoutSupport             1170
#define FUNID_vkGetDeviceBufferMemoryRequirements         1171
#define FUNID_vkGetDeviceGroupPeerMemoryFeatures          1172
#define FUNID_vkGetDeviceGroupPresentCapabilitiesKHR      1173
#define FUNID_vkGetDeviceGroupSurfacePresentModesKHR      1174
#define FUNID_vkGetDeviceImageMemoryRequirements          1175
#define FUNID_vkGetDeviceImageSparseMemoryRequirements    1176
#define FUNID_vkGetDeviceMemoryCommitment                 1177
#define FUNID_vkGetDeviceMemoryOpaqueCaptureAddress       1178
#define FUNID_vkGetDeviceProcAddr                         1179
#define FUNID_vkGetDeviceQueue                            1180
#define FUNID_vkGetDeviceQueue2                           1181
#define FUNID_vkGetDisplayModeProperties2KHR              1182
#define FUNID_vkGetDisplayModePropertiesKHR               1183
#define FUNID_vkGetDisplayPlaneCapabilities2KHR           1184
#define FUNID_vkGetDisplayPlaneCapabilitiesKHR            1185
#define FUNID_vkGetDisplayPlaneSupportedDisplaysKHR       1186
#define FUNID_vkGetEventStatus                            1187
#define FUNID_vkGetFenceStatus                            1188
#define FUNID_vkGetImageMemoryRequirements                1189
#define FUNID_vkGetImageMemoryRequirements2               1190
#define FUNID_vkGetImageSparseMemoryRequirements          1191
#define FUNID_vkGetImageSparseMemoryRequirements2         1192
#define FUNID_vkGetImageSubresourceLayout                 1193
#define FUNID_vkGetInstanceProcAddr                       1194
#define FUNID_vkGetPhysicalDeviceDisplayPlaneProperties2KHR 1195
#define FUNID_vkGetPhysicalDeviceDisplayPlanePropertiesKHR 1196
#define FUNID_vkGetPhysicalDeviceDisplayProperties2KHR     1197
#define FUNID_vkGetPhysicalDeviceDisplayPropertiesKHR      1198
#define FUNID_vkGetPhysicalDeviceExternalBufferProperties  1199
#define FUNID_vkGetPhysicalDeviceExternalFenceProperties   1200
#define FUNID_vkGetPhysicalDeviceExternalSemaphoreProperties 1201
#define FUNID_vkGetPhysicalDeviceFeatures                  1202
#define FUNID_vkGetPhysicalDeviceFeatures2                 1203
#define FUNID_vkGetPhysicalDeviceFormatProperties          1204
#define FUNID_vkGetPhysicalDeviceFormatProperties2         1205
#define FUNID_vkGetPhysicalDeviceImageFormatProperties     1206
#define FUNID_vkGetPhysicalDeviceImageFormatProperties2    1207
#define FUNID_vkGetPhysicalDeviceMemoryProperties          1208
#define FUNID_vkGetPhysicalDeviceMemoryProperties2         1209
#define FUNID_vkGetPhysicalDevicePresentRectanglesKHR      1210
#define FUNID_vkGetPhysicalDeviceProperties                1211
#define FUNID_vkGetPhysicalDeviceProperties2               1212
#define FUNID_vkGetPhysicalDeviceQueueFamilyProperties     1213
#define FUNID_vkGetPhysicalDeviceQueueFamilyProperties2    1214
#define FUNID_vkGetPhysicalDeviceSparseImageFormatProperties 1215
#define FUNID_vkGetPhysicalDeviceSparseImageFormatProperties2 1216
#define FUNID_vkGetPhysicalDeviceSurfaceCapabilities2KHR   1217
#define FUNID_vkGetPhysicalDeviceSurfaceCapabilitiesKHR    1218
#define FUNID_vkGetPhysicalDeviceSurfaceFormats2KHR        1219
#define FUNID_vkGetPhysicalDeviceSurfaceFormatsKHR         1220
#define FUNID_vkGetPhysicalDeviceSurfacePresentModesKHR    1221
#define FUNID_vkGetPhysicalDeviceSurfaceSupportKHR         1222
#define FUNID_vkGetPhysicalDeviceToolProperties            1223
#define FUNID_vkGetPipelineCacheData                       1224
#define FUNID_vkGetPrivateData                             1225
#define FUNID_vkGetQueryPoolResults                        1226
#define FUNID_vkGetRenderAreaGranularity                   1227
#define FUNID_vkGetSemaphoreCounterValue                   1228
#define FUNID_vkGetSwapchainImagesKHR                      1229
#define FUNID_vkInvalidateMappedMemoryRanges               1230
#define FUNID_vkMapMemory                                  1231
#define FUNID_vkMergePipelineCaches                        1232
#define FUNID_vkQueueBindSparse                            1233
#define FUNID_vkQueuePresentKHR                            1234
#define FUNID_vkQueueSubmit                                1235
#define FUNID_vkQueueSubmit2                               1236
#define FUNID_vkQueueWaitIdle                              1237
#define FUNID_vkResetCommandBuffer                         1238
#define FUNID_vkResetCommandPool                           1239
#define FUNID_vkResetDescriptorPool                        1240
#define FUNID_vkResetEvent                                 1241
#define FUNID_vkResetFences                                1242
#define FUNID_vkResetQueryPool                             1243
#define FUNID_vkSetEvent                                   1244
#define FUNID_vkSetPrivateData                             1245
#define FUNID_vkSignalSemaphore                            1246
#define FUNID_vkTrimCommandPool                            1247
#define FUNID_vkUnmapMemory                                1248
#define FUNID_vkUpdateDescriptorSets                       1249
#define FUNID_vkUpdateDescriptorSetWithTemplate            1250
#define FUNID_vkWaitForFences                              1251
#define FUNID_vkWaitSemaphores                             1252
#define FUNID_vkCreateAndroidSurfaceKHR                    1253




#define FUNID_glClientWaitSync ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 1)
#define FUNID_glTestInt1 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 2)
#define FUNID_glTestInt2 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 3)
#define FUNID_glTestInt3 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 4)
#define FUNID_glTestInt4 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 5)
#define FUNID_glTestInt5 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 6)
#define FUNID_glTestInt6 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 7)
#define FUNID_glTestPointer1 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 8)
#define FUNID_glTestPointer2 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 9)
#define FUNID_glTestPointer4 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 10)
#define FUNID_glTestString ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 11)
#define FUNID_glIsBuffer ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 12)
#define FUNID_glIsEnabled ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 13)
#define FUNID_glIsFramebuffer ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 14)
#define FUNID_glIsProgram ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 15)
#define FUNID_glIsRenderbuffer ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 16)
#define FUNID_glIsShader ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 17)
#define FUNID_glIsTexture ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 18)
#define FUNID_glIsQuery ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 19)
#define FUNID_glIsVertexArray ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 20)
#define FUNID_glIsSampler ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 21)
#define FUNID_glIsTransformFeedback ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 22)
#define FUNID_glIsSync ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 23)
#define FUNID_glGetError ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 24)
#define FUNID_glGetString_special ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 25)
#define FUNID_glGetStringi_special ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 26)
#define FUNID_glCheckFramebufferStatus ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 27)
#define FUNID_glQueryMatrixxOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 28)
#define FUNID_glGetFramebufferAttachmentParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 29)
#define FUNID_glGetProgramInfoLog ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 30)
#define FUNID_glGetRenderbufferParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 31)
#define FUNID_glGetShaderInfoLog ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 32)
#define FUNID_glGetShaderPrecisionFormat ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 33)
#define FUNID_glGetShaderSource ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 34)
#define FUNID_glGetTexParameterfv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 35)
#define FUNID_glGetTexParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 36)
#define FUNID_glGetQueryiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 37)
#define FUNID_glGetQueryObjectuiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 38)
#define FUNID_glGetTransformFeedbackVarying ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 39)
#define FUNID_glGetActiveUniformsiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 40)
#define FUNID_glGetActiveUniformBlockiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 41)
#define FUNID_glGetActiveUniformBlockName ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 42)
#define FUNID_glGetSamplerParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 43)
#define FUNID_glGetSamplerParameterfv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 44)
#define FUNID_glGetProgramBinary ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 45)
#define FUNID_glGetInternalformativ ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 46)
#define FUNID_glGetClipPlanexOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 47)
#define FUNID_glGetFixedvOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 48)
#define FUNID_glGetTexEnvxvOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 49)
#define FUNID_glGetTexParameterxvOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 50)
#define FUNID_glGetLightxvOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 51)
#define FUNID_glGetMaterialxvOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 52)
#define FUNID_glGetTexGenxvOES ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 53)
#define FUNID_glGetFramebufferParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 54)
#define FUNID_glGetProgramInterfaceiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 55)
#define FUNID_glGetProgramResourceName ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 56)
#define FUNID_glGetProgramResourceiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 57)
#define FUNID_glGetProgramPipelineiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 58)
#define FUNID_glGetProgramPipelineInfoLog ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 59)
#define FUNID_glGetMultisamplefv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 60)
#define FUNID_glGetTexLevelParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 61)
#define FUNID_glGetTexLevelParameterfv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 62)
#define FUNID_glGetSynciv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 63)
#define FUNID_glGetAttribLocation ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 64)
#define FUNID_glGetUniformLocation ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 65)
#define FUNID_glGetFragDataLocation ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 66)
#define FUNID_glGetUniformBlockIndex ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 67)
#define FUNID_glGetProgramResourceIndex ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 68)
#define FUNID_glGetProgramResourceLocation ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 69)
#define FUNID_glGetActiveAttrib ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 70)
#define FUNID_glGetActiveUniform ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 71)
#define FUNID_glGetAttachedShaders ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 72)
#define FUNID_glGetProgramiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 73)
#define FUNID_glGetShaderiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 74)
#define FUNID_glGetUniformfv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 75)
#define FUNID_glGetUniformiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 76)
#define FUNID_glGetUniformuiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 77)
#define FUNID_glGetUniformIndices ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 78)
#define FUNID_glGetVertexAttribfv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 79)
#define FUNID_glGetVertexAttribiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 80)
#define FUNID_glGetVertexAttribIiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 81)
#define FUNID_glGetVertexAttribIuiv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 82)
#define FUNID_glGetBufferParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 83)
#define FUNID_glGetBufferParameteri64v ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 84)
#define FUNID_glGetBooleanv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 85)
#define FUNID_glGetBooleani_v ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 86)
#define FUNID_glGetFloatv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 87)
#define FUNID_glGetIntegerv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 88)
#define FUNID_glGetIntegeri_v ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 89)
#define FUNID_glGetInteger64v ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 90)
#define FUNID_glGetInteger64i_v ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 91)
#define FUNID_glMapBufferRange_read ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 92)
#define FUNID_glReadPixels_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 93)
#define FUNID_glTestPointer3 ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 94)
#define FUNID_glFlush ((EXPRESS_GPU_FUN_ID << 32u) + 95)
#define FUNID_glFinish ((EXPRESS_GPU_FUN_ID << 32u) + 96)
#define FUNID_glBeginQuery ((EXPRESS_GPU_FUN_ID << 32u) + 97)
#define FUNID_glEndQuery ((EXPRESS_GPU_FUN_ID << 32u) + 98)
#define FUNID_glViewport ((EXPRESS_GPU_FUN_ID << 32u) + 99)
#define FUNID_glTexStorage2D ((EXPRESS_GPU_FUN_ID << 32u) + 100)
#define FUNID_glTexStorage3D ((EXPRESS_GPU_FUN_ID << 32u) + 101)
#define FUNID_glTexImage2D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 102)
#define FUNID_glTexSubImage2D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 103)
#define FUNID_glTexImage3D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 104)
#define FUNID_glTexSubImage3D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 105)
#define FUNID_glReadPixels_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 106)
#define FUNID_glCompressedTexImage2D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 107)
#define FUNID_glCompressedTexSubImage2D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 108)
#define FUNID_glCompressedTexImage3D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 109)
#define FUNID_glCompressedTexSubImage3D_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 110)
#define FUNID_glCopyTexImage2D ((EXPRESS_GPU_FUN_ID << 32u) + 111)
#define FUNID_glCopyTexSubImage2D ((EXPRESS_GPU_FUN_ID << 32u) + 112)
#define FUNID_glCopyTexSubImage3D ((EXPRESS_GPU_FUN_ID << 32u) + 113)
#define FUNID_glVertexAttribPointer_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 114)
#define FUNID_glVertexAttribPointer_offset ((EXPRESS_GPU_FUN_ID << 32u) + 115)
#define FUNID_glMapBufferRange_write ((EXPRESS_GPU_FUN_ID << 32u) + 116)
#define FUNID_glUnmapBuffer_special ((EXPRESS_GPU_FUN_ID << 32u) + 117)
#define FUNID_glWaitSync ((EXPRESS_GPU_FUN_ID << 32u) + 118)
#define FUNID_glShaderBinary ((EXPRESS_GPU_FUN_ID << 32u) + 119)
#define FUNID_glProgramBinary_special ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 120)
#define FUNID_glDrawBuffers ((EXPRESS_GPU_FUN_ID << 32u) + 121)
#define FUNID_glDrawArrays_origin ((EXPRESS_GPU_FUN_ID << 32u) + 122)
#define FUNID_glDrawArraysInstanced_origin ((EXPRESS_GPU_FUN_ID << 32u) + 123)
#define FUNID_glDrawElementsInstanced_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 124)
#define FUNID_glDrawElements_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 125)
#define FUNID_glDrawRangeElements_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 126)
#define FUNID_glTestIntAsyn ((EXPRESS_GPU_FUN_ID << 32u) + 127)
#define FUNID_glPrintfAsyn ((EXPRESS_GPU_FUN_ID << 32u) + 128)
#define FUNID_glEGLImageTargetTexture2DOES ((EXPRESS_GPU_FUN_ID << 32u) + 129)
#define FUNID_glEGLImageTargetRenderbufferStorageOES ((EXPRESS_GPU_FUN_ID << 32u) + 130)
#define FUNID_glGenBuffers ((EXPRESS_GPU_FUN_ID << 32u) + 131)
#define FUNID_glGenRenderbuffers ((EXPRESS_GPU_FUN_ID << 32u) + 132)
#define FUNID_glGenTextures ((EXPRESS_GPU_FUN_ID << 32u) + 133)
#define FUNID_glGenSamplers ((EXPRESS_GPU_FUN_ID << 32u) + 134)
#define FUNID_glCreateProgram ((EXPRESS_GPU_FUN_ID << 32u) + 135)
#define FUNID_glCreateShader ((EXPRESS_GPU_FUN_ID << 32u) + 136)
#define FUNID_glFenceSync ((EXPRESS_GPU_FUN_ID << 32u) + 137)
#define FUNID_glCreateShaderProgramv ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 138)
#define FUNID_glGenFramebuffers ((EXPRESS_GPU_FUN_ID << 32u) + 139)
#define FUNID_glGenProgramPipelines ((EXPRESS_GPU_FUN_ID << 32u) + 140)
#define FUNID_glGenTransformFeedbacks ((EXPRESS_GPU_FUN_ID << 32u) + 141)
#define FUNID_glGenVertexArrays ((EXPRESS_GPU_FUN_ID << 32u) + 142)
#define FUNID_glGenQueries ((EXPRESS_GPU_FUN_ID << 32u) + 143)
#define FUNID_glDeleteBuffers_origin ((EXPRESS_GPU_FUN_ID << 32u) + 144)
#define FUNID_glDeleteRenderbuffers ((EXPRESS_GPU_FUN_ID << 32u) + 145)
#define FUNID_glDeleteTextures ((EXPRESS_GPU_FUN_ID << 32u) + 146)
#define FUNID_glDeleteSamplers ((EXPRESS_GPU_FUN_ID << 32u) + 147)
#define FUNID_glDeleteProgram_origin ((EXPRESS_GPU_FUN_ID << 32u) + 148)
#define FUNID_glDeleteShader ((EXPRESS_GPU_FUN_ID << 32u) + 149)
#define FUNID_glDeleteSync ((EXPRESS_GPU_FUN_ID << 32u) + 150)
#define FUNID_glDeleteFramebuffers ((EXPRESS_GPU_FUN_ID << 32u) + 151)
#define FUNID_glDeleteProgramPipelines ((EXPRESS_GPU_FUN_ID << 32u) + 152)
#define FUNID_glDeleteTransformFeedbacks ((EXPRESS_GPU_FUN_ID << 32u) + 153)
#define FUNID_glDeleteVertexArrays_origin ((EXPRESS_GPU_FUN_ID << 32u) + 154)
#define FUNID_glDeleteQueries ((EXPRESS_GPU_FUN_ID << 32u) + 155)
#define FUNID_glLinkProgram_special ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 156)
#define FUNID_glPixelStorei_origin ((EXPRESS_GPU_FUN_ID << 32u) + 157)
#define FUNID_glDisableVertexAttribArray_origin ((EXPRESS_GPU_FUN_ID << 32u) + 158)
#define FUNID_glEnableVertexAttribArray_origin ((EXPRESS_GPU_FUN_ID << 32u) + 159)
#define FUNID_glReadBuffer_special ((EXPRESS_GPU_FUN_ID << 32u) + 160)
#define FUNID_glVertexAttribDivisor_origin ((EXPRESS_GPU_FUN_ID << 32u) + 161)
#define FUNID_glShaderSource_special ((EXPRESS_GPU_FUN_ID << 32u) + 162)
#define FUNID_glVertexAttribIPointer_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 163)
#define FUNID_glVertexAttribIPointer_offset ((EXPRESS_GPU_FUN_ID << 32u) + 164)
#define FUNID_glBindVertexArray_special ((EXPRESS_GPU_FUN_ID << 32u) + 165)
#define FUNID_glBindBuffer_origin ((EXPRESS_GPU_FUN_ID << 32u) + 166)
#define FUNID_glBeginTransformFeedback ((EXPRESS_GPU_FUN_ID << 32u) + 167)
#define FUNID_glEndTransformFeedback ((EXPRESS_GPU_FUN_ID << 32u) + 168)
#define FUNID_glPauseTransformFeedback ((EXPRESS_GPU_FUN_ID << 32u) + 169)
#define FUNID_glResumeTransformFeedback ((EXPRESS_GPU_FUN_ID << 32u) + 170)
#define FUNID_glBindBufferRange ((EXPRESS_GPU_FUN_ID << 32u) + 171)
#define FUNID_glBindBufferBase ((EXPRESS_GPU_FUN_ID << 32u) + 172)
#define FUNID_glBindTexture ((EXPRESS_GPU_FUN_ID << 32u) + 173)
#define FUNID_glBindRenderbuffer ((EXPRESS_GPU_FUN_ID << 32u) + 174)
#define FUNID_glBindSampler ((EXPRESS_GPU_FUN_ID << 32u) + 175)
#define FUNID_glBindFramebuffer ((EXPRESS_GPU_FUN_ID << 32u) + 176)
#define FUNID_glBindProgramPipeline ((EXPRESS_GPU_FUN_ID << 32u) + 177)
#define FUNID_glBindTransformFeedback ((EXPRESS_GPU_FUN_ID << 32u) + 178)
#define FUNID_glActiveTexture ((EXPRESS_GPU_FUN_ID << 32u) + 179)
#define FUNID_glAttachShader ((EXPRESS_GPU_FUN_ID << 32u) + 180)
#define FUNID_glBlendColor ((EXPRESS_GPU_FUN_ID << 32u) + 181)
#define FUNID_glBlendEquation ((EXPRESS_GPU_FUN_ID << 32u) + 182)
#define FUNID_glBlendEquationSeparate ((EXPRESS_GPU_FUN_ID << 32u) + 183)
#define FUNID_glBlendFunc ((EXPRESS_GPU_FUN_ID << 32u) + 184)
#define FUNID_glBlendFuncSeparate ((EXPRESS_GPU_FUN_ID << 32u) + 185)
#define FUNID_glClear ((EXPRESS_GPU_FUN_ID << 32u) + 186)
#define FUNID_glClearColor ((EXPRESS_GPU_FUN_ID << 32u) + 187)
#define FUNID_glClearDepthf ((EXPRESS_GPU_FUN_ID << 32u) + 188)
#define FUNID_glClearStencil ((EXPRESS_GPU_FUN_ID << 32u) + 189)
#define FUNID_glColorMask ((EXPRESS_GPU_FUN_ID << 32u) + 190)
#define FUNID_glCompileShader ((EXPRESS_GPU_FUN_ID << 32u) + 191)
#define FUNID_glCullFace ((EXPRESS_GPU_FUN_ID << 32u) + 192)
#define FUNID_glDepthFunc ((EXPRESS_GPU_FUN_ID << 32u) + 193)
#define FUNID_glDepthMask ((EXPRESS_GPU_FUN_ID << 32u) + 194)
#define FUNID_glDepthRangef ((EXPRESS_GPU_FUN_ID << 32u) + 195)
#define FUNID_glDetachShader ((EXPRESS_GPU_FUN_ID << 32u) + 196)
#define FUNID_glDisable ((EXPRESS_GPU_FUN_ID << 32u) + 197)
#define FUNID_glEnable ((EXPRESS_GPU_FUN_ID << 32u) + 198)
#define FUNID_glFramebufferRenderbuffer ((EXPRESS_GPU_FUN_ID << 32u) + 199)
#define FUNID_glFramebufferTexture2D ((EXPRESS_GPU_FUN_ID << 32u) + 200)
#define FUNID_glFrontFace ((EXPRESS_GPU_FUN_ID << 32u) + 201)
#define FUNID_glGenerateMipmap ((EXPRESS_GPU_FUN_ID << 32u) + 202)
#define FUNID_glHint ((EXPRESS_GPU_FUN_ID << 32u) + 203)
#define FUNID_glLineWidth ((EXPRESS_GPU_FUN_ID << 32u) + 204)
#define FUNID_glPolygonOffset ((EXPRESS_GPU_FUN_ID << 32u) + 205)
#define FUNID_glReleaseShaderCompiler ((EXPRESS_GPU_FUN_ID << 32u) + 206)
#define FUNID_glRenderbufferStorage ((EXPRESS_GPU_FUN_ID << 32u) + 207)
#define FUNID_glSampleCoverage ((EXPRESS_GPU_FUN_ID << 32u) + 208)
#define FUNID_glScissor ((EXPRESS_GPU_FUN_ID << 32u) + 209)
#define FUNID_glStencilFunc ((EXPRESS_GPU_FUN_ID << 32u) + 210)
#define FUNID_glStencilFuncSeparate ((EXPRESS_GPU_FUN_ID << 32u) + 211)
#define FUNID_glStencilMask ((EXPRESS_GPU_FUN_ID << 32u) + 212)
#define FUNID_glStencilMaskSeparate ((EXPRESS_GPU_FUN_ID << 32u) + 213)
#define FUNID_glStencilOp ((EXPRESS_GPU_FUN_ID << 32u) + 214)
#define FUNID_glStencilOpSeparate ((EXPRESS_GPU_FUN_ID << 32u) + 215)
#define FUNID_glTexParameterf ((EXPRESS_GPU_FUN_ID << 32u) + 216)
#define FUNID_glTexParameteri ((EXPRESS_GPU_FUN_ID << 32u) + 217)
#define FUNID_glUniform1f ((EXPRESS_GPU_FUN_ID << 32u) + 218)
#define FUNID_glUniform1i ((EXPRESS_GPU_FUN_ID << 32u) + 219)
#define FUNID_glUniform2f ((EXPRESS_GPU_FUN_ID << 32u) + 220)
#define FUNID_glUniform2i ((EXPRESS_GPU_FUN_ID << 32u) + 221)
#define FUNID_glUniform3f ((EXPRESS_GPU_FUN_ID << 32u) + 222)
#define FUNID_glUniform3i ((EXPRESS_GPU_FUN_ID << 32u) + 223)
#define FUNID_glUniform4f ((EXPRESS_GPU_FUN_ID << 32u) + 224)
#define FUNID_glUniform4i ((EXPRESS_GPU_FUN_ID << 32u) + 225)
#define FUNID_glUseProgram ((EXPRESS_GPU_FUN_ID << 32u) + 226)
#define FUNID_glValidateProgram ((EXPRESS_GPU_FUN_ID << 32u) + 227)
#define FUNID_glVertexAttrib1f ((EXPRESS_GPU_FUN_ID << 32u) + 228)
#define FUNID_glVertexAttrib2f ((EXPRESS_GPU_FUN_ID << 32u) + 229)
#define FUNID_glVertexAttrib3f ((EXPRESS_GPU_FUN_ID << 32u) + 230)
#define FUNID_glVertexAttrib4f ((EXPRESS_GPU_FUN_ID << 32u) + 231)
#define FUNID_glBlitFramebuffer ((EXPRESS_GPU_FUN_ID << 32u) + 232)
#define FUNID_glRenderbufferStorageMultisample ((EXPRESS_GPU_FUN_ID << 32u) + 233)
#define FUNID_glFramebufferTextureLayer ((EXPRESS_GPU_FUN_ID << 32u) + 234)
#define FUNID_glVertexAttribI4i ((EXPRESS_GPU_FUN_ID << 32u) + 235)
#define FUNID_glVertexAttribI4ui ((EXPRESS_GPU_FUN_ID << 32u) + 236)
#define FUNID_glUniform1ui ((EXPRESS_GPU_FUN_ID << 32u) + 237)
#define FUNID_glUniform2ui ((EXPRESS_GPU_FUN_ID << 32u) + 238)
#define FUNID_glUniform3ui ((EXPRESS_GPU_FUN_ID << 32u) + 239)
#define FUNID_glUniform4ui ((EXPRESS_GPU_FUN_ID << 32u) + 240)
#define FUNID_glClearBufferfi ((EXPRESS_GPU_FUN_ID << 32u) + 241)
#define FUNID_glCopyBufferSubData ((EXPRESS_GPU_FUN_ID << 32u) + 242)
#define FUNID_glUniformBlockBinding ((EXPRESS_GPU_FUN_ID << 32u) + 243)
#define FUNID_glSamplerParameteri ((EXPRESS_GPU_FUN_ID << 32u) + 244)
#define FUNID_glSamplerParameterf ((EXPRESS_GPU_FUN_ID << 32u) + 245)
#define FUNID_glProgramParameteri ((EXPRESS_GPU_FUN_ID << 32u) + 246)
#define FUNID_glAlphaFuncxOES ((EXPRESS_GPU_FUN_ID << 32u) + 247)
#define FUNID_glClearColorxOES ((EXPRESS_GPU_FUN_ID << 32u) + 248)
#define FUNID_glClearDepthxOES ((EXPRESS_GPU_FUN_ID << 32u) + 249)
#define FUNID_glColor4xOES ((EXPRESS_GPU_FUN_ID << 32u) + 250)
#define FUNID_glDepthRangexOES ((EXPRESS_GPU_FUN_ID << 32u) + 251)
#define FUNID_glFogxOES ((EXPRESS_GPU_FUN_ID << 32u) + 252)
#define FUNID_glFrustumxOES ((EXPRESS_GPU_FUN_ID << 32u) + 253)
#define FUNID_glLightModelxOES ((EXPRESS_GPU_FUN_ID << 32u) + 254)
#define FUNID_glLightxOES ((EXPRESS_GPU_FUN_ID << 32u) + 255)
#define FUNID_glLineWidthxOES ((EXPRESS_GPU_FUN_ID << 32u) + 256)
#define FUNID_glMaterialxOES ((EXPRESS_GPU_FUN_ID << 32u) + 257)
#define FUNID_glMultiTexCoord4xOES ((EXPRESS_GPU_FUN_ID << 32u) + 258)
#define FUNID_glNormal3xOES ((EXPRESS_GPU_FUN_ID << 32u) + 259)
#define FUNID_glOrthoxOES ((EXPRESS_GPU_FUN_ID << 32u) + 260)
#define FUNID_glPointSizexOES ((EXPRESS_GPU_FUN_ID << 32u) + 261)
#define FUNID_glPolygonOffsetxOES ((EXPRESS_GPU_FUN_ID << 32u) + 262)
#define FUNID_glRotatexOES ((EXPRESS_GPU_FUN_ID << 32u) + 263)
#define FUNID_glScalexOES ((EXPRESS_GPU_FUN_ID << 32u) + 264)
#define FUNID_glTexEnvxOES ((EXPRESS_GPU_FUN_ID << 32u) + 265)
#define FUNID_glTranslatexOES ((EXPRESS_GPU_FUN_ID << 32u) + 266)
#define FUNID_glPointParameterxOES ((EXPRESS_GPU_FUN_ID << 32u) + 267)
#define FUNID_glSampleCoveragexOES ((EXPRESS_GPU_FUN_ID << 32u) + 268)
#define FUNID_glTexGenxOES ((EXPRESS_GPU_FUN_ID << 32u) + 269)
#define FUNID_glClearDepthfOES ((EXPRESS_GPU_FUN_ID << 32u) + 270)
#define FUNID_glDepthRangefOES ((EXPRESS_GPU_FUN_ID << 32u) + 271)
#define FUNID_glFrustumfOES ((EXPRESS_GPU_FUN_ID << 32u) + 272)
#define FUNID_glOrthofOES ((EXPRESS_GPU_FUN_ID << 32u) + 273)
#define FUNID_glRenderbufferStorageMultisampleEXT ((EXPRESS_GPU_FUN_ID << 32u) + 274)
#define FUNID_glUseProgramStages ((EXPRESS_GPU_FUN_ID << 32u) + 275)
#define FUNID_glActiveShaderProgram ((EXPRESS_GPU_FUN_ID << 32u) + 276)
#define FUNID_glProgramUniform1i ((EXPRESS_GPU_FUN_ID << 32u) + 277)
#define FUNID_glProgramUniform2i ((EXPRESS_GPU_FUN_ID << 32u) + 278)
#define FUNID_glProgramUniform3i ((EXPRESS_GPU_FUN_ID << 32u) + 279)
#define FUNID_glProgramUniform4i ((EXPRESS_GPU_FUN_ID << 32u) + 280)
#define FUNID_glProgramUniform1ui ((EXPRESS_GPU_FUN_ID << 32u) + 281)
#define FUNID_glProgramUniform2ui ((EXPRESS_GPU_FUN_ID << 32u) + 282)
#define FUNID_glProgramUniform3ui ((EXPRESS_GPU_FUN_ID << 32u) + 283)
#define FUNID_glProgramUniform4ui ((EXPRESS_GPU_FUN_ID << 32u) + 284)
#define FUNID_glProgramUniform1f ((EXPRESS_GPU_FUN_ID << 32u) + 285)
#define FUNID_glProgramUniform2f ((EXPRESS_GPU_FUN_ID << 32u) + 286)
#define FUNID_glProgramUniform3f ((EXPRESS_GPU_FUN_ID << 32u) + 287)
#define FUNID_glProgramUniform4f ((EXPRESS_GPU_FUN_ID << 32u) + 288)
#define FUNID_glTransformFeedbackVaryings ((EXPRESS_GPU_FUN_ID << 32u) + 289)
#define FUNID_glTexParameterfv ((EXPRESS_GPU_FUN_ID << 32u) + 290)
#define FUNID_glTexParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + 291)
#define FUNID_glUniform1fv ((EXPRESS_GPU_FUN_ID << 32u) + 292)
#define FUNID_glUniform1iv ((EXPRESS_GPU_FUN_ID << 32u) + 293)
#define FUNID_glUniform2fv ((EXPRESS_GPU_FUN_ID << 32u) + 294)
#define FUNID_glUniform2iv ((EXPRESS_GPU_FUN_ID << 32u) + 295)
#define FUNID_glUniform3fv ((EXPRESS_GPU_FUN_ID << 32u) + 296)
#define FUNID_glUniform3iv ((EXPRESS_GPU_FUN_ID << 32u) + 297)
#define FUNID_glUniform4fv ((EXPRESS_GPU_FUN_ID << 32u) + 298)
#define FUNID_glUniform4iv ((EXPRESS_GPU_FUN_ID << 32u) + 299)
#define FUNID_glVertexAttrib1fv ((EXPRESS_GPU_FUN_ID << 32u) + 300)
#define FUNID_glVertexAttrib2fv ((EXPRESS_GPU_FUN_ID << 32u) + 301)
#define FUNID_glVertexAttrib3fv ((EXPRESS_GPU_FUN_ID << 32u) + 302)
#define FUNID_glVertexAttrib4fv ((EXPRESS_GPU_FUN_ID << 32u) + 303)
#define FUNID_glUniformMatrix2fv ((EXPRESS_GPU_FUN_ID << 32u) + 304)
#define FUNID_glUniformMatrix3fv ((EXPRESS_GPU_FUN_ID << 32u) + 305)
#define FUNID_glUniformMatrix4fv ((EXPRESS_GPU_FUN_ID << 32u) + 306)
#define FUNID_glUniformMatrix2x3fv ((EXPRESS_GPU_FUN_ID << 32u) + 307)
#define FUNID_glUniformMatrix3x2fv ((EXPRESS_GPU_FUN_ID << 32u) + 308)
#define FUNID_glUniformMatrix2x4fv ((EXPRESS_GPU_FUN_ID << 32u) + 309)
#define FUNID_glUniformMatrix4x2fv ((EXPRESS_GPU_FUN_ID << 32u) + 310)
#define FUNID_glUniformMatrix3x4fv ((EXPRESS_GPU_FUN_ID << 32u) + 311)
#define FUNID_glUniformMatrix4x3fv ((EXPRESS_GPU_FUN_ID << 32u) + 312)
#define FUNID_glVertexAttribI4iv ((EXPRESS_GPU_FUN_ID << 32u) + 313)
#define FUNID_glVertexAttribI4uiv ((EXPRESS_GPU_FUN_ID << 32u) + 314)
#define FUNID_glUniform1uiv ((EXPRESS_GPU_FUN_ID << 32u) + 315)
#define FUNID_glUniform2uiv ((EXPRESS_GPU_FUN_ID << 32u) + 316)
#define FUNID_glUniform3uiv ((EXPRESS_GPU_FUN_ID << 32u) + 317)
#define FUNID_glUniform4uiv ((EXPRESS_GPU_FUN_ID << 32u) + 318)
#define FUNID_glClearBufferiv ((EXPRESS_GPU_FUN_ID << 32u) + 319)
#define FUNID_glClearBufferuiv ((EXPRESS_GPU_FUN_ID << 32u) + 320)
#define FUNID_glClearBufferfv ((EXPRESS_GPU_FUN_ID << 32u) + 321)
#define FUNID_glSamplerParameteriv ((EXPRESS_GPU_FUN_ID << 32u) + 322)
#define FUNID_glSamplerParameterfv ((EXPRESS_GPU_FUN_ID << 32u) + 323)
#define FUNID_glInvalidateFramebuffer ((EXPRESS_GPU_FUN_ID << 32u) + 324)
#define FUNID_glInvalidateSubFramebuffer ((EXPRESS_GPU_FUN_ID << 32u) + 325)
#define FUNID_glClipPlanexOES ((EXPRESS_GPU_FUN_ID << 32u) + 326)
#define FUNID_glFogxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 327)
#define FUNID_glLightModelxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 328)
#define FUNID_glLightxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 329)
#define FUNID_glLoadMatrixxOES ((EXPRESS_GPU_FUN_ID << 32u) + 330)
#define FUNID_glMaterialxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 331)
#define FUNID_glMultMatrixxOES ((EXPRESS_GPU_FUN_ID << 32u) + 332)
#define FUNID_glPointParameterxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 333)
#define FUNID_glTexEnvxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 334)
#define FUNID_glClipPlanefOES ((EXPRESS_GPU_FUN_ID << 32u) + 335)
#define FUNID_glTexGenxvOES ((EXPRESS_GPU_FUN_ID << 32u) + 336)
#define FUNID_glProgramUniform1iv ((EXPRESS_GPU_FUN_ID << 32u) + 337)
#define FUNID_glProgramUniform2iv ((EXPRESS_GPU_FUN_ID << 32u) + 338)
#define FUNID_glProgramUniform3iv ((EXPRESS_GPU_FUN_ID << 32u) + 339)
#define FUNID_glProgramUniform4iv ((EXPRESS_GPU_FUN_ID << 32u) + 340)
#define FUNID_glProgramUniform1uiv ((EXPRESS_GPU_FUN_ID << 32u) + 341)
#define FUNID_glProgramUniform2uiv ((EXPRESS_GPU_FUN_ID << 32u) + 342)
#define FUNID_glProgramUniform3uiv ((EXPRESS_GPU_FUN_ID << 32u) + 343)
#define FUNID_glProgramUniform4uiv ((EXPRESS_GPU_FUN_ID << 32u) + 344)
#define FUNID_glProgramUniform1fv ((EXPRESS_GPU_FUN_ID << 32u) + 345)
#define FUNID_glProgramUniform2fv ((EXPRESS_GPU_FUN_ID << 32u) + 346)
#define FUNID_glProgramUniform3fv ((EXPRESS_GPU_FUN_ID << 32u) + 347)
#define FUNID_glProgramUniform4fv ((EXPRESS_GPU_FUN_ID << 32u) + 348)
#define FUNID_glProgramUniformMatrix2fv ((EXPRESS_GPU_FUN_ID << 32u) + 349)
#define FUNID_glProgramUniformMatrix3fv ((EXPRESS_GPU_FUN_ID << 32u) + 350)
#define FUNID_glProgramUniformMatrix4fv ((EXPRESS_GPU_FUN_ID << 32u) + 351)
#define FUNID_glProgramUniformMatrix2x3fv ((EXPRESS_GPU_FUN_ID << 32u) + 352)
#define FUNID_glProgramUniformMatrix3x2fv ((EXPRESS_GPU_FUN_ID << 32u) + 353)
#define FUNID_glProgramUniformMatrix2x4fv ((EXPRESS_GPU_FUN_ID << 32u) + 354)
#define FUNID_glProgramUniformMatrix4x2fv ((EXPRESS_GPU_FUN_ID << 32u) + 355)
#define FUNID_glProgramUniformMatrix3x4fv ((EXPRESS_GPU_FUN_ID << 32u) + 356)
#define FUNID_glProgramUniformMatrix4x3fv ((EXPRESS_GPU_FUN_ID << 32u) + 357)
#define FUNID_glBindAttribLocation ((EXPRESS_GPU_FUN_ID << 32u) + 358)
#define FUNID_glTexEnvf ((EXPRESS_GPU_FUN_ID << 32u) + 359)
#define FUNID_glTexEnvi ((EXPRESS_GPU_FUN_ID << 32u) + 360)
#define FUNID_glTexEnvx ((EXPRESS_GPU_FUN_ID << 32u) + 361)
#define FUNID_glTexParameterx ((EXPRESS_GPU_FUN_ID << 32u) + 362)
#define FUNID_glShadeModel ((EXPRESS_GPU_FUN_ID << 32u) + 363)
#define FUNID_glDrawTexiOES ((EXPRESS_GPU_FUN_ID << 32u) + 364)
#define FUNID_glVertexAttribIPointer_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 365)
#define FUNID_glVertexAttribPointer_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 366)
#define FUNID_glDrawElements_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 367)
#define FUNID_glDrawElementsInstanced_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 368)
#define FUNID_glDrawRangeElements_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 369)
#define FUNID_glFlushMappedBufferRange_special ((EXPRESS_GPU_FUN_ID << 32u) + 370)
#define FUNID_glBufferData_custom ((EXPRESS_GPU_FUN_ID << 32u) + 371)
#define FUNID_glBufferSubData_custom ((EXPRESS_GPU_FUN_ID << 32u) + 372)
#define FUNID_glCompressedTexImage2D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 373)
#define FUNID_glCompressedTexSubImage2D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 374)
#define FUNID_glCompressedTexImage3D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 375)
#define FUNID_glCompressedTexSubImage3D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 376)
#define FUNID_glTexImage2D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 377)
#define FUNID_glTexImage3D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 378)
#define FUNID_glTexSubImage2D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 379)
#define FUNID_glTexSubImage3D_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 380)
#define FUNID_glPrintf ((EXPRESS_GPU_FUN_ID << 32u) + 381)

#define FUNID_glBindEGLImage ((EXPRESS_GPU_FUN_ID << 32u) + 382)

// #define FUNID_glGraphicBufferData ((EXPRESS_GPU_FUN_ID << 32u) + 383)

// #define FUNID_glReadGraphicBuffer ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 384)

#define FUNID_glGetStaticValues ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 385)

#define FUNID_glGetProgramData ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 386)

#define FUNID_glSync ((EXPRESS_GPU_FUN_ID << 32u) + (((uint64_t)0x1) << 24u) + 387)

#define FUNID_glBindImageTexture ((EXPRESS_GPU_FUN_ID << 32u) + 388)

#define FUNID_glBindVertexBuffer ((EXPRESS_GPU_FUN_ID << 32u) + 389)

#define FUNID_glVertexAttribFormat ((EXPRESS_GPU_FUN_ID << 32u) + 390)

#define FUNID_glVertexAttribIFormat ((EXPRESS_GPU_FUN_ID << 32u) + 391)

#define FUNID_glVertexAttribBinding ((EXPRESS_GPU_FUN_ID << 32u) + 392)

#define FUNID_glDispatchCompute ((EXPRESS_GPU_FUN_ID << 32u) + 393)

#define FUNID_glDispatchComputeIndirect ((EXPRESS_GPU_FUN_ID << 32u) + 394)

#define FUNID_glMemoryBarrier ((EXPRESS_GPU_FUN_ID << 32u) + 395)

#define FUNID_glMemoryBarrierByRegion ((EXPRESS_GPU_FUN_ID << 32u) + 396)

#define FUNID_glFramebufferParameteri ((EXPRESS_GPU_FUN_ID << 32u) + 397)

#define FUNID_glSampleMaski ((EXPRESS_GPU_FUN_ID << 32u) + 398)

#define FUNID_glTexStorage2DMultisample ((EXPRESS_GPU_FUN_ID << 32u) + 399)

#define FUNID_glValidateProgramPipeline ((EXPRESS_GPU_FUN_ID << 32u) + 400)

#define FUNID_glVertexBindingDivisor ((EXPRESS_GPU_FUN_ID << 32u) + 401)

#define FUNID_glDrawArraysIndirect_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 402)

#define FUNID_glDrawArraysIndirect_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 403)

#define FUNID_glDrawElementsIndirect_with_bound ((EXPRESS_GPU_FUN_ID << 32u) + 404)

#define FUNID_glDrawElementsIndirect_without_bound ((EXPRESS_GPU_FUN_ID << 32u) + 405)

#define FUNID_glDiscardFramebufferEXT ((EXPRESS_GPU_FUN_ID << 32u) + 406)

#define FUNID_glTexBuffer ((EXPRESS_GPU_FUN_ID << 32u) + 407)

#define FUNID_glTexBufferRange ((EXPRESS_GPU_FUN_ID << 32u) + 408)

#define FUNID_glColorMaski ((EXPRESS_GPU_FUN_ID << 32u) + 409)

#define FUNID_glBlendFuncSeparatei ((EXPRESS_GPU_FUN_ID << 32u) + 410)

#define FUNID_glBlendEquationSeparatei ((EXPRESS_GPU_FUN_ID << 32u) + 411)

// #define FUNID_glBindSharedGLImage ((EXPRESS_GPU_FUN_ID << 32u) + 406)

// #define FUNID_glFramebufferSharedTexture2D ((EXPRESS_GPU_FUN_ID << 32u) + 407)

// #define FUNID_glFramebufferEGLImage ((EXPRESS_GPU_FUN_ID << 32u) + 408)

