/***************************************************************************
 *   Copyright 2015 Michael Eischer                                        *
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

#include "internalreferee.h"
#include "protobuf/ssl_referee.h"

InternalReferee::InternalReferee(QObject *parent) :
    QObject(parent)
{
    // referee packet uses mircoseconds, not nanoseconds
    // timestamp is not set as this would require the current strategy time to have any meaning
    m_referee.set_packet_timestamp(0);
    m_referee.set_stage(SSL_Referee::NORMAL_FIRST_HALF);
    m_referee.set_stage_time_left(0);
    m_referee.set_command(SSL_Referee::HALT);
    m_referee.set_command_counter(0); // !!! is used as delta value by internal referee !!!
    m_referee.set_command_timestamp(0);
    teamInfoSetDefault(m_referee.mutable_yellow());
    teamInfoSetDefault(m_referee.mutable_blue());

    assert(m_referee.IsInitialized());
}

void InternalReferee::sendRefereePacket(bool updateCommand) {
    m_referee.set_command_counter((updateCommand)?1:0);
    m_referee.set_packet_timestamp(0);
    assert(m_referee.IsInitialized());

    std::string referee;
    if (m_referee.SerializeToString(&referee)) {
        Command command(new amun::Command);
        command->mutable_referee()->set_command(referee);
        emit sendCommand(command);
    }
}

void InternalReferee::changeCommand(SSL_Referee::Command command) {
    m_referee.set_command(command);
    m_referee.set_command_timestamp(0);
    sendRefereePacket(true);
}

void InternalReferee::changeStage(SSL_Referee::Stage stage) {
    m_referee.set_stage(stage);
    sendRefereePacket(false);
}

void InternalReferee::changeYellowKeeper(uint id) {
    m_referee.mutable_yellow()->set_goalie(id);
    sendRefereePacket(false);
}

void InternalReferee::changeBlueKeeper(uint id) {
    m_referee.mutable_blue()->set_goalie(id);
    sendRefereePacket(false);
}

void InternalReferee::handleStatus(const Status &status)
{
    if (status->has_game_state()) {
        const amun::GameState &state = status->game_state();
        // update referee information
        m_referee.set_stage(state.stage());
        m_referee.mutable_yellow()->CopyFrom(state.yellow());
        m_referee.mutable_blue()->CopyFrom(state.blue());
        // don't copy command as it is only updated when changeCommand is used
    }
}
