#pragma once

/**
    The passes, as GLSL source.

        1. **sheet**    picture size, RGBA32F: height, its exact gradient and
                        the crease marks. Every facet of every layer drawn as
                        a triangle, blended additively: the layers sum.
        2. **strain**   picture size, RGBA32F, MIPMAPPED: 1/2 grad h grad h,
                        the stretch an inextensible sheet has to take up.
        3. **source**   the solve's grid (the frame mirrored to 2W x 2H): the
                        strain averaged over each cell, packed as two complex
                        fields.
        4. **fft**      forward, millpond's Stockham transform.
        5. **solve**    per mode, the least-squares in-plane displacement.
        6. **fft**      inverse: u_x, u_y.
        7. **composite** the print where the sheet pulled it, lit by the lamp,
                        with the ridges' shadows.

    Nothing is mirrored in C++: the harness checks the passes against closed
    forms -- the contraction across one straight crease, Lambert on a known
    facet, the length of a known ridge's shadow -- not against a copy of
    themselves.
*/

namespace crumple
{

extern const char* const kVertexShader;
extern const char* const kSheetVertexShader;
extern const char* const kSheetShader;
extern const char* const kStrainShader;
extern const char* const kSourceShader;
extern const char* const kFFTShader;
extern const char* const kSolveShader;
extern const char* const kCompositeShader;

} // namespace crumple
