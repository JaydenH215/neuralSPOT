/**
 * @file generic_model.h
 * @author Carlos Morales (carlos.morales@ambiq.com)
 * @brief Generic TF Model wrapper
 * @version 0.1
 * @date 2023-3-08
 *
 * @copyright Copyright (c) 2023
 *
 */

// NS includes
#include "mut_model_metadata.h"
#include "ns_ambiqsuite_harness.h"
#include "ns_debug_log.h"
#include "tflm_validator.h"

// Tensorflow Lite for Microcontroller includes (somewhat boilerplate)
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_profiler.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#ifdef NS_TF_VERSION_fecdd5d
    #include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"
#else
    #include "tensorflow/lite/micro/micro_error_reporter.h"
#endif

// TFLM model
#include "mut_model_data.h"     // Flatbuffer (weights, etc)
#include "mut_model_metadata.h" // Static metadata (arena size, etc)

// Tensorflow Globals
static tflite::ErrorReporter *error_reporter = nullptr;
static const tflite::Model *model = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *model_input = nullptr;
static TfLiteTensor *model_output = nullptr;
static tflite::MicroProfiler *profiler = nullptr;
static constexpr int kTensorArenaSize = 1024 * TFLM_VALIDATOR_ARENA_SIZE;

// static constexpr int kTensorArenaSize = 1024 * 54;
alignas(16) static uint8_t tensor_arena[kTensorArenaSize];

#ifdef NS_MLPROFILE
// Timer is used for TF profiling
ns_timer_config_t basic_tickTimer = {
    .api = &ns_timer_V1_0_0,
    .timer = NS_TIMER_COUNTER,
    .enableInterrupt = false,
};
#endif

/**
 * @brief Initialize TF with KWS model
 *
 * This code is fairly common across most TF-based models.
 * The major differences relate to input and output tensor
 * handling.
 *
 */
static int
model_init(void) {

    tflite::MicroErrorReporter micro_error_reporter;
    error_reporter = &micro_error_reporter;
#ifdef NS_MLPROFILE
    static tflite::MicroProfiler micro_profiler;
    profiler = &micro_profiler;
    NS_TRY(ns_timer_init(&basic_tickTimer), "Timer init failed.\n");
    #ifdef NS_MODEL_ANALYSIS
    ns_perf_mac_count_t basic_mac = {.number_of_layers = mut_model_number_of_estimates,
                                     .mac_count_map = mut_model_mac_estimates};
    ns_TFDebugLogInit(&basic_tickTimer, &basic_mac);
    #else
    ns_TFDebugLogInit(&basic_tickTimer, NULL);
    #endif
#else
    #ifdef NS_MLDEBUG
    ns_TFDebugLogInit(NULL, NULL);
    #endif
#endif

    tflite::InitializeTarget();

    // Map the model into a usable data structure. This doesn't involve any
    // copying or parsing, it's a very lightweight operation.
    model = tflite::GetModel(mut_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        TF_LITE_REPORT_ERROR(error_reporter,
                             "Model provided is schema version %d not equal "
                             "to supported version %d.",
                             model->version(), TFLITE_SCHEMA_VERSION);
        return -1;
    }

    static tflite::AllOpsResolver resolver;

    // Build an interpreter to run the model with.
#ifdef NS_TF_VERSION_fecdd5d
    static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena,
                                                       kTensorArenaSize, nullptr, profiler);
#else
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize, error_reporter, nullptr, profiler);
#endif
    interpreter = &static_interpreter;

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus allocate_status = interpreter->AllocateTensors();

    if (allocate_status != kTfLiteOk) {
        TF_LITE_REPORT_ERROR(error_reporter, "AllocateTensors() failed");
        ns_lp_printf("[ERROR] AllocateTensors() failed\n");
        mut_stats.stats.computed_arena_size = 0xDEADBEEF;
        return -1;
    }

    mut_stats.stats.computed_arena_size =
        interpreter->arena_used_bytes(); // prep to send back to PC

    // Obtain pointers to the model's input and output tensors.
    model_input = interpreter->input(0);
    model_output = interpreter->output(0);
    return 0;
}