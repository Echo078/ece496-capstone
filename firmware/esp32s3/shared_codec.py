"""Compile the existing host/firmware-independent codec without JUCE."""
from pathlib import Path

Import("env")

repo = Path(env.subst("$PROJECT_DIR")).resolve().parents[1]
controller = repo / "plugin" / "Source" / "controller"
env.Append(CPPPATH=[str(repo / "protocol"), str(controller)])
env.BuildSources(
    "$BUILD_DIR/shared_codec", str(controller), src_filter=["+<UsbFrameCodec.cpp>"]
)
