import json
import logging as log
import pickle

import numpy as np
import pydantic_argparse
from autodeploy.gen_library import generateModelLib
from autodeploy.measure_power import generatePowerBinary, measurePower
from autodeploy.validator import (
    ModelConfiguration,
    configModel,
    create_validation_binary,
    get_interpreter,
    getModelStats,
    printStats,
    validateModel,
)
from ns_utils import ModelDetails, reset_dut, rpc_connect_as_client
from pydantic import BaseModel, Field


class Params(BaseModel):
    # General Parameters
    seed: int = Field(42, description="Random Seed")
    create: bool = Field(
        True,
        description="Create source files based on TFlite file",
    )
    onboard_perf: bool = Field(
        True, description="Capture and print performance measurements on EVB"
    )
    cpu_mode: str = Field(
        "NS_MAXIMUM_PERF", description="CPU mode to use for performance measurements"
    )
    joulescope: bool = Field(False, description="Use Joulescope for power measurements")
    create_ambiqsuite_example: bool = Field(
        True, description="Create AmbiqSuite example based on TFlite file"
    )

    tflite_filename: str = Field(
        "model.tflite", description="Name of tflite model to be analyzed"
    )

    configfile: str = Field(
        "", description="Optional configuration file for parameters"
    )

    max_arena_size: int = Field(
        100, description="Maximum KB to be allocated for TF arena"
    )
    arena_size_scratch_buffer_padding: int = Field(
        0,
        description="(TFLM Workaround) Padding to be added to arena size to account for scratch buffer (in KB)",
    )
    max_rpc_buf_size: int = Field(
        4096, description="Maximum bytes to be allocated for RPC RX and TX buffers"
    )
    resource_variable_count: int = Field(
        0,
        description="Maximum ResourceVariables needed by model (typically used by RNNs)",
    )
    max_profile_events: int = Field(
        40,
        description="Maximum number of events (layers) captured during model profiling",
    )

    dataset: str = Field("dataset.pkl", description="Name of dataset")
    random_data: bool = Field(True, description="Use random data")

    runs_power: int = Field(
        100, description="Number of inferences to run for power measurement"
    )
    model_name: str = Field(
        "model", description="Name of model to be used in generated library"
    )
    working_directory: str = Field(
        "../projects/autodeploy",
        description="Directory where generated library will be placed",
    )

    # Logging Parameters
    verbosity: int = Field(1, description="Verbosity level (0-4)")

    # RPC Parameters
    tty: str = Field("/dev/tty.usbmodem1234561", description="Serial device")
    baud: str = Field("115200", description="Baud rate")


def create_parser():
    """Create CLI argument parser
    Returns:
        ArgumentParser: Arg parser
    """
    return pydantic_argparse.ArgumentParser(
        model=Params,
        prog="Measure performance of TFLite model running on EVB",
        description="Characterize TFLite model",
    )


class adResults:
    def __init__(self, p) -> None:
        self.powerMaxPerfInferenceTime = 0
        self.powerMinPerfInferenceTime = 0
        self.powerMaxPerfJoules = 0
        self.powerMinPerfJoules = 0
        self.powerMaxPerfWatts = 0
        self.powerMinPerfWatts = 0
        self.powerIterations = p.runs_power
        self.model_name = p.model_name

    def print(self):
        print("")
        print((f"Joulescope-based Report for {self.model_name}:"))

        print(
            f"[Power]   Max Perf Inference Time (ms):      {self.powerMaxPerfInferenceTime:0.3f}"
        )
        print(
            f"[Power]   Max Perf Inference Energy (uJ):    {self.powerMaxPerfJoules:0.3f}"
        )
        print(
            f"[Power]   Max Perf Inference Avg Power (mW): {self.powerMaxPerfWatts:0.3f}"
        )
        print(
            f"[Power]   Min Perf Inference Time (ms):      {self.powerMinPerfInferenceTime:0.3f}"
        )
        print(
            f"[Power]   Min Perf Inference Energy (uJ):    {self.powerMinPerfJoules:0.3f}"
        )
        print(
            f"[Power]   Min Perf Inference Avg Power (mW): {self.powerMinPerfWatts:0.3f}"
        )
        print(
            f"""
Notes:
        - MACs are estimated based on the number of operations in the model, not via instrumented code
"""
        )

    def setPower(self, cpu_mode, mSeconds, uJoules, mWatts):
        if cpu_mode == "NS_MINIMUM_PERF":
            self.powerMinPerfInferenceTime = mSeconds
            self.powerMinPerfJoules = uJoules
            self.powerMinPerfWatts = mWatts
        else:
            self.powerMaxPerfInferenceTime = mSeconds
            self.powerMaxPerfJoules = uJoules
            self.powerMaxPerfWatts = mWatts


if __name__ == "__main__":
    # parse cmd parameters
    parser = create_parser()
    cli_params = parser.parse_typed_args()

    # override parameters from config file
    json_params = cli_params.json()
    dict_params = json.loads(json_params)
    if cli_params.configfile:
        with open(cli_params.configfile, "r") as f:
            config = json.load(f)
            dict_params.update(config)

    params = Params(**dict_params)

    results = adResults(params)

    print("")  # put a blank line between obnoxious TF output and our output

    # set logging level
    log.basicConfig(
        level=log.DEBUG
        if params.verbosity > 2
        else log.INFO
        if params.verbosity > 1
        else log.WARNING,
        format="%(levelname)s: %(message)s",
    )

    interpreter = get_interpreter(params)

    mc = ModelConfiguration(params)
    md = ModelDetails(interpreter)

    print(mc.modelStructureDetails.overallMacEstimates)

    print("")
    print(f"*** Characterize inference energy consumption on EVB using Joulescope")

    if params.onboard_perf:
        generatePowerBinary(params, mc, md, params.cpu_mode)
        print(
            f"{params.cpu_mode} Performance code flashed to EVB - connect to SWO and press reset to see results."
        )

    if params.joulescope:
        for cpu_mode in ["NS_MINIMUM_PERF", "NS_MAXIMUM_PERF"]:
            generatePowerBinary(params, mc, md, cpu_mode)
            td, i, v, p, c, e = measurePower()
            energy = (e["value"] / params.runs_power) * 1000000  # Joules
            t = (td.total_seconds() * 1000) / params.runs_power
            w = (e["value"] / td.total_seconds()) * 1000
            results.setPower(cpu_mode=cpu_mode, mSeconds=t, uJoules=energy, mWatts=w)
            log.info(
                f"Model Power Measurement in {cpu_mode} mode: {t:.3f} ms and {energy:.3f} uJ per inference (avg {w:.3f} mW))"
            )
            results.print()

    # if params.create_library:
    #     print("")
    #     print(f"*** Stage [{stage}/{total_stages}]: Generate minimal static library")
    #     generateModelLib(params, mc, md, ambiqsuite=False)
    #     stage += 1

    # if params.create_ambiqsuite_example:
    #     print("")
    #     print(f"*** Stage [{stage}/{total_stages}]: Generate AmbiqSuite Example")
    #     generateModelLib(params, mc, md, ambiqsuite=True)
    #     stage += 1
