#pragma once

namespace monitour::inference {

struct HeadPose {
    float yaw{0.f};     // degrees, + = looking right
    float pitch{0.f};   // degrees, + = looking up
    float roll{0.f};    // degrees, + = tilt right
    float confidence{0.f};
};

}  // namespace monitour::inference
