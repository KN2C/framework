/***************************************************************************
 *   Copyright 2015 Michael Eischer, Philipp Nordhus                       *
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

#ifndef AMUN_H
#define AMUN_H

#include "protobuf/command.h"
#include "protobuf/status.h"

class NetworkInterfaceWatcher;
class Processor;
class Receiver;
class Simulator;
class Strategy;
class Timer;
class Transceiver;
class NetworkTransceiver;
class QHostAddress;

class Amun : public QObject
{
    Q_OBJECT

public:
    explicit Amun(QObject *parent = 0);
    ~Amun() override;

signals:
    void sendStatus(const Status &status);
    void gotCommand(const Command &command);
    void setScaling(float scaling);
    void updateVisionPort(quint16 port);

public:
    void start();
    void stop();

public slots:
    void handleCommand(const Command &command);

private slots:
    void handleStatus(const Status &status);

private:
    void setupReceiver(Receiver *&receiver, const QHostAddress &address, quint16 port);
    void setSimulatorEnabled(bool enabled, bool useNetworkTransceiver);
    void updateScaling(float scaling);

private:
    QThread *m_processorThread;
    QThread *m_networkThread;
    QThread *m_simulatorThread;
    QThread *m_strategyThread[2];

    Processor *m_processor;
    Transceiver *m_transceiver;
    NetworkTransceiver *m_networkTransceiver;
    Simulator *m_simulator;
    Receiver *m_referee;
    Receiver *m_vision;
    Receiver *m_networkCommand;
    Receiver *m_mixedTeam;
    Strategy *m_strategy[2];
    qint64 m_lastTime;
    Timer *m_timer;
    bool m_simulatorEnabled;
    float m_scaling;
    bool m_useNetworkTransceiver;

    NetworkInterfaceWatcher *m_networkInterfaceWatcher;
};

#endif // AMUN_H
