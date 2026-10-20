#!/usr/bin/env bash
# The MCP server's test suites with g++ (or $CXX), for contributors without
# MSVC: every suite is built with the warnings the project requires and run,
# then the whole server is linked and runs --self-test, and the embedded
# resources are checked against Reloaded.Editor.Mcp/Resources. It is the same
# list as tools/test_mcp.cmd; keep the two in step.
#   tools/test_mcp.sh [-j jobs] [-k] [build dir]
# -k keeps going after a failure. Without a build dir the binaries go to a
# temporary folder that is removed afterwards. Needs bash 4.3 or later.
set -u
cd "$(dirname "$0")/.." || exit 1

jobs=$(nproc 2>/dev/null || echo 2)
keepGoing=0
while getopts "j:k" option; do
    case $option in
        j) jobs=$OPTARG ;;
        k) keepGoing=1 ;;
        *) echo "usage: tools/test_mcp.sh [-j jobs] [-k] [build dir]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -gt 0 ]; then out=$1; mkdir -p "$out" || exit 1; else out=$(mktemp -d); trap 'rm -rf "$out"' EXIT; fi

CXX=${CXX:-g++}
FLAGS=(-std=c++20 -Wall -Wextra -Wshadow -Werror -O1)
PYTHON=${PYTHON:-python3}
W=Reloaded.Editor/WorkflowModel.cpp
P=Reloaded.Editor/RecoveredPolygonImport.cpp
M=Reloaded.Editor.Mcp

# name, then the sources and flags it builds from. Run from the repository
# root: several suites read fixtures and sources relative to it.
suites=(
    "McpBridgeModelTests|tests/McpBridgeModelTests.cpp"
    "McpPlanModelTests|tests/McpPlanModelTests.cpp"
    "McpOpCoverageTests|tests/McpOpCoverageTests.cpp"
    "McpRecoverModelTests|tests/McpRecoverModelTests.cpp"
    "MapCheckJsonModelTests|tests/MapCheckJsonModelTests.cpp"
    "McpProtocolModelTests|tests/McpProtocolModelTests.cpp"
    "LevelSpecModelTests|tests/LevelSpecModelTests.cpp $W"
    "LevelSpecCompilerTests|tests/LevelSpecCompilerTests.cpp $W $P"
    "LevelRulesModelTests|tests/LevelRulesModelTests.cpp $W $P"
    "GeometricEventModelTests|tests/GeometricEventModelTests.cpp"
    "ImageCodecTests|tests/ImageCodecTests.cpp"
    "PlanRenderTests|tests/PlanRenderTests.cpp"
    "GridPlanModelTests|tests/GridPlanModelTests.cpp $W"
    "BrushImportModelTests|tests/BrushImportModelTests.cpp $W $P"
    "ImportersTests|tests/ImportersTests.cpp $W $P"
    "PromptsTests|tests/PromptsTests.cpp $W"
    "McpServerTests|tests/McpServerTests.cpp $M/McpServer.cpp $M/Resources.cpp $M/BridgeClient.cpp $M/Tools/SessionTools.cpp $M/Tools/QueryTools.cpp $M/Tools/EscapeTools.cpp $W $P -pthread"
    "McpRecoverToolsTests|tests/McpRecoverToolsTests.cpp $M/McpServer.cpp $M/Resources.cpp $M/BridgeClient.cpp $M/Tools/SessionTools.cpp $M/Tools/QueryTools.cpp $M/Tools/EscapeTools.cpp $W $P -pthread"
    "McpSpecToolsTests|tests/McpSpecToolsTests.cpp $M/Tools/SpecTools.cpp $M/Tools/IntentTools.cpp $M/Tools/ValidateTools.cpp $W $P -pthread"
    "McpMediaToolsTests|tests/McpMediaToolsTests.cpp $M/Media.cpp $M/Tools/MediaTools.cpp $M/Tools/ImportTools.cpp $W $P -pthread"
    "McpReleaseToolsTests|tests/McpReleaseToolsTests.cpp $M/Tools/ReleaseTools.cpp"
    # The server itself, from the sources Reloaded.Editor.Mcp.vcxproj lists.
    "Reloaded.Editor.Mcp|$M/main.cpp $M/McpServer.cpp $M/Resources.cpp $M/BridgeClient.cpp $M/Prompts.cpp $M/Media.cpp $M/Tools/SessionTools.cpp $M/Tools/QueryTools.cpp $M/Tools/ValidateTools.cpp $M/Tools/SpecTools.cpp $M/Tools/IntentTools.cpp $M/Tools/EscapeTools.cpp $M/Tools/ImportTools.cpp $M/Tools/MediaTools.cpp $M/Tools/ReleaseTools.cpp $W $P -pthread"
)

failures=()
fail()
{
    failures+=("$1")
    [ $keepGoing = 1 ] && return
    kill $(jobs -p) 2> /dev/null
    report
    exit 1
}
report()
{
    if [ ${#failures[@]} -eq 0 ]; then echo "test_mcp: all passed"; return 0; fi
    echo "test_mcp: FAILED: ${failures[*]}" >&2
    return 1
}

# Build in parallel, each into its own log, then run one at a time.
declare -A pids
for suite in "${suites[@]}"; do
    name=${suite%%|*}
    read -r -a sources <<< "${suite#*|}"
    while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
    "$CXX" "${FLAGS[@]}" "${sources[@]}" -o "$out/$name" > "$out/$name.build.log" 2>&1 &
    pids[$name]=$!
done
for suite in "${suites[@]}"; do
    name=${suite%%|*}
    if ! wait "${pids[$name]}"; then
        echo "--- $name: build failed" >&2
        head -n 40 "$out/$name.build.log" >&2
        fail "$name(build)"
        continue
    fi
    if [ "$name" = Reloaded.Editor.Mcp ]; then
        "$out/$name" --version > "$out/$name.run.log" 2>&1 && "$out/$name" --self-test >> "$out/$name.run.log" 2>&1
    else
        "$out/$name" > "$out/$name.run.log" 2>&1
    fi
    status=$?
    if [ $status -ne 0 ]; then
        echo "--- $name: exited $status" >&2
        tail -n 40 "$out/$name.run.log" >&2
        fail "$name"
    else
        echo "$name: $(tail -n 1 "$out/$name.run.log")"
    fi
done

"$PYTHON" tools/mcp/embed_resources.py --check || fail "embed_resources(--check)"

report
