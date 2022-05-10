// Copyright 1996-2022 Cyberbotics Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "WbStreamingServer.hpp"

#include "WbApplication.hpp"
#include "WbControlledWorld.hpp"
#include "WbField.hpp"
#include "WbHttpReply.hpp"
#include "WbLanguage.hpp"
#include "WbMainWindow.hpp"
#include "WbNodeOperations.hpp"
#include "WbPreferences.hpp"
#include "WbProject.hpp"
#include "WbRobot.hpp"
#include "WbSimulationState.hpp"
#include "WbSimulationWorld.hpp"
#include "WbStandardPaths.hpp"
#include "WbStreamingTcpServer.hpp"
#include "WbSupervisorUtilities.hpp"
#include "WbTemplateManager.hpp"
#include "WbWorld.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QSslKey>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

#include <iostream>

WbMainWindow *WbStreamingServer::cMainWindow = NULL;

WbStreamingServer::WbStreamingServer(bool stream) :
  QObject(),
  mPauseTimeout(-1),
  mWebSocketServer(NULL),
  mClientsReadyToReceiveMessages(false),
  mStream(stream) {
  connect(WbApplication::instance(), &WbApplication::postWorldLoaded, this, &WbStreamingServer::newWorld);
  connect(WbApplication::instance(), &WbApplication::preWorldLoaded, this, &WbStreamingServer::deleteWorld);
  connect(WbApplication::instance(), &WbApplication::worldLoadingHasProgressed, this,
          &WbStreamingServer::setWorldLoadingProgress);
  connect(WbApplication::instance(), &WbApplication::worldLoadingStatusHasChanged, this,
          &WbStreamingServer::setWorldLoadingStatus);
  connect(WbNodeOperations::instance(), &WbNodeOperations::nodeAdded, this, &WbStreamingServer::propagateNodeAddition);
  connect(WbTemplateManager::instance(), &WbTemplateManager::postNodeRegeneration, this,
          &WbStreamingServer::propagateNodeAddition);
}

WbStreamingServer::~WbStreamingServer() {
  if (isActive())
    destroy();
  WbLog::info(tr("Streaming server closed"));
};

QString WbStreamingServer::clientToId(QWebSocket *client) {
  return QString::number((quintptr)client);
}

void WbStreamingServer::setMainWindow(WbMainWindow *mainWindow) {
  cMainWindow = mainWindow;
}

void WbStreamingServer::start(int port) {
  static int originalPort = -1;
  if (originalPort == -1)
    originalPort = port;
  mPort = port;
  try {
    create(port);
    originalPort = -1;
  } catch (const QString &e) {
    if (originalPort + 10 > port) {
      std::cerr << tr("Error when creating the TCP streaming server on port %1: %2, trying again with port %3")
                     .arg(port)
                     .arg(e)
                     .arg(port + 1)
                     .toUtf8()
                     .constData()
                << std::endl;
      mPort++;
      start(mPort);
    } else
      mPort = -1;  // failed, giving up
  }
  if (mStream)
    WbLog::info(tr("Streaming server listening on port %1.").arg(port));
}

void WbStreamingServer::sendToJavascript(const QByteArray &string) {
  WbRobot *robot = dynamic_cast<WbRobot *>(sender());
  if (robot) {
    QJsonObject jsonObject;
    jsonObject.insert("name", robot->name());
    jsonObject.insert("message", QString::fromUtf8(string));
    const QJsonDocument jsonDocument(jsonObject);
    sendToClients("robot: " + jsonDocument.toJson(QJsonDocument::Compact));
  } else
    WbLog::info("WbStreamingServer::sendToJavaScript: Can't send message: " + QString::fromUtf8(string));
}

void WbStreamingServer::stop() {
  destroy();
}

void WbStreamingServer::create(int port) {
  // Create a simple HTTP server, serving:
  // - a websocket on "/"
  // - texture images on the other urls. e.g. "/textures/dir/image.[jpg|png|hdr]"

  // Reference to let live QTcpSocket and QWebSocketServer on the same port using `QWebSocketServer::handleConnection()`:
  // - https://bugreports.qt.io/browse/QTBUG-54276
  mWebSocketServer = new QWebSocketServer("Webots Streaming Server", QWebSocketServer::NonSecureMode, this);
  mTcpServer = new WbStreamingTcpServer();
  if (!mTcpServer->listen(QHostAddress::Any, port))
    throw tr("Cannot set the server in listen mode: %1").arg(mTcpServer->errorString());
  connect(mWebSocketServer, &QWebSocketServer::newConnection, this, &WbStreamingServer::onNewWebSocketConnection);
  connect(mTcpServer, &WbStreamingTcpServer::newConnection, this, &WbStreamingServer::onNewTcpConnection);
  connect(WbSimulationState::instance(), &WbSimulationState::controllerReadRequestsCompleted, this,
          &WbStreamingServer::sendUpdatePackageToClients, Qt::UniqueConnection);
  connect(WbLog::instance(), &WbLog::logEmitted, this, &WbStreamingServer::propagateWebotsLogToClients);
}

void WbStreamingServer::destroy() {
  disconnect(WbSimulationState::instance(), &WbSimulationState::controllerReadRequestsCompleted, this,
             &WbStreamingServer::sendUpdatePackageToClients);
  disconnect(WbLog::instance(), &WbLog::logEmitted, this, &WbStreamingServer::propagateWebotsLogToClients);

  if (mWebSocketServer)
    mWebSocketServer->close();

  foreach (QWebSocket *client, mWebSocketClients) {
    disconnect(client, &QWebSocket::textMessageReceived, this, &WbStreamingServer::processTextMessage);
    disconnect(client, &QWebSocket::disconnected, this, &WbStreamingServer::socketDisconnected);
  };
  qDeleteAll(mWebSocketClients);
  mWebSocketClients.clear();

  delete mWebSocketServer;
  mWebSocketServer = NULL;

  delete mTcpServer;
  mTcpServer = NULL;
}

void WbStreamingServer::closeClient(const QString &clientID) {
  foreach (QWebSocket *client, mWebSocketClients) {
    if (clientToId(client) == clientID) {
      disconnect(client, &QWebSocket::textMessageReceived, this, &WbStreamingServer::processTextMessage);
      disconnect(client, &QWebSocket::disconnected, this, &WbStreamingServer::socketDisconnected);
      emit sendRobotWindowClientID(clientToId(client), NULL, "disconnected");
      mWebSocketClients.removeAll(client);
      client->deleteLater();
    }
  }
}

void WbStreamingServer::onNewTcpConnection() {
  QTcpSocket *socket = mTcpServer->nextPendingConnection();
  if (socket) {
    mWebSocketServer->handleConnection(socket);
    connect(socket, &QTcpSocket::readyRead, this, &WbStreamingServer::onNewTcpData);
  }
}

void WbStreamingServer::onNewTcpData() {
  QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());

  const QByteArray request = socket->peek(3);
  if (request != "GET")  // probably a WebSocket message
    return;
  const QString &line(socket->peek(8 * 1024));  // Peek the request header to determine the requested url.
  QStringList tokens = QString(line).split(QRegularExpression("[ \r\n][ \r\n]*"));
  if (tokens[0] == "GET" && tokens[1] != "/") {  // "/" is reserved for the websocket.
    const int hostIndex = tokens.indexOf("Host:") + 1;
    const QString host = hostIndex ? tokens[hostIndex] : "";
    const int etagIndex = tokens.indexOf("If-None-Match:") + 1;
    const QString etag = etagIndex ? tokens[etagIndex] : "";
    if (host.isEmpty())
      WbLog::warning(tr("No host specified in HTTP header."));
    sendTcpRequestReply(tokens[1].sliced(1), etag, host, socket);
  }
}

void WbStreamingServer::sendTcpRequestReply(const QString &completeUrl, const QString &etag, const QString &host,
                                            QTcpSocket *socket) {
  const QString url = completeUrl.left(completeUrl.lastIndexOf('?'));
  if (WbHttpReply::mimeType(url).isEmpty()) {
    WbLog::warning(tr("Unsupported file type '/%2'").arg(url));
    socket->write(WbHttpReply::forge404Reply("/" + url));
    return;
  }
  QString filePath;
  static const QStringList streamerFiles = {"index.html", "setup_viewer.js", "style.css", "webots_icon.png"};
  if (url == "streaming")  // shortcut
    filePath = WbStandardPaths::resourcesWebPath() + "streaming_viewer/index.html";
  else if (streamerFiles.contains(url))
    filePath = WbStandardPaths::resourcesWebPath() + "streaming_viewer/" + url;
  else if (url.startsWith("robot_windows/generic/"))
    filePath = WbStandardPaths::resourcesProjectsPath() + "plugins/" + url;
  else if (url.startsWith("robot_windows/"))
    filePath = WbProject::current()->pluginsPath() + url;
  else if (url.endsWith(".js") || url.endsWith(".css") || url.endsWith(".html"))
    filePath = WbStandardPaths::webotsHomePath() + url;
  socket->write(filePath.isEmpty() ? WbHttpReply::forge404Reply(url) : WbHttpReply::forgeFileReply(filePath, etag, host, url));
}

void WbStreamingServer::onNewWebSocketConnection() {
  QWebSocket *client = mWebSocketServer->nextPendingConnection();
  if (client) {
    connect(client, &QWebSocket::textMessageReceived, this, &WbStreamingServer::processTextMessage);
    connect(client, &QWebSocket::disconnected, this, &WbStreamingServer::socketDisconnected);
    mWebSocketClients << client;
    if (mStream)
      WbLog::info(tr("Streaming server: New client [%1] (%2 connected client(s)).")
                    .arg(clientToId(client))
                    .arg(mWebSocketClients.size()));
  }
}

void WbStreamingServer::sendFileToClient(QWebSocket *client, const QString &type, const QString &folder, const QString &path,
                                         const QString &filename) {
  QFile file(path + "/" + filename);
  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QString content;
    int numberOfLines = 0;
    while (!file.atEnd()) {
      content += file.readLine();
      numberOfLines++;
    }
    file.close();
    WbLog::info("Sending " + folder + "/" + filename + " " + type + " to web interface (" + QString::number(numberOfLines) +
                " lines).");
    const QString &answer =
      "set " + type + ":" + folder + "/" + filename + ":" + QString::number(numberOfLines) + "\n" + content;
    client->sendTextMessage(answer);
  }
}

void WbStreamingServer::processTextMessage(QString message) {
  QWebSocket *client = qobject_cast<QWebSocket *>(sender());

  if (message.startsWith("robot:")) {
    QString name;
    QString robotMessage;
    const QString &data = message.mid(6).trimmed();
    const QJsonDocument &jsonDocument = QJsonDocument::fromJson(data.toUtf8());
    if (jsonDocument.isNull() || !jsonDocument.isObject()) {
      // backward compatibility
      const int nameSize = data.indexOf(":");
      name = data.left(nameSize);
      robotMessage = data.mid(nameSize + 1);
    } else {
      name = jsonDocument.object().value("name").toString();
      robotMessage = jsonDocument.object().value("message").toString();
    }
    if (mStream)
      WbLog::info(tr("Streaming server: received robot message for %1: \"%2\".").arg(name).arg(robotMessage));

    if (robotMessage == "init robot window") {
      sendToClients();
      const int nameSize = data.indexOf(":");
      name = data.left(nameSize);
      emit sendRobotWindowClientID(clientToId(client), name, "connected");  // issue here, client?
    } else {
      const QList<WbRobot *> &robots = WbWorld::instance()->robots();
      const QByteArray &byteRobotMessage = robotMessage.toUtf8();
      foreach (WbRobot *const robot, robots)
        if (robot->name() == name) {
          robot->receiveFromJavascript(byteRobotMessage);
          break;
        }
    }
  } else if (mStream) {
    if (message == "pause") {
      disconnect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
                 &WbStreamingServer::propagateSimulationStateChange);
      WbSimulationState::instance()->setMode(WbSimulationState::PAUSE);
      connect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
              &WbStreamingServer::propagateSimulationStateChange);
      printf("pause\n");
      fflush(stdout);
      client->sendTextMessage("paused by client");
    } else if (message.startsWith("real-time:") or message.startsWith("fast:")) {
      const bool realTime = message.startsWith("real-time:");
      const double timeout = realTime ? message.mid(10).toDouble() : message.mid(5).toDouble();
      if (timeout >= 0)
        mPauseTimeout = WbSimulationState::instance()->time() + timeout;
      else
        mPauseTimeout = -1.0;
      disconnect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
                 &WbStreamingServer::propagateSimulationStateChange);
      if (realTime) {
        printf("real-time\n");
        WbSimulationState::instance()->setMode(WbSimulationState::REALTIME);
        client->sendTextMessage("real-time");
      } else {
        printf("fast\n");
        WbSimulationState::instance()->setMode(WbSimulationState::FAST);
        client->sendTextMessage("fast");
      }
      connect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
              &WbStreamingServer::propagateSimulationStateChange);
      fflush(stdout);
    } else if (message == "step") {
      disconnect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
                 &WbStreamingServer::propagateSimulationStateChange);
      WbSimulationState::instance()->setMode(WbSimulationState::STEP);
      printf("step\n");
      fflush(stdout);
      WbSimulationWorld::instance()->step();
      WbSimulationState::instance()->setMode(WbSimulationState::PAUSE);
      connect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
              &WbStreamingServer::propagateSimulationStateChange);
      printf("pause\n");
      fflush(stdout);
      client->sendTextMessage("paused by client");
    } else if (message.startsWith("timeout:")) {
      const double timeout = message.mid(8).toDouble();
      if (timeout >= 0)
        mPauseTimeout = WbSimulationState::instance()->time() + timeout;
      else
        mPauseTimeout = -1.0;
    } else if (message == "reset") {
      resetSimulation();
      sendToClients("reset finished");
    } else if (message == "reload")
      WbApplication::instance()->worldReload();
    else if (message.startsWith("load:")) {
      const QString &worldsPath = WbProject::current()->worldsPath();
      const QString &fullPath = worldsPath + '/' + message.mid(5);
      if (!QFile::exists(fullPath))
        WbLog::error(tr("Streaming server: world %1 doesn't exist.").arg(fullPath));
      else if (QDir(worldsPath) != QFileInfo(fullPath).absoluteDir())
        WbLog::error(tr("Streaming server: you are not allowed to open a world in another project directory."));
      else if (cMainWindow)
        cMainWindow->loadDifferentWorld(fullPath);
    } else
      WbLog::error(tr("Streaming server: Unsupported message: %1.").arg(message));
  }
}

void WbStreamingServer::socketDisconnected() {
  QWebSocket *client = qobject_cast<QWebSocket *>(sender());
  if (client) {
    emit sendRobotWindowClientID(clientToId(client), NULL, "disconnected");
    mWebSocketClients.removeAll(client);
    client->deleteLater();
    if (mStream)
      WbLog::info(tr("Streaming server: Client disconnected [%1] (remains %2 client(s)).")
                    .arg(clientToId(client))
                    .arg(mWebSocketClients.size()));
  }
}

void WbStreamingServer::sendUpdatePackageToClients() {
  if (mWebSocketClients.size() > 0) {
    const qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (mLastUpdateTime < 0.0 || currentTime - mLastUpdateTime >= 1000.0 / WbWorld::instance()->worldInfo()->fps()) {
      foreach (QWebSocket *client, mWebSocketClients)
        pauseClientIfNeeded(client);
      mLastUpdateTime = currentTime;
    }
  }
}

void WbStreamingServer::propagateControllerLogToClients(WbLog::Level level, const QString &message, bool popup) {
  propagateLogToClients(level, message);
}

bool WbStreamingServer::isControllerMessageIgnored(const QString &pattern, const QString &message) const {
  if (!QRegularExpression(pattern.arg(".+")).match(message).hasMatch())
    return false;

  return true;
}

void WbStreamingServer::propagateWebotsLogToClients(WbLog::Level level, const QString &message, bool popup) {
  if (message.startsWith("INFO: Streaming server") || level == WbLog::STATUS || level == WbLog::DEBUG)
    // do not propagate streaming server logs, status or debug messages
    return;

  // do not propagate start and exit messages coming from controllers
  if (isControllerMessageIgnored("^INFO: %1: Starting:", message) ||
      isControllerMessageIgnored("^INFO: '%1' controller", message))
    return;

  propagateLogToClients(level == WbLog::INFO ? WbLog::STDOUT : level, message);
}

void WbStreamingServer::propagateLogToClients(WbLog::Level level, const QString &message) {
  QString result;

  if (level == WbLog::STDOUT)
    result = "stdout:";
  else
    result = "stderr:";

  result += message;
  sendToClients(result);
}

void WbStreamingServer::sendToClients(const QString &message) {
  if (message.isEmpty()) {
    mClientsReadyToReceiveMessages = true;
    if (mMessageToClients.isEmpty())
      return;
  } else if (mMessageToClients.isEmpty())
    mMessageToClients = message;
  else
    mMessageToClients += "\n" + message;
  if (mWebSocketClients.isEmpty() || !mClientsReadyToReceiveMessages) {
    return;
  }
  foreach (QWebSocket *client, mWebSocketClients)
    client->sendTextMessage(mMessageToClients);
  mMessageToClients = "";
}

void WbStreamingServer::connectNewRobot(const WbRobot *robot) {
  connect(robot, &WbRobot::sendToJavascript, this, &WbStreamingServer::sendToJavascript);
}

bool WbStreamingServer::prepareWorld() {
  try {
    foreach (QWebSocket *client, mWebSocketClients)
      sendWorldToClient(client);
  } catch (const QString &e) {
    WbLog::error(tr("Error when reloading world: %1.").arg(e));
    destroy();
    return false;
  }

  return true;
}

void WbStreamingServer::newWorld() {
  if (mWebSocketServer == NULL)
    return;

  printf("open\n");
  fflush(stdout);

  if (!prepareWorld())
    return;
}

void WbStreamingServer::deleteWorld() {
  if (mWebSocketServer == NULL)
    return;
  foreach (QWebSocket *client, mWebSocketClients)
    client->sendTextMessage("delete world");
}

void WbStreamingServer::resetSimulation() {
  WbApplication::instance()->simulationReset(true);
  QCoreApplication::processEvents();  // this is required to make sure the simulation reset has been performed before sending
                                      // the update
  mLastUpdateTime = -1.0;
  mPauseTimeout = -1.0;
}

void WbStreamingServer::setWorldLoadingProgress(const int progress) {
  foreach (QWebSocket *client, mWebSocketClients) {
    client->sendTextMessage("loading:" + mCurrentWorldLoadingStatus + ":" + QString::number(progress));
    client->flush();
  }
}

void WbStreamingServer::propagateNodeAddition(WbNode *node) {
  if (mWebSocketServer == NULL || WbWorld::instance() == NULL)
    return;

  if (node->isProtoParameterNode()) {
    // PROTO parameter nodes are not exported to X3D or transmitted to webots.min.js
    foreach (WbNode *nodeInstance, node->protoParameterNodeInstances())
      propagateNodeAddition(nodeInstance);
    return;
  }

  const WbRobot *robot = dynamic_cast<WbRobot *>(node);
  if (robot)
    connectNewRobot(robot);
}

QString WbStreamingServer::simulationStateString(bool pauseTime) {
  switch (WbSimulationState::instance()->mode()) {
    case WbSimulationState::PAUSE:
      return pauseTime ? QString("pause: %1").arg(WbSimulationState::instance()->time()) : "pause";
    case WbSimulationState::REALTIME:
      return "real-time";
    case WbSimulationState::FAST:
      return "fast";
    default:
      return "";
  }
}

void WbStreamingServer::propagateSimulationStateChange() const {
  if (mWebSocketServer == NULL || WbWorld::instance() == NULL || mWebSocketClients.isEmpty())
    return;

  QString message = simulationStateString();
  if (message.isEmpty())
    return;
  foreach (QWebSocket *client, mWebSocketClients)
    client->sendTextMessage(message);
}

void WbStreamingServer::pauseClientIfNeeded(QWebSocket *client) {
  if (mPauseTimeout < 0 || WbSimulationState::instance()->time() < mPauseTimeout)
    return;

  disconnect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
             &WbStreamingServer::propagateSimulationStateChange);
  WbSimulationState::instance()->setMode(WbSimulationState::PAUSE);
  connect(WbSimulationState::instance(), &WbSimulationState::modeChanged, this,
          &WbStreamingServer::propagateSimulationStateChange);
  client->sendTextMessage(QString("pause: %1").arg(WbSimulationState::instance()->time()));
  printf("pause\n");
  fflush(stdout);
}

void WbStreamingServer::sendWorldToClient(QWebSocket *client) {
  const WbWorld *world = WbWorld::instance();
  const QDir dir = QFileInfo(world->fileName()).dir();
  const QStringList worldList = dir.entryList(QStringList() << "*.wbt", QDir::Files);
  QString worlds;
  for (int i = 0; i < worldList.size(); ++i)
    worlds += (i == 0 ? "" : ";") + QFileInfo(worldList.at(i)).fileName();
  client->sendTextMessage("world:" + QFileInfo(world->fileName()).fileName() + ':' + worlds);

  const QList<WbRobot *> &robots = WbWorld::instance()->robots();
  foreach (const WbRobot *robot, robots) {
    if (!robot->window().isEmpty()) {
      QJsonObject windowObject;
      windowObject.insert("robot", robot->name());
      windowObject.insert("window", robot->window());
      const QJsonDocument windowDocument(windowObject);
      client->sendTextMessage("robot window: " + windowDocument.toJson(QJsonDocument::Compact));
    }
  }
  client->sendTextMessage("scene load completed");
}
