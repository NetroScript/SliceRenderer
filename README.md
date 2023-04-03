# SliceRenderer

SliceRenderer is a tool for rendering volumes using the [CGV framework](https://github.com/sgumhold/cgv). It can quickly generate hundreds of images of the volume from different perspectives, which can be used for training a NeRF network. The tool also produces a `transforms.json` file along with the images.

You can export the volume as `.vox` file, and also export the transfer function used during volume rendering for uses in external applications.

![Screenshot](https://user-images.githubusercontent.com/18115780/229581279-8860b3d5-eb92-4827-aae5-a4bc5f7338ea.png)

## Building

To build SliceRenderer, please refer to the [CGV framework documentation](https://sgumhold.github.io/cgv/install.html) for Windows. For Linux you will need to create a Cmake configuration file according to the `config.def` file in the root directory of the project.

## Usage

To use SliceRenderer, please follow these steps:

1. Either use the default generated volume, or drag and drop a `.vox` file into the window.
2. On the right side configure your wanted resolution, and settings and apply them.
3. If you are content click on the `Generate Samples` button.

Additionally you can make use of the `Export Transfer Function` or `Export Volume` buttons to export the transfer function or volume respectively.

Additionally configuration options considering the volume rendering itself can be found inb the CGV framework documentation.

## Sample output

![image](https://user-images.githubusercontent.com/18115780/229582498-d860db3c-6bc0-412d-8bda-c3a081b2107d.png)
