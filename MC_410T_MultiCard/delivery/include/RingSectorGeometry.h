#pragma once

namespace ring_sector {
inline constexpr double kNineOClockDeg = 180.0;

// Positive reconstruction angles go clockwise on screen (rows increase downwards).
// Keep the protocol's sectorCcw name: its numeric meaning is the sign of the step.
inline constexpr int directionSign(int sectorCcw)
{
    return sectorCcw ? 1 : -1;
}

inline constexpr double scanStartDeg(double firstDeg, double widthDeg, int rank, int sectorCcw)
{
    return firstDeg + directionSign(sectorCcw) * rank * widthDeg;
}

// CUDA masks take the lower boundary in the positive-angle direction, even when
// acquisition enters the sector at its upper boundary and proceeds backwards.
inline constexpr double maskStartDeg(double firstDeg, double widthDeg, int rank, int sectorCcw)
{
    return firstDeg + (sectorCcw ? rank : -rank - 1) * widthDeg;
}
}
