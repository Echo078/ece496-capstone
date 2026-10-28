# protocol

The definitions of the messages between the controller firmware and the plugin.

`ControlIds.h` defines stable logical IDs; `ControllerTypes.h` defines events and
feedback payloads. `UsbProtocolV1.h` supplies portable constants/endian helpers
for the bounded CDC binary protocol, without JUCE or hardware mappings.
See [USB protocol v1](../docs/usb-protocol-v1.md) for the firmware contract and
[controller architecture](../docs/controller-architecture.md) for host semantics.
