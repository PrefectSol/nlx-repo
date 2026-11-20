#!/usr/bin/env python3
import argparse
import struct
import sys

import numpy as np

ACTIVATIONS = {"identity": 0, "relu": 1, "tanh": 2, "sigmoid": 3}


def tensor(state, key):
    value = state[key]
    value = value.detach().cpu().numpy() if hasattr(value, "detach") else np.asarray(value)
    return np.ascontiguousarray(value, dtype="<f4")


def collect(state, gru, dense):
    layers = 0
    while f"{gru}.weight_ih_l{layers}" in state:
        layers += 1

    if layers == 0:
        raise SystemExit(f"no GRU weights under '{gru}.weight_ih_l0'")

    if any(key.startswith(f"{gru}.") and key.endswith("_reverse") for key in state):
        raise SystemExit("bidirectional GRU is not supported")

    recurrent = []
    for layer in range(layers):
        recurrent.append([tensor(state, f"{gru}.{name}_l{layer}") for name in ("weight_ih", "weight_hh", "bias_ih", "bias_hh")])

    hidden = recurrent[0][1].shape[1]
    inputs = recurrent[0][0].shape[1]

    head = []
    width = hidden
    for spec in dense:
        prefix, _, activation = spec.partition(":")
        weight = tensor(state, f"{prefix}.weight")
        bias = tensor(state, f"{prefix}.bias") if f"{prefix}.bias" in state else np.zeros(weight.shape[0], "<f4")

        if weight.shape[1] != width:
            raise SystemExit(f"{prefix}: expects {weight.shape[1]} inputs, previous layer gives {width}")

        head.append((weight, bias, ACTIVATIONS[activation or "identity"]))
        width = weight.shape[0]

    return inputs, hidden, recurrent, head


def write(path, inputs, hidden, recurrent, head):
    with open(path, "wb") as file:
        file.write(b"NLXM")
        file.write(struct.pack("<5I", 1, inputs, hidden, len(recurrent), len(head)))
        file.write(struct.pack(f"<{len(head)}I", *[weight.shape[0] for weight, _, _ in head]))
        file.write(struct.pack(f"<{len(head)}I", *[kind for _, _, kind in head]))
        for tensors in recurrent:
            for value in tensors:
                file.write(value.tobytes())
        for weight, bias, _ in head:
            file.write(weight.tobytes())
            file.write(bias.tobytes())


def main():
    parser = argparse.ArgumentParser(description="PyTorch GRU + MLP checkpoint -> NLXM (static shapes, float32, PyTorch gate order r, z, n)")
    parser.add_argument("checkpoint", help="torch.save(model.state_dict()) file, or a whole model")
    parser.add_argument("output", help="model.nlxm, seal it afterwards: nlx_seal seal model.nlxm models/model.nlxe .env")
    parser.add_argument("--gru", default="gru", help="state_dict prefix of nn.GRU (batch_first does not matter)")
    parser.add_argument("--dense", required=True, help="comma separated Linear prefixes with activations, e.g. head.0:relu,head.2:relu,head.4")
    arguments = parser.parse_args()

    import torch

    loaded = torch.load(arguments.checkpoint, map_location="cpu", weights_only=False)
    state = loaded.state_dict() if hasattr(loaded, "state_dict") else loaded

    inputs, hidden, recurrent, head = collect(state, arguments.gru, arguments.dense.split(","))
    write(arguments.output, inputs, hidden, recurrent, head)

    parameters = sum(value.size for tensors in recurrent for value in tensors) + sum(weight.size + bias.size for weight, bias, _ in head)
    print(f"input = {inputs}, hidden = {hidden}, layers = {len(recurrent)}, dense = {[weight.shape[0] for weight, _, _ in head]}, parameters = {parameters}")


if __name__ == "__main__":
    sys.exit(main())
