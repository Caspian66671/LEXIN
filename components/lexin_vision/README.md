# LeXin Vision Component

This component ports the camera, ESP-WHO face detector, and lightweight
expression pipeline from `Jnassh/LeXin` commit
`723310f745c20e28e62515809bc738b7e0eefcb9`.

The LeXin application keeps ownership of LCD, touch, LVGL, networking, and
assistant behavior. This component owns only camera capture and local vision
inference, and publishes a small snapshot through `lexin_vision.h`.

Current capability:

- SC2336 MIPI-CSI capture through the ESP32-P4 Function EV Board BSP.
- ESP-WHO local face detection.
- Local lightweight expression fallback for `NEUTRAL`, `HAPPY`, and `SAD`.

The expression stage is intentionally identified as a lightweight local
classifier until a trained `expression.espdl` model is supplied.

