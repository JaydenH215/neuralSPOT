/**
 * @file tflm_validator.cc
 * @author Carlos Morales
 * @brief Shell application for instantiating a TFLM model and allowing a RPC
 * client to set input tensors, invoke() it, and collect output tensors
 * @version 0.1
 * @date 2023-02-28
 *
 * @copyright Copyright (c) 2023
 *
 */

#include <cstdlib>
#include <cstring>

#include "generic_model.h"
#include "mut_model_metadata.h"
#include "tflm_validator.h"

#include "ns_ambiqsuite_harness.h"
#include "ns_core.h"
#include "ns_malloc.h"
#include "ns_peripherals_button.h"
#include "ns_peripherals_power.h"
#include "ns_rpc_generic_data.h"
#include "ns_usb.h"

ns_incoming_config_t mut_cfg;
ns_outgoing_stats_t mut_stats;
static uint32_t invokes_so_far = 0;

/**
 * @brief Initializes the model per config struct
 *
 * @param block - serialized tflm_config_struct
 * @return status
 */
status
configureModel(const dataBlock *in) {
    ns_lp_printf("[INFO] PC requested model initialization\n");

    // Grab incoming buffer, decode into config struct
    if (in->buffer.dataLength != sizeof(mut_cfg)) {
        ns_lp_printf("[ERROR] Configuration Size mismatch, expected %d, got %d", sizeof(mut_cfg),
                     in->buffer.dataLength);
        return ns_rpc_data_failure;
    }

    memcpy(&mut_cfg, in->buffer.data, sizeof(mut_cfg));
    ns_lp_printf("[INFO] MUT configuration: profile %d, warmup %d, input tensor length %d, output "
                 "tensor length %d\n",
                 mut_cfg.config.profile_mut, mut_cfg.config.profile_warmup,
                 mut_cfg.config.input_length, mut_cfg.config.output_length);

    int status = model_init();
    ns_rpc_genericDataOperations_printDatablock(in);
    if (status == 0) {
        return ns_rpc_data_success;
    } else {
        return ns_rpc_data_failure;
    }
}

// Handler for fetchBlockFromEVB, invoked by PC
/**
 * @brief Get the statistics about the last Invoke()
 *
 * @param block - serialized tflm_stats_struct
 * @return status
 */
status
getStatistics(dataBlock *res) {
    ns_lp_printf("[INFO] Server asked for statistics\n");

    uint8_t *resultBuffer =
        (uint8_t *)ns_malloc(sizeof(mut_stats.bytes) *
                             sizeof(uint8_t)); // see above for explanation of why we need malloc
    char *msg_store = (char *)ns_malloc(sizeof(char) * 30);
    res->length = sizeof(mut_stats.bytes) * sizeof(uint8_t);
    res->dType = uint8_e;
    res->description = msg_store;
    res->cmd = generic_cmd;
    binary_t binaryBlock = {.data = (uint8_t *)resultBuffer,
                            .dataLength = sizeof(mut_stats.bytes) * sizeof(uint8_t)};
    res->buffer = binaryBlock;
    mut_stats.stats.computed_stat_buffer_size = sizeof(mut_stats.bytes);
    mut_stats.stats.computed_stat_per_event_size = sizeof(ns_profiler_event_stats_t);
#ifdef NS_TFLM_VALIDATOR
    mut_stats.stats.captured_events = ns_microProfilerSidecar.captured_event_num;
    memcpy(mut_stats.stats.stat_buffer, ns_profiler_events_stats,
           sizeof(mut_stats.stats.stat_buffer));
#else
    mut_stats.stats.captured_events = 0;
#endif
    memcpy(resultBuffer, mut_stats.bytes, sizeof(mut_stats.bytes) * sizeof(uint8_t));

    char msg[] = "StatsOK\0";
    memcpy(msg_store, msg, sizeof(msg));

    return ns_rpc_data_success;
}

// Handler for computeOnEVB, invoked by PC
/**
 * @brief Calls TFLM's invoke()
 *
 * @param in  - input tensor
 * @param res - output tensor
 * @return status - fail if not configured or if invoke fails
 */
status
infer_on_tflm(const dataBlock *in, dataBlock *res) {
    // Prep the return block, needs to happen whether errors occur or not

    uint8_t *resultBuffer =
        (uint8_t *)ns_malloc(mut_cfg.config.output_length *
                             sizeof(uint8_t)); // see above for explanation of why we need malloc
    char *msg_store = (char *)ns_malloc(sizeof(char) * 30);
    res->length = mut_cfg.config.output_length * sizeof(uint8_t);
    res->dType = uint8_e;
    res->description = msg_store;
    res->cmd = generic_cmd;
    binary_t binaryBlock = {.data = (uint8_t *)resultBuffer,
                            .dataLength = mut_cfg.config.output_length * sizeof(uint8_t)};
    res->buffer = binaryBlock;

    // 'in' contains the input tensors, treat as homogeneous block
    memcpy(model_input->data.int8, in->buffer.data, in->buffer.dataLength);

    TfLiteStatus invoke_status = interpreter->Invoke();

    if (invoke_status != kTfLiteOk) {
        ns_lp_printf("Invoke failed\n");
        char error_msg[] = "Invoke failed\0";
        memcpy(msg_store, error_msg, sizeof(error_msg));
        return ns_rpc_data_failure;
    }

    if ((mut_cfg.config.profile_mut == 1) && (invokes_so_far == mut_cfg.config.profile_warmup)) {
        ns_lp_printf("[INFO] requested warmup %d,  invokes_so_far %d",
                     mut_cfg.config.profile_warmup, invokes_so_far);
        profiler->LogCsv(); // prints and also captures events in a buffer
        ns_stop_perf_profiler();
    }

    // Prep the return block with output tensor
    memcpy(resultBuffer, model_output->data.int8, mut_cfg.config.output_length);

    char res_msg[] = "Invoke Successful!\0";
    memcpy(msg_store, res_msg, sizeof(res_msg));
    ns_lp_printf(".");
    invokes_so_far++;
    return ns_rpc_data_success;
}

void
ns_preAction(void) {
    ns_lp_printf("Starting action\n");
}

void
ns_postAction(void) {
    ns_lp_printf("Stopping action\n");
}

uint8_t tflm_v_cdc_rx_ff_buf[TFLM_VALIDATOR_RX_BUFSIZE];
uint8_t tlfm_v_cdc_tx_ff_buf[TFLM_VALIDATOR_TX_BUFSIZE];

int
main(void) {
    ns_core_config_t ns_core_cfg = {.api = &ns_core_V1_0_0};

    NS_TRY(ns_core_init(&ns_core_cfg), "Core init failed.\b");
    NS_TRY(ns_power_config(&ns_development_default), "Power Init Failed\n");
    ns_itm_printf_enable();

    ns_interrupt_master_enable();

    // Add callbacks to handle incoming requests
    ns_rpc_config_t rpcConfig = {.api = &ns_rpc_gdo_V1_0_0,
                                 .mode = NS_RPC_GENERICDATA_SERVER, // Puts EVB in RPC server mode
                                 .rx_buf = tflm_v_cdc_rx_ff_buf,
                                 .rx_bufLength = TFLM_VALIDATOR_TX_BUFSIZE,
                                 .tx_buf = tlfm_v_cdc_tx_ff_buf,
                                 .tx_bufLength = TFLM_VALIDATOR_TX_BUFSIZE,
                                 .sendBlockToEVB_cb = configureModel,
                                 .fetchBlockFromEVB_cb = getStatistics,
                                 .computeOnEVB_cb = infer_on_tflm};
    NS_TRY(ns_rpc_genericDataOperations_init(&rpcConfig), "RPC Init Failed\n");

    // Add some pre/post callbacks
    // erpc_server_add_pre_cb_action(&ns_preAction);
    // erpc_server_add_post_cb_action(&ns_postAction);

    ns_lp_printf("Ready to receive RPC Calls\n");

    while (1) {
        ns_rpc_genericDataOperations_pollServer(&rpcConfig);
        ns_delay_us(1000);
        // ns_deep_sleep();
    }
}