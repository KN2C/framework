/***************************************************************************
 *   Copyright 2015 Michael Eischer, Jan Kallwies, Philipp Nordhus         *
 *   Robotics Erlangen e.V.                                                *
 *   http://www.robotics-erlangen.de/                                      *
 *   info@robotics-erlangen.de                                             *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   any later version.                                                    *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "core/rng.h"
#include "mesh.h"
#include "protobuf/ssl_detection.pb.h"
#include "simball.h"
#include "simrobot.h"
#include "simulator.h"
#include <cmath>

SimRobot::SimRobot(RNG *rng, const robot::Specs &specs, btDiscreteDynamicsWorld *world, const btVector3 &pos, float dir) :
    m_rng(rng),
    m_specs(specs),
    m_world(world),
    m_charge(false),
    m_isCharged(false),
    m_shootTime(0.0),
    m_commandTime(0.0),
    error_sum_v_s(0),
    error_sum_v_f(0),
    error_sum_omega(0)
{
    btConvexHullShape* shape = new btConvexHullShape;
    m_shape = shape;

    // setup robot collision shape
    Mesh mesh;
    // subtract collision margin from dimensions;
    mesh.createRobotMesh(m_specs.radius() - COLLISION_MARGIN / SIMULATOR_SCALE,
                         m_specs.height() - 2 * COLLISION_MARGIN / SIMULATOR_SCALE, m_specs.angle());
    foreach (const QVector3D& v, mesh.hull()) {
        shape->addPoint(btVector3(v.x(), v.y(), v.z()) * SIMULATOR_SCALE);
    }

    btVector3 localInertia(0,0,0);
    m_shape->calculateLocalInertia(m_specs.mass(), localInertia);

    btTransform startWorldTransform;
    startWorldTransform.setIdentity();
    startWorldTransform.setOrigin(btVector3(pos.x(), pos.y(), m_specs.height() / 2.0f) * SIMULATOR_SCALE);
    startWorldTransform.setRotation(btQuaternion(btVector3(0, 0, 1), dir - M_PI_2));
    m_motionState = new btDefaultMotionState(startWorldTransform);

    // set robot dynamics and move to start position
    btRigidBody::btRigidBodyConstructionInfo rbInfo(m_specs.mass(), m_motionState, m_shape, localInertia);

    m_body = new btRigidBody(rbInfo);
    // see simulator.cpp
    m_body->setRestitution(0.6f);
    m_body->setFriction(0.22f);
    m_world->addRigidBody(m_body);

    // FIXME wheels currently aren't used
    const float angles[] = { 35.5, 135, 225, 324.5 };
    m_wheels.reserve(4);
    for (int i = 0; i < 4; i++){
        const float angle = (90 + angles[i]) / 180 * M_PI;
        Wheel wheel;
        wheel.angle = angle;
        wheel.pos = btVector3(cos(angle), sin(angle), 0.0f) * m_specs.radius() * SIMULATOR_SCALE;
        wheel.dir = btVector3(-sin(angle), cos(angle), 0.0f);
        m_wheels.append(wheel);
    }
}

SimRobot::~SimRobot()
{
    m_world->removeRigidBody(m_body);
    delete m_body;
    delete m_shape;
    delete m_motionState;
}

void SimRobot::begin(SimBall *ball, double time)
{
    m_commandTime += time;

    // after 0.1s without new command reset to stop
    if (m_commandTime > 0.1) {
        m_command.Clear();
    }

    if (m_move.has_p_x()) {
        if (m_move.position()) {
            // set the robot to the given position and speed
            const btVector3 pos(m_move.p_x(), m_move.p_y(), m_specs.height() / 2.0f);
            const btQuaternion rot = btQuaternion(btVector3(0, 0, 1), m_move.phi() - M_PI_2);
            m_body->setWorldTransform(btTransform(rot, pos * SIMULATOR_SCALE));

            const btVector3 linVel(m_move.v_x(), m_move.v_y(), 0.0f);
            m_body->setLinearVelocity(linVel * SIMULATOR_SCALE);

            const btVector3 angVel(m_move.omega(), 0.0f, 0.0f);
            m_body->setAngularVelocity(angVel);
            m_body->activate();
            m_body->setDamping(0.0, 0.0);
            m_move.Clear(); // clear move command
            // reset is neccessary, as the command is only sent once
            // without one canceling it
        } else {
            // move robot by hand
            btVector3 force(m_move.p_x(), m_move.p_y(), 0.0f);
            force = force - m_body->getWorldTransform().getOrigin() / SIMULATOR_SCALE;
            force.setZ(0.0f);
            m_body->activate();
            m_body->applyCentralImpulse(force * m_specs.mass() * (1./6) * SIMULATOR_SCALE);
            m_body->setDamping(0.99, 0.99);
        }
        return;
    }

    m_body->setDamping(0.7, 0.8);

    btTransform t = m_body->getWorldTransform();
    t.setOrigin(btVector3(0,0,0));

    // charge kicker only if enabled
    if (m_charge) {
        m_shootTime += time;
        // recharge only after a short timeout, to prevent kick the ball twice
        if (!m_isCharged && m_shootTime > 0.1) {
            m_isCharged = true;
        }
    } else {
        m_isCharged = false;
    }
    // check if should kick and can do that
    if (m_isCharged && m_command.has_kick_power() && m_command.kick_power() > 0 && canKickBall(ball)) {
        if (m_command.kick_style() == robot::Command::Linear) {
            const float shootSpeed = m_specs.shot_linear_max();
            const float power = qBound(0.05f, m_command.kick_power(), shootSpeed);

            // kick forward with the specified fraction of the max shoot speed
            ball->kick(t * btVector3(0, 1, 0) * (1/time) * SIMULATOR_SCALE * power * BALL_MASS);
        } else if (m_command.kick_style() == robot::Command::Chip) {
            // reverse strategy calculation -> get desired chip distance
            const float shootDist = m_specs.shot_chip_max();
            const float targetDist = qBound(0.05f, m_command.kick_power(), shootDist);

            // just assume a angle of 45 degrees
            const float angle = 45./180*M_PI;
            const float dirFloor = std::cos(angle);
            const float dirUp = std::sin(angle);

            // if the ball hits the robot the chip distance actually decreases
            const btVector3 relBallSpeed = relativeBallSpeed(ball) / SIMULATOR_SCALE;
            const float speedCompensation = -std::max((btScalar)0, relBallSpeed.y())
                    - qBound((btScalar)0, (btScalar)0.5 * relBallSpeed.y(), (btScalar)0.5 * dirFloor);

            // airtime = 2 * (shootSpeed * dirUp) / g
            // targetDist = shootSpeed * dirFloor * airtime
            const float shootSpeed = std::sqrt(targetDist*m_world->getGravity().length() / (2*std::abs(dirUp*dirFloor)*SIMULATOR_SCALE));
            ball->kick(t * btVector3(0, dirFloor * shootSpeed + speedCompensation, dirUp * shootSpeed) * (1/time) * SIMULATOR_SCALE * BALL_MASS);
        }
        // discharge
        m_isCharged = false;
        m_shootTime = 0.0;
    }

    if (!m_command.has_v_f() || !m_command.has_v_s() || !m_command.has_omega()) {
        return;
    }

    Q_ASSERT(m_command.has_omega());

    btVector3 v_local(t.inverse() * m_body->getLinearVelocity());
    btVector3 v_d_local(m_command.v_s(), m_command.v_f(), 0);

    float v_f = v_local.y()/SIMULATOR_SCALE;
    float v_s = v_local.x()/SIMULATOR_SCALE;
    float omega = m_body->getAngularVelocity().z();

    const float error_v_s = v_d_local.x() - v_s;
    const float error_v_f = v_d_local.y() - v_f;
    const float error_omega = m_command.omega() - omega;

    error_sum_v_s += error_v_s;
    error_sum_v_f += error_v_f;
    error_sum_omega += error_omega;

    const float error_sum_limit = 20.0f;
    error_sum_v_s = qBound(-error_sum_limit, error_sum_v_s, error_sum_limit);
    error_sum_v_f = qBound(-error_sum_limit, error_sum_v_f, error_sum_limit);

    // (1-(1-linear_damping)^timestep)/timestep - compensates damping
    const float V = 1.200f; // keep current speed
    const float K = 1.f/time * 0.5f; // correct half the error during each subtimestep
    const float K_I = /*0.1; //*/ 0.f;

    // as a certain part of the acceleration is required to compensate damping, the robot will run into a speed limit!
    // bound acceleration
    // the speed limit is acceleration * accelScale / V
    float a_f = V*v_f + K*error_v_f + K_I*error_sum_v_f;
    float a_s = V*v_s + K*error_v_s + K_I*error_sum_v_s;

    const float accelScale = 2.f; // let robot accelerate / brake faster than the accelerator does
    a_f = bound(a_f, v_f, accelScale*m_specs.acceleration().a_speedup_f_max(), accelScale*m_specs.acceleration().a_brake_f_max());
    a_s = bound(a_s, v_s, accelScale*m_specs.acceleration().a_speedup_s_max(), accelScale*m_specs.acceleration().a_brake_s_max());
    const btVector3 force(a_s*m_specs.mass(), a_f*m_specs.mass(), 0);

    // localInertia.z() / SIMULATOR_SCALE^2 \approx 1/12*mass*(robot_width^2+robot_depth^2)
    // (1-(1-angular_damping)^timestep)/timestep * localInertia.z()/SIMULATOR_SCALE^2 - compensates damping
    const float V_phi = 1.603f; // keep current rotation
    const float K_phi = 1.f/time * 0.5f; // correct half the error during each subtimestep
    const float K_I_phi = /*0*0.2/1000; //*/ 0.f;

    const float a_phi = V_phi*omega + K_phi*error_omega + K_I_phi*error_sum_omega;
    const float a_phi_bound = bound(a_phi, omega, accelScale*m_specs.acceleration().a_speedup_phi_max(), accelScale*m_specs.acceleration().a_brake_phi_max());
    const btVector3 torque(0, 0, a_phi_bound * 0.007884f);

    if (force.length2() > 0 || torque.length2() > 0) {
        m_body->activate();
        m_body->applyCentralForce(t * force * SIMULATOR_SCALE);
        m_body->applyTorque(torque * SIMULATOR_SCALE * SIMULATOR_SCALE);
    }
}

// copy-paste from accelerator
float SimRobot::bound(float acceleration, float oldSpeed, float speedupLimit, float brakeLimit) const
{
    // In case the robot needs to gain speed
    if ((std::signbit(acceleration) == std::signbit(oldSpeed)) || (oldSpeed == 0)) {
        // the acceleration needs to be bounded with values for speeding up.
        return qBound(-speedupLimit, acceleration, speedupLimit);
    } else {
        // bound braking acceleration, in order to avoid fallover
        return qBound(-brakeLimit, acceleration, brakeLimit);
    }
}

btVector3 SimRobot::relativeBallSpeed(SimBall *ball) const
{
    btTransform t = m_body->getWorldTransform();
    const btVector3 ballSpeed = ball->speed();

    const btQuaternion robotDir = t.getRotation();
    const btVector3 diff = (ballSpeed).rotate(robotDir.getAxis(), -robotDir.getAngle());

    return diff;
}

bool SimRobot::canKickBall(SimBall *ball) const
{
    bool ballCollidesWithRobot = false;

    // check for collision between ball and robot
    int numManifolds = m_world->getDispatcher()->getNumManifolds();
    for (int i = 0; i < numManifolds; ++i) {
        btPersistentManifold *contactManifold = m_world->getDispatcher()->getManifoldByIndexInternal(i);
        btCollisionObject *objectA = (btCollisionObject*)(contactManifold->getBody0());
        btCollisionObject *objectB = (btCollisionObject*)(contactManifold->getBody1());
        if ((objectA == m_body && objectB == ball->body())
                || (objectA == ball->body() && objectB == m_body)) {
            int numContacts = contactManifold->getNumContacts();
            for (int j = 0; j < numContacts; ++j) {
                btManifoldPoint &pt = contactManifold->getContactPoint(j);
                if (pt.getDistance() < 0.001f * SIMULATOR_SCALE) {
                    ballCollidesWithRobot = true;
                    break;
                }
            }
        }
    }

    btTransform t = m_body->getWorldTransform();
    const btVector3 position = t.getOrigin();
    const btVector3 robotPos(position.x(), position.y(), 0);
    const btVector3 ballPos = ball->position();
    // can't kick jumping ball
    if (ballPos.z() > 0.05f * SIMULATOR_SCALE) {
        return false;
    }

    const btQuaternion robotDir = t.getRotation();
    const btVector3 diff = (ballPos - robotPos).rotate(robotDir.getAxis(), -robotDir.getAngle());

    // check that the ball is in front of the dribbler
    if (!ballCollidesWithRobot || diff.y() < 0) {
        return false;
    }

    const float dribblerWidth = (m_specs.has_dribbler_width()) ? m_specs.dribbler_width() : 0.07f;
    // check that ball is inside the dribbler, not left or right of it
    if (fabs(diff.x()) > dribblerWidth * (0.5f * SIMULATOR_SCALE)) {
        return false;
    }
    return true;
}

robot::RadioResponse SimRobot::setCommand(const robot::Command &command, SimBall *ball, bool charge)
{
    m_command = command;
    m_commandTime = 0.0f;
    m_charge = charge;

    robot::RadioResponse response;
    response.set_generation(m_specs.generation());
    response.set_id(m_specs.id());
    response.set_battery(1);
    response.set_packet_loss_rx(0);
    response.set_packet_loss_tx(0);
    response.set_ball_detected(canKickBall(ball));
    response.set_cap_charged(m_isCharged);

    // current velocities
    btTransform t = m_body->getWorldTransform();
    t.setOrigin(btVector3(0,0,0));
    btVector3 v_local(t.inverse() * m_body->getLinearVelocity());
    float v_f = v_local.y()/SIMULATOR_SCALE;
    float v_s = v_local.x()/SIMULATOR_SCALE;
    float omega = m_body->getAngularVelocity().z();

    robot::SpeedStatus *speedStatus = response.mutable_estimated_speed();
    speedStatus->set_v_f(v_f);
    speedStatus->set_v_s(v_s);
    speedStatus->set_omega(omega);

    return response;
}

void SimRobot::update(SSL_DetectionRobot *robot, float stddev_p, float stddev_phi)
{
    // setup vision packet
    robot->set_robot_id(m_specs.id());
    robot->set_confidence(1.0);
    robot->set_pixel_x(0);
    robot->set_pixel_y(0);

    // add noise
    btTransform transform;
    m_motionState->getWorldTransform(transform);
    const btVector3 p = transform.getOrigin() / SIMULATOR_SCALE;
    const Vector2 p_noise = m_rng->normalVector(stddev_p);
    robot->set_x((p.y() + p_noise.x) * 1000.0f);
    robot->set_y(-(p.x() + p_noise.y) * 1000.0f);

    const btQuaternion q = transform.getRotation();
    const btVector3 dir = btMatrix3x3(q).getColumn(0);
    robot->set_orientation(atan2(dir.y(), dir.x()) + m_rng->normal(stddev_phi));
}

void SimRobot::move(const amun::SimulatorMoveRobot &robot)
{
    m_move = robot;
}

bool SimRobot::isFlipped()
{
    btTransform t = m_body->getWorldTransform();
    t.setOrigin(btVector3(0, 0, 0));
    return ((t * btVector3(0, 0, 1)).z() < 0);
}
