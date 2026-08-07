#pragma once
// Free-fly debug camera.
//
// Position is kept in *voxel units* as doubles rather than in metres as floats. At the
// edge of a 64-bit voxel world a float has a spacing of thousands of metres; a double has
// about a hundredth of a voxel, which is far below anything visible. The renderer never
// sees the absolute position — it gets the camera's chunk coordinate and a local offset,
// so the shader works in small numbers no matter where in the world you are
// (documentation/03-voxel-data-model.md §1, floating origin).
//
// This is a developer camera. The real one arrives in Stage 10, sitting between the eyes
// of an actual player model (answer D15).

#include <cmath>

#include "core/types.hpp"
#include "platform/window.hpp"

namespace ws {

class Camera {
public:
    void set_position_metres(f64 x, f64 y, f64 z) {
        x_ = x * kVoxelsPerMetre;
        y_ = y * kVoxelsPerMetre;
        z_ = z * kVoxelsPerMetre;
    }

    void set_look(f64 yaw_degrees, f64 pitch_degrees) {
        yaw_ = yaw_degrees * 3.14159265358979 / 180.0;
        pitch_ = pitch_degrees * 3.14159265358979 / 180.0;
    }

    // `wheel_for_speed` is false while a tool has claimed the wheel — the chisel takes it
    // to set its working distance. Two things silently reading the same wheel is how you
    // get a camera that doubles its speed every time you adjust a cut.
    void update(const InputState& input, f64 dt, bool mouse_look, bool wheel_for_speed = true) {
        if (mouse_look) {
            // Increasing yaw rotates toward the right vector, so moving the mouse right
            // has to increase it.
            yaw_ += static_cast<f64>(input.mouse_dx) * look_sensitivity_;
            pitch_ -= static_cast<f64>(input.mouse_dy) * look_sensitivity_;
            const f64 limit = 1.5533;   // just under 89 degrees
            pitch_ = (pitch_ > limit) ? limit : ((pitch_ < -limit) ? -limit : pitch_);
        }

        if (wheel_for_speed && input.wheel != 0.0f) {
            speed_metres_ *= std::pow(1.25, static_cast<f64>(input.wheel));
            speed_metres_ = (speed_metres_ < 0.25) ? 0.25
                                                   : ((speed_metres_ > 4000.0) ? 4000.0
                                                                               : speed_metres_);
        }

        const f64 cy = std::cos(yaw_);
        const f64 sy = std::sin(yaw_);
        const f64 cp = std::cos(pitch_);
        const f64 sp = std::sin(pitch_);

        const f64 fx = cy * cp;
        const f64 fy = sp;
        const f64 fz = sy * cp;
        const f64 rx = -sy;
        const f64 rz = cy;

        f64 forward = 0.0;
        f64 strafe = 0.0;
        f64 rise = 0.0;
        if (input.is_down(Key::W)) forward += 1.0;
        if (input.is_down(Key::S)) forward -= 1.0;
        if (input.is_down(Key::D)) strafe += 1.0;
        if (input.is_down(Key::A)) strafe -= 1.0;
        if (input.is_down(Key::Space)) rise += 1.0;
        // Down is C, not Ctrl. Ctrl is half of every editing shortcut a person already
        // knows — reaching for Ctrl+Z and sinking through the floor is not a thing anyone
        // should have to learn their way around.
        if (input.is_down(Key::C)) rise -= 1.0;

        f64 speed = speed_metres_ * kVoxelsPerMetre;
        if (input.is_down(Key::Shift)) speed *= 6.0;

        x_ += (fx * forward + rx * strafe) * speed * dt;
        y_ += (fy * forward + rise) * speed * dt;
        z_ += (fz * forward + rz * strafe) * speed * dt;
    }

    // The chunk this camera is in. The shader works relative to its corner.
    i64 chunk_x() const { return static_cast<i64>(std::floor(x_ / kChunkEdge)); }
    i64 chunk_y() const { return static_cast<i64>(std::floor(y_ / kChunkEdge)); }
    i64 chunk_z() const { return static_cast<i64>(std::floor(z_ / kChunkEdge)); }

    // Position within that chunk, in voxels. Always small, so a 32-bit float is exact
    // enough for the ray origin.
    f32 local_x() const { return static_cast<f32>(x_ - chunk_x() * f64{kChunkEdge}); }
    f32 local_y() const { return static_cast<f32>(y_ - chunk_y() * f64{kChunkEdge}); }
    f32 local_z() const { return static_cast<f32>(z_ - chunk_z() * f64{kChunkEdge}); }

    // Absolute position in voxels. Tools that touch the world work here, in doubles, and
    // convert to camera-relative only when handing something to a shader.
    f64 position_x() const { return x_; }
    f64 position_y() const { return y_; }
    f64 position_z() const { return z_; }

    i64 voxel_x() const { return static_cast<i64>(std::floor(x_)); }
    i64 voxel_y() const { return static_cast<i64>(std::floor(y_)); }
    i64 voxel_z() const { return static_cast<i64>(std::floor(z_)); }

    f64 metres_x() const { return x_ / kVoxelsPerMetre; }
    f64 metres_y() const { return y_ / kVoxelsPerMetre; }
    f64 metres_z() const { return z_ / kVoxelsPerMetre; }

    void forward_vector(f32 out[3]) const {
        const f64 cp = std::cos(pitch_);
        out[0] = static_cast<f32>(std::cos(yaw_) * cp);
        out[1] = static_cast<f32>(std::sin(pitch_));
        out[2] = static_cast<f32>(std::sin(yaw_) * cp);
    }

    void right_vector(f32 out[3]) const {
        out[0] = static_cast<f32>(-std::sin(yaw_));
        out[1] = 0.0f;
        out[2] = static_cast<f32>(std::cos(yaw_));
    }

    void up_vector(f32 out[3]) const {
        f32 f[3];
        f32 r[3];
        forward_vector(f);
        right_vector(r);
        // up = right x forward, which stays orthonormal because right has no y component.
        out[0] = r[1] * f[2] - r[2] * f[1];
        out[1] = r[2] * f[0] - r[0] * f[2];
        out[2] = r[0] * f[1] - r[1] * f[0];
    }

    f32 fov_degrees() const { return fov_degrees_; }
    void set_fov_degrees(f32 degrees) { fov_degrees_ = degrees; }
    f32 tan_half_fov() const {
        return std::tan(fov_degrees_ * 0.5f * 3.14159265f / 180.0f);
    }
    f64 speed_metres_per_second() const { return speed_metres_; }

private:
    f64 x_ = 0.0;
    f64 y_ = 0.0;
    f64 z_ = 0.0;
    f64 yaw_ = 0.0;
    f64 pitch_ = 0.0;
    f64 speed_metres_ = 12.0;
    f64 look_sensitivity_ = 0.0025;
    f32 fov_degrees_ = 90.0f;
};

}  // namespace ws
