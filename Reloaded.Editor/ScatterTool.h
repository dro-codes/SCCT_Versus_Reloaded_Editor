#pragma once
#include <windows.h>

// RE+ Tools > Scatter Props...: fills a room with clutter. A list of static
// meshes (the Static Mesh Browser's current one, or a favourite), each with a
// weight; a count or a density; the builder brush's footprint or a circle
// round the last viewport click; random yaw (or a full random turn), a scale
// range, the least spacing between props and an option to sit them flat on a
// slope; a seed and Re-roll. Each prop is dropped onto the floor with Drop to
// Floor's rays and left out where it would sit in a wall or on another prop.
// The viewports preview where they go; Place adds them all as one Undo step,
// in the group Scatter. ScatterModel.h holds the sampling and spacing,
// ScatterNative.inl the editor side.
namespace ScatterTool
{
    // Reserved block 41500-41519.
    constexpr UINT kOpen = 41500;

    // From the frame's command dispatcher. True when the command was ours.
    bool HandleCommand(UINT command);
}
