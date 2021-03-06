/***************************************************************************
 *   Copyright 2015 Michael Bleier, Michael Eischer, Jan Kallwies,         *
 *       Philipp Nordhus                                                   *
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

#include "config.h"
#include "core/timer.h"
#include "firmware/common/radiocommand.h"
#include "firmware/2012/common/radiocommand2012.h"
#include "firmware/2012/common/transceiver2012.h"
#include "firmware/2014/common/radiocommand2014.h"
#include "firmware/common/radiocommand.h"
#include "transceiver.h"
#include "usbthread.h"
#include "usbdevice.h"
#include <QTimer>
#include <QDebug>

static_assert(sizeof(RadioCommand2014) == 11, "Expected radio command packet of size 11");
static_assert(sizeof(RadioResponse2014) == 10, "Expected radio response packet of size 10");

const int PROTOCOL_VERSION = 2;

typedef struct
{
    int64_t time;
} __attribute__ ((packed)) TransceiverPingData;


Transceiver::Transceiver(QObject *parent) :
    QObject(parent),
    m_charge(false),
    m_packetCounter(0),
    m_context(nullptr),
    m_device(nullptr),
    m_connectionState(State::DISCONNECTED),
    m_simulatorEnabled(false)
{
    // default channel
    m_configuration.set_channel(10);

    m_timeoutTimer = new QTimer(this);
    connect(m_timeoutTimer, &QTimer::timeout, this, &Transceiver::timeout);
}

Transceiver::~Transceiver()
{
    close();
#ifdef USB_FOUND
    delete m_context;
#endif
}

void Transceiver::handleRadioCommands(const QList<robot::RadioCommand> &commands)
{
    Status status(new amun::Status);
    const qint64 transceiver_start = Timer::systemTime();

    // charging the condensator can be enabled / disable separately
    sendCommand(commands, m_charge);

    status->mutable_timing()->set_transceiver((Timer::systemTime() - transceiver_start) / 1E9);
    emit sendStatus(status);
}

void Transceiver::handleCommand(const Command &command)
{
    if (command->has_simulator()) {
        if (command->simulator().has_enable()) {
            m_simulatorEnabled = command->simulator().enable();
            if (m_simulatorEnabled) {
                close();
            }
        }
    }

    if (command->has_transceiver()) {
        const amun::CommandTransceiver &t = command->transceiver();
        if (t.has_enable() && !m_simulatorEnabled) {
            if (t.enable()) {
                open();
            } else {
                close();
            }
        }

        if (t.has_charge()) {
            m_charge = t.charge();
        }

        if (t.has_configuration()) {
            m_configuration = t.configuration();
            sendTransceiverConfiguration();
        }
    }

    if (command->has_robot_parameters()) {
        sendParameters(command->robot_parameters());
    }

    if (command->has_set_team_blue()) {
        handleTeam(command->set_team_blue());
    }

    if (command->has_set_team_yellow()) {
        handleTeam(command->set_team_yellow());
    }
}

void Transceiver::handleTeam(const robot::Team &team)
{
    for (int i = 0; i < team.robot_size(); ++i) {
        const robot::Specs &spec = team.robot(i);
        m_ir_param[qMakePair(spec.generation(), spec.id())] = spec.ir_param();
    }
}

void Transceiver::open()
{
#ifdef USB_FOUND
    if (m_context == nullptr) {
        m_context = new USBThread();
    }

    close();

    // get transceiver
    QList<USBDevice*> devices = USBDevice::getDevices(0x03eb, 0x6127, m_context);
    if (devices.isEmpty()) {
        Status status(new amun::Status);
        status->mutable_transceiver()->set_active(false);
        status->mutable_transceiver()->set_error("Device not found!");
        emit sendStatus(status);
        m_connectionState = State::DISCONNECTED;
        return;
    }

    // just assumes it's the first matching device
    USBDevice *device = devices.takeFirst();
    qDeleteAll(devices);

    m_device = device;
    connect(m_device, SIGNAL(readyRead()), SLOT(receive()));

    // try to open the communication channel
    if (!device->open(QIODevice::ReadWrite)) {
        close();
        return;
    }

    // publish transceiver status
    Status status(new amun::Status);
    status->mutable_transceiver()->set_active(true);
    status->mutable_transceiver()->set_error("Handshake");
    emit sendStatus(status);

    // send protocol handshake
    // the transceiver should return the highest supported version <= hostConfig->protocolVersion
    // if the version is higher/less than supported by the host, then the connection will fail
    m_connectionState = State::HANDSHAKE;
    // don't get stuck if the transceiver doesn't answer / has too old firmware
    m_timeoutTimer->start(500);
    sendInitPacket();

    return;
#else
    Status status(new amun::Status);
    status->mutable_transceiver()->set_active(false);
    status->mutable_transceiver()->set_error("Compiled without libusb support!");
    emit sendStatus(status);
    return;
#endif // USB_FOUND
}

bool Transceiver::ensureOpen()
{
    if (!m_device) {
        open();
        return false;
    }
    return m_connectionState == State::CONNECTED;
}

void Transceiver::close(const QString &errorMsg)
{
#ifdef USB_FOUND
    // close and cleanup
    if (m_device != nullptr) {
        Status status(new amun::Status);
        status->mutable_transceiver()->set_active(false);
        if (errorMsg.isNull()) {
            status->mutable_transceiver()->set_error(m_device->errorString().toStdString());
        } else {
            status->mutable_transceiver()->set_error(errorMsg.toStdString());
        }
        emit sendStatus(status);

        delete m_device;
        m_device = nullptr;
        m_timeoutTimer->stop();
    }
    m_connectionState = State::DISCONNECTED;
#endif // USB_FOUND
}

void Transceiver::timeout()
{
    close("Transceiver is not responding");
}

bool Transceiver::write(const char *data, qint64 size)
{
#ifdef USB_FOUND
    if (!m_device) {
        return false;
    }

    // close radio link on errors
    if (m_device->write(data, size) < 0) {
        close();
        return false;
    }
#endif // USB_FOUND
    return true;
}

void Transceiver::receive()
{
#ifdef USB_FOUND
    if (!m_device) {
        return;
    }

    const int maxSize = 512;
    char buffer[maxSize];
    QList<robot::RadioResponse> responses;

    // read until no further data is available
    const int size = m_device->read(buffer, maxSize);
    if (size == -1) {
        close();
        return;
    }

    // transceiver class is only used for real robot -> global timer can be used
    const qint64 receiveTime = Timer::systemTime();

    int pos = 0;
    while (pos < size) {
        // check header size
        if (pos + (int)sizeof(TransceiverResponsePacket) > size) {
            break;
        }

        const TransceiverResponsePacket *header = (const TransceiverResponsePacket*) &buffer[pos];
        // check command content size
        if (pos + (int)sizeof(TransceiverResponsePacket) + header->size > size) {
            break;
        }

        pos += sizeof(TransceiverResponsePacket);
        // handle command
        switch (header->command) {
        case COMMAND_INIT_REPLY:
            handleInitPacket(&buffer[pos], header->size);
            break;
        case COMMAND_PING_REPLY:
            handlePingPacket(&buffer[pos], header->size);
            break;
        case COMMAND_STATUS_REPLY:
            handleStatusPacket(&buffer[pos], header->size);
            break;
        case COMMAND_REPLY_FROM_ROBOT:
            handleResponsePacket(responses, &buffer[pos], header->size, receiveTime);
            break;
        case COMMAND_DATAGRAM_RECEIVED:
            handleDatagramPacket(&buffer[pos], header->size);
            break;
        }

        pos += header->size;
    }

    emit sendRadioResponses(responses);
#endif // USB_FOUND
}

void Transceiver::handleInitPacket(const char *data, uint size)
{
    // only allowed during handshake
    if (m_connectionState != State::HANDSHAKE || size < 2) {
        close("Invalid reply from transceiver");
        return;
    }

    const TransceiverInitPacket *handshake = (const TransceiverInitPacket *)data;
    if (handshake->protocolVersion < PROTOCOL_VERSION) {
        close("Outdated firmware");
        return;
    } else if (handshake->protocolVersion > PROTOCOL_VERSION) {
        close("Not yet supported transceiver firmware");
        return;
    }

    m_connectionState = State::CONNECTED;
    m_timeoutTimer->stop();

    Status status(new amun::Status);
    status->mutable_transceiver()->set_active(true);
    emit sendStatus(status);

    // send channel informations
    sendTransceiverConfiguration();
}

void Transceiver::handlePingPacket(const char *data, uint size)
{
    if (size < sizeof(TransceiverPingData)) {
        return;
    }

    const TransceiverPingData *ping = (const TransceiverPingData *)data;
    Status status(new amun::Status);
    status->mutable_timing()->set_transceiver_rtt((Timer::systemTime() - ping->time) / 1E9);
    emit sendStatus(status);
    // stop ping timeout timer
    m_timeoutTimer->stop();
}

void Transceiver::handleStatusPacket(const char *data, uint size)
{
    if (size < sizeof(TransceiverStatusPacket)) {
        return;
    }

    const TransceiverStatusPacket *transceiverStatus = (const TransceiverStatusPacket *)data;
    Status status(new amun::Status);
    status->mutable_transceiver()->set_active(true);
    status->mutable_transceiver()->set_dropped_usb_packets(transceiverStatus->droppedPackets);
    emit sendStatus(status);
}

void Transceiver::handleDatagramPacket(const char *data, uint size)
{
    qDebug() << QByteArray(data, size);
}

float Transceiver::calculateDroppedFramesRatio(uint generation, uint id, uint8_t counter, int skipedFrames)
{
    // get frame counter, is created with default values if not existing
    DroppedFrameCounter &c = m_droppedFrames[qMakePair(generation, id)];
    if (skipedFrames >= 0) {
        c.skipedFrames = skipedFrames;
    }

    // correctly handle startup
    if (c.startValue == -1) {
        c.startValue = counter;
    } else if (counter > c.lastFrameCounter) {
        // counter should have increased by one
        // if it has increased further, then we've lost a packet
        c.droppedFramesCounter += counter - c.lastFrameCounter - 1;
    } else {
        // counter isn't increasing -> counter has overflown, update statistic
        // account for packets lost somewhere around the counter overflow
        // as the robot can only reply if it got a frame, skip the frames it didn't get (only 2014)
        c.droppedFramesRatio = (c.droppedFramesCounter + (255 - c.lastFrameCounter) - c.skipedFrames)
                / (256.f - c.startValue - c.skipedFrames);
        // if the counter is non-zero we've already lost some packets
        c.droppedFramesCounter = counter;
        c.startValue = 0;
    }

    c.lastFrameCounter = counter;

    return c.droppedFramesRatio;
}

void Transceiver::handleResponsePacket(QList<robot::RadioResponse> &responses, const char *data, uint size, qint64 time)
{
    const RadioResponseHeader *header = (const RadioResponseHeader *)data;
    size -= sizeof(RadioResponseHeader);
    data += sizeof(RadioResponseHeader);

    if (header->command == RESPONSE_2012_DEFAULT && size == sizeof(RadioResponse2012)) {
        const RadioResponse2012 *packet = (const RadioResponse2012 *)data;

        robot::RadioResponse r;
        r.set_time(time);
        r.set_generation(2);
        r.set_id(packet->id);
        r.set_battery(packet->battery / 255.0f);
        r.set_packet_loss_rx(packet->packet_loss / 255.0f);
        float df = calculateDroppedFramesRatio(2, packet->id, packet->counter, 0);
        r.set_packet_loss_tx(df);
        if (packet->main_active) {
            robot::SpeedStatus *speedStatus = r.mutable_estimated_speed();
            speedStatus->set_v_f(packet->v_f / 1000.f);
            speedStatus->set_v_s(packet->v_s / 1000.f);
            speedStatus->set_omega(packet->omega / 1000.f);
            r.set_error_present(packet->motor_in_power_limit);
        }
        if (packet->kicker_active) {
            r.set_ball_detected(packet->ball_detected);
            r.set_cap_charged(packet->cap_charged);
        }
        responses.append(r);
    } else  if (header->command == RESPONSE_2014_DEFAULT && size == sizeof(RadioResponse2014)) {
        const RadioResponse2014 *packet = (const RadioResponse2014 *)data;

        robot::RadioResponse r;
        r.set_time(time);
        r.set_generation(3);
        r.set_id(packet->id);

        int packet_loss = (packet->extension_id == EXTENSION_BASIC_STATUS) ? packet->packet_loss : -1;
        float df = calculateDroppedFramesRatio(3, packet->id, packet->counter, packet_loss);
        switch (packet->extension_id) {
        case EXTENSION_BASIC_STATUS:
            r.set_battery(packet->battery / 255.0f);
            r.set_packet_loss_rx(packet->packet_loss / 256.0f);
            r.set_packet_loss_tx(df);
            break;
        case EXTENSION_EXTENDED_ERROR:
        {
            robot::ExtendedError *e = r.mutable_extended_error();
            e->set_motor_1_error(packet->motor_1_error);
            e->set_motor_2_error(packet->motor_2_error);
            e->set_motor_3_error(packet->motor_3_error);
            e->set_motor_4_error(packet->motor_4_error);
            e->set_dribbler_error(packet->dribler_error);
            e->set_kicker_error(packet->kicker_error);
            e->set_temperature(packet->temperature);
            break;
        }
        default:
            break;
        }

        if (packet->power_enabled) {
            robot::SpeedStatus *speedStatus = r.mutable_estimated_speed();
            speedStatus->set_v_f(packet->v_f / 1000.f);
            speedStatus->set_v_s(packet->v_s / 1000.f);
            speedStatus->set_omega(packet->omega / 1000.f);
            r.set_error_present(packet->error_present);

            r.set_ball_detected(packet->ball_detected);
            r.set_cap_charged(packet->cap_charged);
        }
        if (m_frameTimes.contains(packet->counter)) {
            r.set_radio_rtt((time - m_frameTimes[packet->counter]) / 1E9);
        }
        responses.append(r);
    }
}

void Transceiver::addRobot2012Command(int id, const robot::Command &command, bool charge, quint8 packetCounter, QByteArray &usb_packet)
{
    // copy command
    RadioCommand2012 data;
    data.charge = charge;
    data.standby = command.standby();
    data.counter = packetCounter;
    data.dribbler = qBound<qint32>(-RADIOCOMMAND2012_DRIBBLER_MAX, command.dribbler() * RADIOCOMMAND2012_DRIBBLER_MAX, RADIOCOMMAND2012_DRIBBLER_MAX);
    data.chip = command.kick_style() == robot::Command::Chip;
    data.shot_power = qMin<quint32>(command.kick_power() * RADIOCOMMAND2012_KICK_MAX, RADIOCOMMAND2012_KICK_MAX);
    data.v_x = qBound<qint32>(-RADIOCOMMAND2012_V_MAX, command.v_s() * 1000.0f, RADIOCOMMAND2012_V_MAX);
    data.v_y = qBound<qint32>(-RADIOCOMMAND2012_V_MAX, command.v_f() * 1000.0f, RADIOCOMMAND2012_V_MAX);
    data.omega = qBound<qint32>(-RADIOCOMMAND2012_OMEGA_MAX, command.omega() * 1000.0f, RADIOCOMMAND2012_OMEGA_MAX);
    data.id = id;

    // set address
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_SEND_NRF24;
    senderCommand.size = sizeof(data) + sizeof(TransceiverSendNRF24Packet);

    TransceiverSendNRF24Packet targetAddress;
    memcpy(targetAddress.address, robot2012_address, sizeof(targetAddress.address));
    targetAddress.address[4] |= id;
    targetAddress.expectedResponseSize = sizeof(RadioResponseHeader) + sizeof(RadioResponse2012);

    usb_packet.append((const char*) &senderCommand, sizeof(senderCommand));
    usb_packet.append((const char*) &targetAddress, sizeof(targetAddress));
    usb_packet.append((const char*) &data, sizeof(data));
}

void Transceiver::addRobot2014Command(int id, const robot::Command &command, bool charge, quint8 packetCounter, QByteArray &usb_packet)
{
    // copy command
    RadioCommand2014 data;
    data.charge = charge;
    data.standby = command.standby();
    data.counter = packetCounter;
    data.dribbler = qBound<qint32>(-RADIOCOMMAND2014_DRIBBLER_MAX, command.dribbler() * RADIOCOMMAND2014_DRIBBLER_MAX, RADIOCOMMAND2014_DRIBBLER_MAX);
    data.chip = command.kick_style() == robot::Command::Chip;
    if (data.chip) {
        data.shot_power = qMin<quint32>(command.kick_power() / RADIOCOMMAND2014_CHIP_MAX * RADIOCOMMAND2014_KICK_MAX, RADIOCOMMAND2014_KICK_MAX);
    } else {
        data.shot_power = qMin<quint32>(command.kick_power() / RADIOCOMMAND2014_LINEAR_MAX * RADIOCOMMAND2014_KICK_MAX, RADIOCOMMAND2014_KICK_MAX);
    }
    data.v_x = qBound<qint32>(-RADIOCOMMAND2014_V_MAX, command.v_s() * 1000.0f, RADIOCOMMAND2014_V_MAX);
    data.v_y = qBound<qint32>(-RADIOCOMMAND2014_V_MAX, command.v_f() * 1000.0f, RADIOCOMMAND2014_V_MAX);
    data.omega = qBound<qint32>(-RADIOCOMMAND2014_OMEGA_MAX, command.omega() * 1000.0f, RADIOCOMMAND2014_OMEGA_MAX);
    data.id = id;
    data.force_kick = command.force_kick();
    data.ir_param = qBound<quint8>(0, m_ir_param[qMakePair(3, id)], 63);
    data.eject_sdcard = command.eject_sdcard();
    data.unused = 0;

    // set address
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_SEND_NRF24;
    senderCommand.size = sizeof(data) + sizeof(TransceiverSendNRF24Packet);

    TransceiverSendNRF24Packet targetAddress;
    memcpy(targetAddress.address, robot2014_address, sizeof(targetAddress.address));
    targetAddress.address[4] |= id;
    targetAddress.expectedResponseSize = sizeof(RadioResponseHeader) + sizeof(RadioResponse2014);

    usb_packet.append((const char*) &senderCommand, sizeof(senderCommand));
    usb_packet.append((const char*) &targetAddress, sizeof(targetAddress));
    usb_packet.append((const char*) &data, sizeof(data));
}

void Transceiver::addPingPacket(qint64 time, QByteArray &usb_packet)
{
    // Append ping packet with current timestamp
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_PING;
    senderCommand.size = sizeof(TransceiverPingData);

    TransceiverPingData ping;
    ping.time = time;

    usb_packet.append((const char*) &senderCommand, sizeof(senderCommand));
    usb_packet.append((const char*) &ping, sizeof(ping));
}

void Transceiver::addStatusPacket(QByteArray &usb_packet)
{
    // request count of dropped usb packets
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_STATUS;
    senderCommand.size = 0;

    usb_packet.append((const char*) &senderCommand, sizeof(senderCommand));
}

void Transceiver::sendCommand(const QList<robot::RadioCommand> &commands, bool charge)
{
    if (!ensureOpen()) {
        return;
    }

    typedef QList<robot::RadioCommand> RobotList;

    QMap<uint, RobotList> generations;
    foreach (const robot::RadioCommand &robot, commands) {
        // group by generation
        generations[robot.generation()].append(robot);
    }

    m_packetCounter++;
    // remember when the packetCounter was used
    const qint64 time = Timer::systemTime();
    m_frameTimes[m_packetCounter] = time;

    // used for packet assembly
    QByteArray usb_packet;

    QMapIterator<uint, RobotList> it(generations);
    while (it.hasNext()) {
        it.next();

        foreach (const robot::RadioCommand &radio_command, it.value()) {
            if (it.key() == 2) {
                addRobot2012Command(radio_command.id(), radio_command.command(), charge, m_packetCounter, usb_packet);
            } else if (it.key() == 3) {
                addRobot2014Command(radio_command.id(), radio_command.command(), charge, m_packetCounter, usb_packet);
            }
        }
    }

    addPingPacket(time, usb_packet);
    if (m_packetCounter == 255) {
        addStatusPacket(usb_packet);
    }

    // Workaround for usb problems if packet size is a multiple of transfer size
    if (usb_packet.size() % 64 == 0) {
        addPingPacket(time, usb_packet);
    }

    write(usb_packet.data(), usb_packet.size());

    // only restart timeout if not yet active
    if (!m_timeoutTimer->isActive()) {
        m_timeoutTimer->start(1000);
    }
}

void Transceiver::sendParameters(const robot::RadioParameters &parameters)
{
    if (!ensureOpen()) {
        return;
    }
    RadioParameters2012 p;
    memset(&p, 0, sizeof(p));

    // copy up to 16 radio parameters, 2 bytes per parameter
    for (int i=0; i<parameters.p_size() && i<16; i++) {
        p.k[i] = parameters.p(i);
    }

    // broadcast config
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_SEND_NRF24;
    senderCommand.size = sizeof(p) + sizeof(TransceiverSendNRF24Packet);

    TransceiverSendNRF24Packet targetAddress;
    memcpy(targetAddress.address, robot2012_config_broadcast, sizeof(targetAddress.address));
    targetAddress.expectedResponseSize = 0;

    QByteArray usb_packet;
    usb_packet.append((const char *) &senderCommand, sizeof(senderCommand));
    usb_packet.append((const char *) &targetAddress, sizeof(targetAddress));
    usb_packet.append((const char *) &p, senderCommand.size);
    write(usb_packet.data(), usb_packet.size());
}

void Transceiver::sendTransceiverConfiguration()
{
    // configure transceiver frequency
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_SET_FREQUENCY;
    senderCommand.size = sizeof(TransceiverSetFrequencyPacket);

    TransceiverSetFrequencyPacket config;
    config.channel = m_configuration.channel();

    QByteArray usb_packet;
    usb_packet.append((const char *) &senderCommand, sizeof(senderCommand));
    usb_packet.append((const char *) &config, sizeof(config));
    write(usb_packet.data(), usb_packet.size());
}

void Transceiver::sendInitPacket()
{
    // send init handshake
    TransceiverCommandPacket senderCommand;
    senderCommand.command = COMMAND_INIT;
    senderCommand.size = sizeof(TransceiverInitPacket);

    TransceiverInitPacket config;
    config.protocolVersion = PROTOCOL_VERSION;

    QByteArray usb_packet;
    usb_packet.append((const char *) &senderCommand, sizeof(senderCommand));
    usb_packet.append((const char *) &config, sizeof(config));
    write(usb_packet.data(), usb_packet.size());
}
