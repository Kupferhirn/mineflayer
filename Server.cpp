#include "Server.h"

#include <QDir>
#include <QCoreApplication>

#include <cmath>

Server::Server(QUrl connection_info) :
    m_connection_info(connection_info),
    m_socket_thread(NULL),
    m_parser(NULL),
    m_position_update_timer(NULL),
    m_login_state(Disconnected)
{
    // we run in m_socket_thread
    m_socket_thread = new QThread(this);
    m_socket_thread->start();
    this->moveToThread(m_socket_thread);

    bool success;
    success = QMetaObject::invokeMethod(this, "initialize", Qt::QueuedConnection);
    Q_ASSERT(success);
}

Server::~Server()
{
    delete m_parser;
    delete m_position_update_timer;
}

void Server::initialize()
{
    Q_ASSERT(QThread::currentThread() == m_socket_thread);

    m_socket = new QTcpSocket(this);

    bool success;

    success = connect(m_socket, SIGNAL(connected()), this, SLOT(handleConnected()));
    Q_ASSERT(success);

    success = connect(m_socket, SIGNAL(disconnected()), this, SLOT(cleanUpAfterDisconnect()));
    Q_ASSERT(success);

    success = connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(handleSocketError(QAbstractSocket::SocketError)));
    Q_ASSERT(success);
}

void Server::socketConnect()
{
    if (QThread::currentThread() != m_socket_thread) {
        bool success = QMetaObject::invokeMethod(this, "socketConnect", Qt::QueuedConnection);
        Q_ASSERT(success);
        return;
    }
    Q_ASSERT(m_socket);

    changeLoginState(Connecting);
    m_socket->connectToHost(m_connection_info.host(), m_connection_info.port(25565));
}

void Server::socketDisconnect()
{
    if (QThread::currentThread() != m_socket_thread) {
        bool success = QMetaObject::invokeMethod(this, "socketDisconnect", Qt::QueuedConnection);
        Q_ASSERT(success);
        return;
    }

    if (m_socket->isOpen())
        m_socket->disconnectFromHost();
}

void Server::sendMessage(QSharedPointer<OutgoingRequest> msg)
{
    if (msg.data()->messageType == Message::DummyDisconnect) {
        socketDisconnect();
        return;
    }
    if (m_socket->isOpen()) {
        QDataStream stream(m_socket);
        msg.data()->writeToStream(stream);
    }
}

void Server::handleConnected()
{
    delete m_parser;
    m_parser = new IncomingMessageParser(m_socket);

    bool success;
    success = connect(m_parser, SIGNAL(messageReceived(QSharedPointer<IncomingResponse>)), this, SLOT(processIncomingMessage(QSharedPointer<IncomingResponse>)));
    Q_ASSERT(success);

    changeLoginState(WaitingForHandshakeResponse);
    sendMessage(QSharedPointer<OutgoingRequest>(new HandshakeRequest(m_connection_info.userName())));
}

void Server::cleanUpAfterDisconnect()
{
    qDebug() << "Cleaning up, disconnected";
    m_socket_thread->exit();
    this->moveToThread(QCoreApplication::instance()->thread());
    bool success;
    success = QMetaObject::invokeMethod(this, "terminate", Qt::QueuedConnection);
    Q_ASSERT(success);
}

void Server::terminate()
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    m_socket_thread->exit();
    m_socket_thread->wait();

    changeLoginState(Disconnected);
    emit socketDisconnected();
}

void Server::processIncomingMessage(QSharedPointer<IncomingResponse> incomingMessage)
{
    // possibly handle the message (only for the initial setup)
    switch (incomingMessage.data()->messageType) {
        case Message::Handshake: {
            HandshakeResponse * message = (HandshakeResponse *)incomingMessage.data();
            Q_ASSERT(m_login_state == WaitingForHandshakeResponse);
            // we don't support authenticated logging in yet.
            Q_ASSERT_X(message->connectionHash == HandshakeResponse::AuthenticationNotRequired, "",
                       (QString("unexpected connection hash: ") + message->connectionHash).toStdString().c_str());
            changeLoginState(WaitingForLoginResponse);
            changeLoginState(WaitingForPlayerPositionAndLook);
            sendMessage(QSharedPointer<OutgoingRequest>(new LoginRequest(m_connection_info.userName(), m_connection_info.password())));
            break;
        }
        case Message::PlayerPositionAndLook: {
            PlayerPositionAndLookResponse * message = (PlayerPositionAndLookResponse *) incomingMessage.data();
            fromNotchianXyz(player_position, message->x, message->z, message->y);
            player_position.stance = message->stance;
            fromNotchianYawPitch(player_position, message->yaw, message->pitch);
            player_position.roll = 0.0f;
            player_position.on_ground = message->on_ground;
            if (m_login_state == WaitingForPlayerPositionAndLook)
                gotFirstPlayerPositionAndLookResponse();
            break;
        }
        case Message::MapChunk: {
            MapChunkResponse * message = (MapChunkResponse *) incomingMessage.data();

            QByteArray tmp = message->compressed_data;
            // prepend a guess at the final size... or just 0.
            tmp.prepend(QByteArray("\0\0\0\0", 4));
            QByteArray decompressed = qUncompress(tmp);

            Chunk::Coord position;
            fromNotchianXyz(position, message->x, message->y, message->z);
            Chunk::Coord notchian_size;
            notchian_size.x = message->size_x_minus_one + 1;
            notchian_size.y = message->size_y_minus_one + 1;
            notchian_size.z = message->size_z_minus_one + 1;
            Chunk::Coord size;
            fromNotchianXyz(size, notchian_size.x, notchian_size.y, notchian_size.z);

            Chunk * chunk = new Chunk(position, size);

            int array_index = 0;
            Chunk::Coord notchian_relative_pos;
            for (notchian_relative_pos.x = 0; notchian_relative_pos.x < notchian_size.x; notchian_relative_pos.x++) {
                for (notchian_relative_pos.z = 0; notchian_relative_pos.z < notchian_size.z; notchian_relative_pos.z++) {
                    for (notchian_relative_pos.y = 0; notchian_relative_pos.y < notchian_size.y; notchian_relative_pos.y++) {
                        int block_type = decompressed.at(array_index++);
                        Chunk::Coord relative_pos;
                        fromNotchianXyz(relative_pos, notchian_relative_pos.x, notchian_relative_pos.y, notchian_relative_pos.z);
                        chunk->getBlock(relative_pos).data()->type = block_type;
                    }
                }
            }
            // ... and then just ignore the other two arrays.
            emit mapChunkUpdated(QSharedPointer<Chunk>(chunk));
            break;
        }
        case Message::DisconnectOrKick: {
            DisconnectOrKickResponse * message = (DisconnectOrKickResponse *)incomingMessage.data();
            qDebug() << "got disconnected: " << message->reason;
            finishWritingAndDisconnect();
            break;
        }
        default: {
            // qDebug() << "ignoring message type: 0x" << QString::number(incomingMessage.data()->messageType, 16).toStdString().c_str();
            break;
        }
    }
}

void Server::fromNotchianXyz(EntityPosition &destination, double notchian_x, double notchian_y, double notchian_z)
{
    // east
    destination.x = notchian_z;
    // north
    destination.y = -notchian_x;
    // up
    destination.z = notchian_y;
}
void Server::fromNotchianXyz(Chunk::Coord &destination, int notchian_x, int notchian_y, int notchian_z)
{
    // east
    destination.x = notchian_z;
    // north
    destination.y = -notchian_x;
    // up
    destination.z = notchian_y;
}

void Server::toNotchianXyz(const EntityPosition &source, double &destination_notchian_x, double &destination_notchian_y, double &destination_notchian_z)
{
    // east
    destination_notchian_z = source.x;
    // north
    destination_notchian_x = -source.y;
    // up
    destination_notchian_y = source.z;
}


void Server::fromNotchianYawPitch(EntityPosition &destination, float notchian_yaw, float notchian_pitch)
{
    // amazingly, yaw is oriented properly.
    destination.yaw = euclideanMod(degreesToRadians(notchian_yaw), c_2pi);
    // TODO: check pitch
    destination.pitch = euclideanMod(degreesToRadians(notchian_pitch) + c_pi, c_2pi) - c_2pi;
}

void Server::toNotchianYawPitch(const EntityPosition &source, float &destination_notchian_yaw, float &destination_notchian_pitch)
{
    destination_notchian_yaw = radiansToDegrees(source.yaw);
    // TODO: check pitch
    destination_notchian_pitch = radiansToDegrees(source.pitch);
}

const float Server::c_pi = 3.14159265f;
const float Server::c_2pi = 6.28318531f;
const float Server::c_degrees_per_radian = 57.2957795f;
const float Server::c_radians_per_degree = 0.0174532925f;

float Server::degreesToRadians(float degrees)
{
    return degrees * c_radians_per_degree;
}

float Server::radiansToDegrees(float radians)
{
    return radians * c_degrees_per_radian;
}

float Server::euclideanMod(float numerator, float denominator)
{
    float result = std::fmod(numerator, denominator);
    if (result < 0)
        result += denominator;
    return result;
}


void Server::gotFirstPlayerPositionAndLookResponse()
{
    delete m_position_update_timer;
    m_position_update_timer = new QTimer();
    m_position_update_timer->setInterval(200);

    bool success;
    success = connect(m_position_update_timer, SIGNAL(timeout()), this, SLOT(sendPosition()));
    Q_ASSERT(success);

    m_position_update_timer->start();

    changeLoginState(Success);
}

void Server::sendPosition()
{
    PlayerPositionAndLookRequest * request = new PlayerPositionAndLookRequest;
    toNotchianXyz(player_position, request->x, request->y, request->z);
    request->stance = player_position.stance;
    toNotchianYawPitch(player_position, request->yaw, request->pitch);
    request->on_ground = player_position.on_ground;
    sendMessage(QSharedPointer<OutgoingRequest>(request));
}

void Server::changeLoginState(LoginStatus state)
{
    m_login_state = state;
    emit loginStatusUpdated(state);
}

void Server::handleSocketError(QAbstractSocket::SocketError error)
{
    qDebug() << "Socket error: " << error;
    changeLoginState(SocketError);
}

void Server::finishWritingAndDisconnect()
{
    // put a dummy message on the queue
    this->sendMessage(QSharedPointer<OutgoingRequest>(new DummyDisconnectRequest()));
}

Server::LoginStatus Server::loginStatus()
{
    return m_login_state;
}
