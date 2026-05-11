# ovrtx Examples

This directory contains example projects demonstrating various features of ovrtx.

> Fork-only: this fork (`shunlin-1/ovrtx`) adds personal R&D projects
> for ovrtx-rendered Python viewers and CEF / Ultralight UI embedding.
> See [`EXPERIMENTS.md`](EXPERIMENTS.md) for the index.

## C Examples

<table>
  <tr>
    <td align="center" width="50%">
      <a href="c/minimal/">
        <img src="../img/example-minimal.jpg" alt="Minimal Example" width="100%">
      </a>
      <br>
      <b>Minimal</b>
      <br>
      <sub>Basic renderer initialization, rendering a single frame from an RGB camera and writing the result to disk as a PNG.</sub>
    </td>
    <td align="center" width="50%">
      <a href="c/vulkan-interop/">
        <img src="../img/example-vulkan-interop.gif" alt="Vulkan Interop Example" width="100%">
      </a>
      <br>
      <b>Vulkan Interop</b>
      <br>
      <sub>Demonstrates ovrtx-Vulkan interoperability, rendering USD scenes and displaying them in a Vulkan window with interactive orbit camera control.</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="c/neon-robot-c/">
        <b>Neon Robot (C)</b>
      </a>
      <br>
      <sub>Fork of Vulkan Interop that injects an animated rig of UsdLux SphereLights into the robot scene via ovrtx_add_usd with inline USDA, then drives their poses each frame with ovrtx_set_xform_mat. Live mouse-orbit window with moving neon lights.</sub>
    </td>
    <td></td>
  </tr>
</table>

## Python Examples

<table>
  <tr>
    <td align="center" width="50%">
      <a href="python/minimal/">
        <img src="../img/example-minimal.jpg" alt="Minimal Example" width="100%">
      </a>
      <br>
      <b>Minimal</b>
      <br>
      <sub>Basic workflow: create a Renderer, load a USD layer, step the renderer, and map/display the rendered output.</sub>
    </td>
    <td align="center" width="50%">
      <a href="python/planet-system/">
        <img src="../img/example-planet-system.jpg" alt="Planet System Example" width="100%">
      </a>
      <br>
      <b>Planet System</b>
      <br>
      <sub>Animated planetary system using Warp kernels for GPU-accelerated animation, demonstrating dynamic scene modification and zero-copy transform updates.</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="python/neon-robot/">
        <b>Neon Robot</b>
      </a>
      <br>
      <sub>Loads the robot-ovrtx scene and injects an animated rig of UsdLux SphereLights in saturated neon colors that orbit the robot, demonstrating per-frame light-pose updates via add_usd_layer + bind_attribute.</sub>
    </td>
    <td></td>
  </tr>
</table>
