# OpenVINO&trade; Model Server fork of [MediaPipe](https://google.github.io/mediapipe/).
This repository is allowing users to take advantage of OpenVINO&trade; in mediapipe framework. During the graph execution the inference calls on pretrained models are directed to OpenVINO&trade; Model Server instance initialized and parametrized by the specific inference calculators.

We show in several demos the full required configuration but any graph with proper adjustements can run with OpenVINO&trade; backend.

# List of main features
- inference calculators that allow the use of OpenVINO&trade; backend for effective inference inside mediapipe framework.

# List of changes introduced in this repository fork

- added Dockerfile.openvino dockerfile that creates runtime and development environment.
- added Makefile file with build, test and demo targets for the ease of use.
- modified build_desktop_examples.sh script to build new demos.
- added [calculators](mediapipe/calculators/ovms)  for OpenVINO&trade; inference in mediapipe graphs
  detailed description can be found here (TODO link).
- modified [targets](mediapipe/examples/desktop)  to use OpenVINO&trade; inference calculators (the list of available dmeos is in the table below).
- modified WORKSPACE file to add OpenVINO&trade; Model Server dependencies.
  Specifically target @ovms//src:ovms_lib as dependency from [OVMS](https://github.com/openvinotoolkit/model_server)
- modified [graphs and targets](mediapipe/modules/) to use OpenVINO&trade; inference instead of TensorFlow inference in examples/desktop targets.
- added setup_ovms.py script to create models directory setup used in OpenVINO&trade; inference
- modified setup_opecv.py to install 4.7.0 OpenCV version instead of previous 3.4.

[]() OpenVINO&trade; demo                                                    | C++                                                     | Python                                                        | Original Google demo                                                        |
:---------------------------------------------------------------------------------------- | :-----------------------------------------------------: | :-----------------------------------------------------------: | :---------------------------------------------------------------------------------------: |
[Face Detection](mediapipe/examples/desktop/face_detection/README.md)                     | ✅                                                      |                                                                |[Face Detection](https://google.github.io/mediapipe/solutions/face_detection)             |
[Iris](mediapipe/examples/desktop/iris_tracking/README.md)                                | ✅                                                      |                                                                |[Iris](https://google.github.io/mediapipe/solutions/iris)                                 |
[Pose](mediapipe/examples/desktop/pose_tracking/README.md)                                | ✅                                                      |                                                                |[Pose](https://google.github.io/mediapipe/solutions/pose)                                 |
[Holistic](mediapipe/examples/desktop/holistic_tracking/README.md)                        | ✅                                                      |                                                                |[Holistic](https://google.github.io/mediapipe/solutions/holistic)                         |
[Object Detection](mediapipe/examples/desktop/object_detection/README.md)                 | ✅                                                      |                                                                |[Object Detection](https://google.github.io/mediapipe/solutions/object_detection)         |

# Development instructions

The list of instructions and best practises in developing your own application and graphs can be found [here](docs/development.md)

# Deployment fast truck instructions

The list of instructions and best practises in deploying the applications and graphs can be found [here](docs/deployment.md)

![MediaPipe](https://mediapipe.dev/images/mediapipe_small.png)

--------------------------------------------------------------------------------

## Live ML anywhere

[MediaPipe](https://google.github.io/mediapipe/) offers cross-platform, customizable
ML solutions for live and streaming media.

![accelerated.png](https://mediapipe.dev/images/accelerated_small.png)                                                               | ![cross_platform.png](https://mediapipe.dev/images/cross_platform_small.png)
:------------------------------------------------------------------------------------------------------------: | :----------------------------------------------------:
***End-to-End acceleration***: *Built-in fast ML inference and processing accelerated even on common hardware* | ***Build once, deploy anywhere***: *Unified solution works across Android, iOS, desktop/cloud, web and IoT*
![ready_to_use.png](https://mediapipe.dev/images/ready_to_use_small.png)                                                             | ![open_source.png](https://mediapipe.dev/images/open_source_small.png)
***Ready-to-use solutions***: *Cutting-edge ML solutions demonstrating full power of the framework*            | ***Free and open source***: *Framework and solutions both under Apache 2.0, fully extensible and customizable*

## ML solutions in MediaPipe OpenVINO&trade; fork

Face Detection                                                                                                                 | Iris                                                                                                      | Pose                                                                                                      | Holistic                                                                          | Object Detection                                              |
:----------------------------------------------------------------------------------------------------------------------------: | :-------------------------------------------------------------------------------------------------------: | :-------------------------------------------------------------------------------------------------------: | :------: | :----------------------------------------------------------------------------------------------------------------------------------: |
[![face_detection](https://mediapipe.dev/images/mobile/face_detection_android_gpu_small.gif)](https://google.github.io/mediapipe/solutions/face_detection) | [![iris](https://mediapipe.dev/images/mobile/iris_tracking_android_gpu_small.gif)](https://google.github.io/mediapipe/solutions/iris) | [![pose](https://mediapipe.dev/images/mobile/pose_tracking_android_gpu_small.gif)](https://google.github.io/mediapipe/solutions/pose) | [![holistic_tracking](https://mediapipe.dev/images/mobile/holistic_tracking_android_gpu_small.gif)](https://google.github.io/mediapipe/solutions/holistic) | [![object_detection](https://mediapipe.dev/images/mobile/object_detection_android_gpu_small.gif)](https://google.github.io/mediapipe/solutions/object_detection) |

## Fork baseline
The fork is based on original mediapipe release origin/v0.9.1 (Author: Sebastian Schmidt <mrschmidt@google.com> Date:   Tue Jan 24 12:11:53 2023).

Original v0.9.1 Google ML solutions in MediaPipe can be found [here](https://github.com/google/mediapipe/tree/v0.9.1)