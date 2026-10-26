# Settings Controller Illustrations

These original generated front-view illustrations are not gameplay captures or
extracted game artwork. The owned PNG files retain full RGBA shading and
transparency, with metadata stripped. Their native dimensions are 300 x 129 for
the handheld and 250 x 167 for the PlayStation controller.

`tools/handheld/make_settings_assets.py` decodes the PNG pixels without editing
them and reproducibly embeds lossless RGBA runs for the software menu. Native
drawing does not read image files, use a graphics API, or quantize these images
to the symbol palette. The separate flat sprites remain original generated
button/symbol artwork.
