# Manual exposure correction

The stitcher does not normalize exposure automatically. Preview, final panorama, and thumbnail
rendering use each source screenshot unchanged unless the user explicitly matches exposure.

In the GUI, the user selects a target pose and one or more intersecting poses, then clicks
**Match exposure**. The stitcher estimates one scalar linear-light RGB gain from their overlap and
applies that gain to every selected pose. Corrections remain in memory for the current GUI session,
never modify source files, and are used by subsequent preview, panorama, and thumbnail renders.

The preview's optional boundaries overlay links numbered poses to their panorama coverage. Hovering
over the preview displays a magnified crop; clicks are resolved through that active crop, so the
visibly selected pixel determines the candidate poses. **Match exposure** remains disabled until a
target and at least one non-target pose are selected.

Coverage masks used by this interaction are generated at viewport resolution rather than at the
larger CUDA preview-render resolution. Generation observes the render cancellation event. Retained
mask memory is therefore proportional to the displayed viewport and frame count, not the CUDA
preview multiplier. Pillow and NumPy overlay composition runs on a latest-request-wins background
worker; Tk's event loop only creates and applies the completed `PhotoImage`. This keeps pointer,
Cancel, and Close handling responsive while boundaries are enabled.

Hard and feather blending otherwise behave normally. Auto contrast remains a separate optional
output-wide SDR operation; it is not per-pose exposure correction.
