// Two ways of being in a clip, and the walking one is the one with opinions.
//
// Orbit is a turntable: the clip sits still and you go round it. That is what you want for a
// fragment somebody has just changed, because the question is "what shape is it now".
//
// Walk puts you inside at eye height with gravity, a body that fits through a door and legs that
// find a stair, because the facility's brief says it has to be walkable and that is not a claim a
// screenshot can settle. The numbers are the game's own where the game has them: 1.62 m eye
// height, a 0.6 m body, an 0.18 m riser to climb.
//
// Fly is walk with the gravity and the body switched off, and the up button turns it on by being
// tapped twice — the same gesture as everywhere else that has one.

import { boxHitsMatter, solidAt } from './format.js';

const EYE_STANDING = 1.62;
const EYE_CROUCHED = 1.10;
const BODY_STANDING = 1.80;
const BODY_CROUCHED = 1.20;
const RADIUS = 0.30;
const GRAVITY = 24.0;
const JUMP = 7.0;             // about 1.02 m, which clears a 0.9 m parapet from a run-up
const WALK_SPEED = 4.4;
const CROUCH_SPEED = 1.9;
const FLY_SPEED = 14.0;
const STEP_HEIGHT = 0.55;     // three of the building's 0.18 m risers, so stairs are walked up
const STEP_TRY = 0.05;

// The slice is honoured by the body as well as by the eye. Cutting the front off a building and
// then walking into the wall that is no longer drawn is the kind of thing that makes a viewer feel
// broken, and clamping the query box to the kept side is exact rather than approximate.
function hits(clip, plane, x, y, z, height) {
    let lowX = x - RADIUS, lowY = y, lowZ = z - RADIUS;
    let highX = x + RADIUS, highY = y + height, highZ = z + RADIUS;
    if (plane) {
        const low = [lowX, lowY, lowZ];
        const high = [highX, highY, highZ];
        if (plane.sign > 0) {
            high[plane.axis] = Math.min(high[plane.axis], plane.at);
        } else {
            low[plane.axis] = Math.max(low[plane.axis], plane.at);
        }
        if (low[plane.axis] >= high[plane.axis]) return false;
        [lowX, lowY, lowZ] = low;
        [highX, highY, highZ] = high;
    }
    return boxHitsMatter(clip, lowX, lowY, lowZ, highX, highY, highZ);
}

export class Controls {
    constructor() {
        this.mode = 'orbit';

        this.yaw = 0;
        this.pitch = -0.25;
        this.distance = 30;
        this.target = [0, 0, 0];

        this.position = [0, 0, 0];    // the feet, in walk mode
        this.velocity = [0, 0, 0];
        this.onGround = false;
        this.flying = false;
        this.crouching = false;

        this.move = { x: 0, y: 0 };   // the joystick, -1 to 1
        this.up = false;
        this.down = false;
        this.jumpQueued = false;
        this.keys = new Set();

        this.eye = [0, 0, 0];
        this.at = [0, 0, 1];
        this.fov = 1.15;
        this.near = 0.04;
        this.far = 400;
    }

    // The camera is put where all eight corners of the matter's box are just inside the frustum,
    // which is a closed form and not a search: a corner at `c` from the target needs
    // `d >= |sideways| / tan(half fov) - depth`, and the answer is the largest of the sixteen
    // numbers that gives.
    //
    // Two simpler versions were both wrong and in opposite directions. The widest single dimension
    // puts the camera INSIDE a clip that is wide in all three — the rotunda is 12.6 m across and
    // 11.6 m tall, and its own width put the eye a metre inside its own wall. The bounding sphere
    // never does that and is far too generous for a building, which is a slab: the facility came
    // out a third of the height of a phone screen with sky all round it. Fitting the box itself
    // costs eight dot products and is right for both.
    frame(clip, aspect) {
        this.target = clip.centre.slice();
        this.yaw = Math.PI;           // looking north, at the elevation the building is judged from
        this.pitch = -0.22;

        const cp = Math.cos(this.pitch);
        const away = [Math.sin(this.yaw) * cp, -Math.sin(this.pitch), Math.cos(this.yaw) * cp];
        const forward = away.map((v) => -v);
        let right = [forward[2], 0, -forward[0]];
        const span = Math.hypot(right[0], right[2]) || 1;
        right = [right[0] / span, 0, right[2] / span];
        const up = [
            right[1] * forward[2] - right[2] * forward[1],
            right[2] * forward[0] - right[0] * forward[2],
            right[0] * forward[1] - right[1] * forward[0],
        ];

        const tanVertical = Math.tan(this.fov * 0.5);
        const tanHorizontal = tanVertical * (aspect || 1);
        let distance = 0;
        for (let corner = 0; corner < 8; ++corner) {
            const c = [
                ((corner & 1) ? clip.high[0] : clip.low[0]) - this.target[0],
                ((corner & 2) ? clip.high[1] : clip.low[1]) - this.target[1],
                ((corner & 4) ? clip.high[2] : clip.low[2]) - this.target[2],
            ];
            const depth = c[0] * forward[0] + c[1] * forward[1] + c[2] * forward[2];
            const across = Math.abs(c[0] * right[0] + c[1] * right[1] + c[2] * right[2]);
            const above = Math.abs(c[0] * up[0] + c[1] * up[1] + c[2] * up[2]);
            distance = Math.max(distance, across / tanHorizontal - depth, above / tanVertical - depth);
        }
        this.distance = distance * 1.06;
        this.far = Math.max(200, clip.reach * 8);
    }

    // Stand outside the south face at ground level and look into the clip. For the facility that
    // is on the lawn below the great steps, facing the portico, which is the way in.
    spawn(clip) {
        const x = (clip.low[0] + clip.high[0]) * 0.5;
        const z = clip.low[2] + 1.2;
        const step = 1 / clip.collisionMetre;
        let y = clip.low[1];
        let footing = false;
        for (let probe = clip.high[1]; probe > clip.origin[1]; probe -= step) {
            if (solidAt(clip, x, probe, z)) {
                y = probe + step;
                footing = true;
                break;
            }
        }

        this.position = [x, y, z];
        this.velocity = [0, 0, 0];
        // Some clips have no ground at all — a pane hanging in the air, a single sphere. Standing
        // on nothing there is a fall to the bottom of the box and a respawn to fall again, so the
        // only sensible way to be inside one of those is flying, and that is what it does.
        this.flying = !footing;
        // Zero, not π. In orbit the yaw is where the CAMERA stands and π puts it south of the
        // clip looking north; walking, the yaw is which way the body FACES, and the same number
        // spawns you on the lawn with your back to the building.
        this.yaw = 0;
        this.pitch = -0.05;
        this.onGround = footing;
        this.crouching = false;
    }

    height() {
        return this.crouching ? BODY_CROUCHED : BODY_STANDING;
    }

    // The joystick and the keyboard say the same thing, so they are added and clamped rather than
    // one winning: a phone with a bluetooth keyboard should not have to choose.
    intent() {
        let x = this.move.x;
        let z = this.move.y;
        if (this.keys.has('KeyW') || this.keys.has('ArrowUp')) z += 1;
        if (this.keys.has('KeyS') || this.keys.has('ArrowDown')) z -= 1;
        if (this.keys.has('KeyD') || this.keys.has('ArrowRight')) x += 1;
        if (this.keys.has('KeyA') || this.keys.has('ArrowLeft')) x -= 1;
        const length = Math.hypot(x, z);
        if (length > 1) { x /= length; z /= length; }
        return { x, z };
    }

    update(clip, dt) {
        if (dt > 0.1) dt = 0.1;   // a tab that was in the background does not teleport anybody
        if (this.mode === 'orbit') {
            this.pitch = Math.max(-1.5, Math.min(1.5, this.pitch));
            this.distance = Math.max(clip ? clip.reach * 0.03 : 0.5, this.distance);
            const cp = Math.cos(this.pitch);
            this.eye = [
                this.target[0] + Math.sin(this.yaw) * cp * this.distance,
                this.target[1] - Math.sin(this.pitch) * this.distance,
                this.target[2] + Math.cos(this.yaw) * cp * this.distance,
            ];
            this.at = this.target.slice();
            return;
        }

        this.pitch = Math.max(-1.55, Math.min(1.55, this.pitch));
        const want = this.intent();
        const forward = [Math.sin(this.yaw), 0, Math.cos(this.yaw)];
        // Screen-right is `forward x up`, and getting its sign wrong strafes you the wrong way.
        // Facing north (yaw 0, forward +Z) with up +Y, that cross product is -X, which is what the
        // view matrix's own right column holds -- so this is the same vector the camera uses and
        // not a second guess at it. It was written negated and D and A were swapped.
        const right = [-forward[2], 0, forward[0]];

        if (this.flying) {
            const lift = (this.up ? 1 : 0) - (this.down ? 1 : 0);
            const rise = Math.sin(this.pitch);
            // Looking up and pushing forward flies up, which is what anybody who has used a noclip
            // expects, and the buttons still give pure vertical on top of it.
            const speed = FLY_SPEED * (this.keys.has('ShiftLeft') ? 2.5 : 1);
            this.position[0] += (forward[0] * want.z + right[0] * want.x) * speed * dt;
            this.position[2] += (forward[2] * want.z + right[2] * want.x) * speed * dt;
            this.position[1] += (rise * want.z + lift) * speed * dt;
            this.velocity = [0, 0, 0];
        } else {
            const speed = this.crouching ? CROUCH_SPEED : WALK_SPEED;
            const wishX = (forward[0] * want.z + right[0] * want.x) * speed;
            const wishZ = (forward[2] * want.z + right[2] * want.x) * speed;

            this.velocity[1] -= GRAVITY * dt;
            if (this.jumpQueued && this.onGround) {
                this.velocity[1] = JUMP;
                this.onGround = false;
            }
            this.jumpQueued = false;

            const height = this.height();
            const p = this.position;

            // One axis at a time, so a body sliding along a wall keeps the component that is not
            // into it. Vertical last, so a jump that clips a ceiling still moves sideways.
            const before = p[1];
            p[0] += wishX * dt;
            if (clip && hits(clip, this.plane, p[0], p[1], p[2], height)) {
                if (!this.stepUp(clip, height, before)) p[0] -= wishX * dt;
            }
            p[2] += wishZ * dt;
            if (clip && hits(clip, this.plane, p[0], p[1], p[2], height)) {
                if (!this.stepUp(clip, height, before)) p[2] -= wishZ * dt;
            }

            const dy = this.velocity[1] * dt;
            p[1] += dy;
            this.onGround = false;
            if (clip && hits(clip, this.plane, p[0], p[1], p[2], height)) {
                p[1] -= dy;
                if (this.velocity[1] < 0) this.onGround = true;
                this.velocity[1] = 0;
                // Settle onto the surface rather than hovering up to a cell above it.
                const settle = 1 / (clip.collisionMetre * 4);
                for (let i = 0; i < 8; ++i) {
                    p[1] -= settle;
                    if (hits(clip, this.plane, p[0], p[1], p[2], height)) {
                        p[1] += settle;
                        break;
                    }
                }
            }

            // Below the clip entirely: there is no world down there, so put them back on top of
            // whatever they walked off rather than letting them fall forever.
            if (clip && p[1] < clip.origin[1] - 40) this.spawn(clip);
        }

        const eyeHeight = this.crouching ? EYE_CROUCHED : EYE_STANDING;
        this.eye = [this.position[0], this.position[1] + eyeHeight, this.position[2]];
        const cp = Math.cos(this.pitch);
        this.at = [
            this.eye[0] + Math.sin(this.yaw) * cp,
            this.eye[1] + Math.sin(this.pitch),
            this.eye[2] + Math.cos(this.yaw) * cp,
        ];
    }

    // A stair is not an obstacle. Raise the body a little and see whether the move it just refused
    // becomes possible; three risers is as far as it will reach, which climbs every stair in the
    // building and does not climb its walls.
    stepUp(clip, height, floor) {
        if (!this.onGround && this.velocity[1] < -0.5) return false;
        const p = this.position;
        for (let lift = STEP_TRY; lift <= STEP_HEIGHT; lift += STEP_TRY) {
            if (!hits(clip, this.plane, p[0], floor + lift, p[2], height)) {
                p[1] = floor + lift;
                return true;
            }
        }
        return false;
    }

    // Crouching under something and then standing up has to be refused, or a player ends up inside
    // a coffer with the camera in the stone.
    toggleCrouch(clip, wanted) {
        if (!wanted && this.crouching && clip) {
            const p = this.position;
            if (hits(clip, this.plane, p[0], p[1], p[2], BODY_STANDING)) return;
        }
        this.crouching = wanted;
    }
}
