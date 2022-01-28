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

#include "WbProtoList.hpp"

#include "../nodes/utils/WbDownloader.hpp"
#include "WbLog.hpp"
#include "WbNode.hpp"
#include "WbParser.hpp"
#include "WbPreferences.hpp"
#include "WbProtoModel.hpp"
#include "WbStandardPaths.hpp"
#include "WbTokenizer.hpp"
#include "WbVrmlWriter.hpp"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QRegularExpression>
#include <chrono>
#include <thread>

WbProtoList *gCurrent = NULL;
QFileInfoList WbProtoList::gResourcesProtoCache;
QFileInfoList WbProtoList::gProjectsProtoCache;
QFileInfoList WbProtoList::gExtraProtoCache;

WbProtoList *WbProtoList::current() {
  return gCurrent;
}

/*
WbProtoList::WbProtoList(const QString &primarySearchPath) {
  gCurrent = this;
  mPrimarySearchPath = primarySearchPath;

  static bool firstCall = true;
  if (firstCall) {
    updateResourcesProtoCache();
    updateProjectsProtoCache();
    updateExtraProtoCache();
    firstCall = false;
  }

  updatePrimaryProtoCache();
}
*/

WbProtoList::WbProtoList(const QString &path) {
  printf("WbProtoList::WbProtoList()\n");
  // if first call, open all worlds in projects folder and do shallow retrieval
  static bool firstCall = true;
  if (firstCall) {
    mDownloader = NULL;
    firstCall = false;
  }

  // open path (world), retrieve extern, kickoff download (separate recursive function)
  mDownloadingFiles = 0;

  mWorldName = path;
  downloadExternProto(path, WbStandardPaths::webotsTmpProtoPath());

  // recursivelyRetrieveExternProto(path, QString());
  // while (mDownloadingFiles > 0)
  //  std::this_thread::sleep_for(std::chrono::microseconds(100));
}

// we do not delete the PROTO models here: each PROTO model is automatically deleted when its last PROTO instance is deleted
WbProtoList::~WbProtoList() {
  if (gCurrent == this)
    gCurrent = NULL;

  foreach (WbProtoModel *model, mModels)
    model->unref();

  clearProtoSearchPaths();
}

void WbProtoList::findProtosRecursively(const QString &dirPath, QFileInfoList &protoList, bool inProtos) {
  QDir dir(dirPath);
  if (!dir.exists() || !dir.isReadable())
    // no PROTO nodes
    return;

  // search in folder
  if (inProtos) {
    QStringList filter("*.proto");
    protoList.append(dir.entryInfoList(filter, QDir::Files, QDir::Name));
  }
  // search in subfolders
  QFileInfoList subfolderInfoList = dir.entryInfoList(QDir::AllDirs | QDir::NoSymLinks | QDir::NoDotAndDotDot);

  if (!inProtos) {
    // try to identify a project root folder
    foreach (QFileInfo subfolder, subfolderInfoList) {
      const QString &fileName = subfolder.fileName();
      if (fileName == "controllers" || fileName == "worlds" || fileName == "protos" || fileName == "plugins") {
        const QString protosPath = dirPath + "/protos";
        if (QFile::exists(protosPath))
          findProtosRecursively(protosPath, protoList, true);
        return;
      }
    }
  }
  foreach (QFileInfo subfolder, subfolderInfoList) {
    if (inProtos &&
        (subfolder.fileName() == "textures" || subfolder.fileName() == "icons" || subfolder.fileName() == "meshes")) {
      // skip any textures or icons subfolder inside a protos folder
      continue;
    }
    findProtosRecursively(subfolder.absoluteFilePath(), protoList, inProtos);
  }
}

void WbProtoList::updateResourcesProtoCache() {
  gResourcesProtoCache.clear();
  QFileInfoList protosInfo;
  findProtosRecursively(WbStandardPaths::resourcesProjectsPath(), protosInfo);
  gResourcesProtoCache << protosInfo;
}

void WbProtoList::updateProjectsProtoCache() {
  gProjectsProtoCache.clear();
  QFileInfoList protosInfo;
  findProtosRecursively(WbStandardPaths::projectsPath(), protosInfo);
  gProjectsProtoCache << protosInfo;
}

void WbProtoList::updateExtraProtoCache() {
  gExtraProtoCache.clear();
  QFileInfoList protosInfo;
  if (!WbPreferences::instance()->value("General/extraProjectsPath").toString().isEmpty())
    findProtosRecursively(WbPreferences::instance()->value("General/extraProjectsPath").toString(), protosInfo);
  gExtraProtoCache << protosInfo;
}

void WbProtoList::updatePrimaryProtoCache() {
  mPrimaryProtoCache.clear();

  if (mPrimarySearchPath.isEmpty())
    return;

  QFileInfoList protosInfo;
  findProtosRecursively(mPrimarySearchPath, protosInfo, mPrimarySearchPath.endsWith("protos"));
  mPrimaryProtoCache << protosInfo;
}

WbProtoModel *WbProtoList::readModel(const QString &fileName, const QString &worldPath, QStringList baseTypeList) const {
  WbTokenizer tokenizer;
  int errors = tokenizer.tokenize(fileName);
  if (errors > 0)
    return NULL;

  // TODO: should be moved elsewhere (WbParser), as this point might be reached while parsing a world too
  printf("readmodel (next word is %s)\n", tokenizer.peekWord().toUtf8().constData());
  WbParser parser(&tokenizer);

  // if (!parser.parseProtoInterface(worldPath))
  //  return NULL;

  tokenizer.rewind();
  bool prevInstantiateMode = WbNode::instantiateMode();
  try {
    WbNode::setInstantiateMode(false);
    WbProtoModel *model = new WbProtoModel(&tokenizer, worldPath, fileName, baseTypeList);
    WbNode::setInstantiateMode(prevInstantiateMode);
    return model;
  } catch (...) {
    WbNode::setInstantiateMode(prevInstantiateMode);
    return NULL;
  }
}

void WbProtoList::readModel(WbTokenizer *tokenizer, const QString &worldPath) {
  WbProtoModel *model = NULL;
  bool prevInstantiateMode = WbNode::instantiateMode();
  try {
    WbNode::setInstantiateMode(false);
    model = new WbProtoModel(tokenizer, worldPath);
    WbNode::setInstantiateMode(prevInstantiateMode);
  } catch (...) {
    WbNode::setInstantiateMode(prevInstantiateMode);
    return;
  }
  mModels.prepend(model);
  model->ref();
}

WbProtoModel *WbProtoList::customFindModel(const QString &modelName, const QString &worldPath, QStringList baseTypeList) {
  printf("WbProtoList::customFindModel\n");
  return NULL;

  foreach (WbProtoModel *model, mModels)
    if (model->name() == modelName)
      return model;

  QFileInfoList tmpProto;

  QDirIterator it(WbStandardPaths::webotsTmpProtoPath(), QStringList("*.proto"), QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    QFileInfo fi(it.next());
    tmpProto << fi;
    printf("-- found %s\n", fi.fileName().toUtf8().constData());
  }

  foreach (const QFileInfo &fi, tmpProto) {
    if (fi.baseName() == modelName) {
      WbProtoModel *model = readModel(fi.absoluteFilePath(), worldPath, baseTypeList);
      if (model == NULL)  // can occur if the PROTO contains errors
        return NULL;
      mModels << model;
      model->ref();
      return model;
    }
  }

  return NULL;
}

WbProtoModel *WbProtoList::findModel(const QString &modelName, const QString &worldPath, QStringList baseTypeList) {
  printf("WbProtoList::findModel\n");
  // see if model is already loaded
  foreach (WbProtoModel *model, mModels)
    if (model->name() == modelName)
      return model;

  QFileInfoList tmpProto;  // protos in Webots temporary folder (i.e added by EXTERNPROTO reference)
  foreach (const QString &path, WbStandardPaths::webotsTmpProtoPath())
    findProtosRecursively(path, tmpProto);  // TODO: works because folder in tmp is called "protos". No need to have list of
                                            // searchable paths for each primary proto if this is good enough
  printf("> done searching\n");

  foreach (const QFileInfo &fi, tmpProto) {
    if (fi.baseName() == modelName) {
      WbProtoModel *model = readModel(fi.absoluteFilePath(), worldPath, baseTypeList);
      if (model == NULL)  // can occur if the PROTO contains errors
        return NULL;
      mModels << model;
      model->ref();
      return model;
    }
  }

  return NULL;  // not found
}

QString WbProtoList::findModelPath(const QString &modelName) const {
  printf("WbProtoList::findModelPath\n");
  QFileInfoList availableProtoFiles;
  availableProtoFiles << mPrimaryProtoCache << gExtraProtoCache << gProjectsProtoCache << gResourcesProtoCache;

  foreach (const QFileInfo &fi, availableProtoFiles) {
    if (fi.baseName() == modelName)
      return fi.absoluteFilePath();
  }

  return QString();  // not found
}

QStringList WbProtoList::fileList() {
  QStringList list;
  foreach (WbProtoModel *model, gCurrent->mModels)
    list << model->fileName();
  return list;
}

QStringList WbProtoList::fileList(int cache) {
  QStringList list;

  QFileInfoList availableProtoFiles;
  switch (cache) {
    case RESOURCES_PROTO_CACHE:
      availableProtoFiles << gResourcesProtoCache;
      break;
    case PROJECTS_PROTO_CACHE:
      availableProtoFiles << gProjectsProtoCache;
      break;
    case EXTRA_PROTO_CACHE:
      availableProtoFiles << gExtraProtoCache;
      break;
    default:
      return list;
  }

  foreach (const QFileInfo &fi, availableProtoFiles)
    list.append(fi.baseName());

  return list;
}

void WbProtoList::clearProtoSearchPaths(void) {
  printf("> clearing proto search paths\n");
  mProtoSearchPaths.clear();
  // TODO: add current working project path by default (location of world file), others?
}

void WbProtoList::insertProtoSearchPath(const QString &path) {
  QDir dir(path);
  if (dir.exists() && !mProtoSearchPaths.contains(path))
    mProtoSearchPaths << path;

  printf("Searchable paths:\n");
  foreach (const QString &path, mProtoSearchPaths)
    printf("- %s\n", path.toUtf8().constData());
}

void WbProtoList::recursivelyRetrieveExternProto(const QString &filename, const QString &parent) {
  QFile file(filename);
  if (file.open(QIODevice::ReadOnly)) {
    const QString content = file.readAll();

    QRegularExpression re("EXTERNPROTO\\s([a-zA-Z0-9-_+]+)\\s\"(.*\\.proto)\"");  // TODO: test it more

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
      QRegularExpressionMatch match = it.next();
      if (match.hasMatch()) {
        const QString identifier = match.captured(1);
        const QString url = match.captured(2);
        if (!url.endsWith(identifier + ".proto")) {
          WbLog::error(tr("Malformed extern proto url. The identifier and url do not coincide.\n"));
          return;
        }

        printf("REGEX found >>%s<< >>%s<<\n", identifier.toUtf8().constData(), url.toUtf8().constData());

        // create directory for this proto

        QString rootPath = WbStandardPaths::webotsTmpProtoPath();
        if (!parent.isEmpty())
          rootPath += parent + "/";
        rootPath += identifier + "/";

        QFileInfo protoFile(rootPath + identifier + ".proto");

        if (!protoFile.exists()) {
          printf("> will download to: %s\n", identifier.toUtf8().constData());
          QDir dir;
          dir.mkpath(protoFile.absolutePath());
          printf("making dir %s\n", protoFile.absolutePath().toUtf8().constData());

          if (mDownloader != NULL && mDownloader->device() != NULL)
            delete mDownloader;
          mDownloader = new WbDownloader(this);
          mDownloader->download(QUrl(url), protoFile.filePath());
          mDownloadingFiles++;
          connect(mDownloader, &WbDownloader::complete, this, &WbProtoList::protoRetrieved);
        } else
          printf("> %s already exists in tmp\n", identifier.toUtf8().constData());
      }
    }
  } else
    WbLog::error(tr("Could not open file: '%1'.").arg(filename));
}

void WbProtoList::protoRetrieved() {
  printf("Download completed\n");
  const QString parent = QFileInfo(mDownloader->mDestination).baseName();
  // recursivelyRetrieveExternProto(mDownloader->mDestination, parent);
  mDownloadingFiles--;
}

// logic

void WbProtoList::downloadExternProto(const QString &filename, const QString &parent) {
  qDeleteAll(mRetrievers);
  mRetrievers.clear();
  mToRetrieve = 0;

  connect(this, &WbProtoList::retrieved, this, &WbProtoList::tracker);
  recursiveProtoRetrieval(filename, parent);
}

QVector<QPair<QString, QString>> WbProtoList::getExternProto(const QString &filename) {
  QVector<QPair<QString, QString>> list;

  QFile file(filename);
  if (file.open(QIODevice::ReadOnly)) {
    const QString content = file.readAll();

    QRegularExpression re("EXTERNPROTO\\s([a-zA-Z0-9-_+]+)\\s\"(.*\\.proto)\"");  // TODO: test it more

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
      QRegularExpressionMatch match = it.next();
      if (match.hasMatch()) {
        QPair<QString, QString> pair;
        pair.first = match.captured(1);
        pair.second = match.captured(2);

        if (!pair.second.endsWith(pair.first + ".proto")) {
          WbLog::error(tr("Malformed extern proto url. The identifier and url do not coincide.\n"));
          return list;
        }

        printf(" > found >>%s<< >>%s<<\n", pair.first.toUtf8().constData(), pair.second.toUtf8().constData());
        list.push_back(pair);
      }
    }
  }

  return list;
}

void WbProtoList::recursiveProtoRetrieval(const QString &filename, const QString &parent) {
  printf("recursing on: %s with parent >%s<\n", filename.toUtf8().constData(), parent.toUtf8().constData());
  QVector<QPair<QString, QString>> externProtos = getExternProto(filename);

  for (int i = 0; i < externProtos.size(); ++i) {
    // create folder
    QString rootPath = parent;
    if (!parent.endsWith("/"))
      rootPath += "/";
    rootPath += externProtos[i].first + "/";

    QFileInfo protoFile(rootPath + externProtos[i].first + ".proto");
    QDir dir;
    dir.mkpath(protoFile.absolutePath());
    printf(" > making dir %s\n", protoFile.absolutePath().toUtf8().constData());

    // download
    printf(" > downloading: %s to %s\n", externProtos[i].first.toUtf8().constData(),
           protoFile.absoluteFilePath().toUtf8().constData());
    mRetrievers.push_back(new WbDownloader(this));
    mRetrievers.last()->download(QUrl(externProtos[i].second), protoFile.absoluteFilePath());
    mToRetrieve++;
    connect(mRetrievers.last(), &WbDownloader::complete, this, &WbProtoList::recurser);
  }
}

void WbProtoList::recurser() {
  WbDownloader *retriever = dynamic_cast<WbDownloader *>(sender());
  printf(" > download complete.\n");
  if (retriever) {
    const QString parent = QFileInfo(retriever->mDestination).absolutePath();
    printf("   : %s\n", parent.toUtf8().constData());
    recursiveProtoRetrieval(retriever->mDestination, parent);
    emit retrieved();
  }
}

void WbProtoList::tracker() {
  mToRetrieve--;

  if (mToRetrieve == 0) {
    printf("FINISHED\n");
    disconnect(this, &WbProtoList::retrieved, this, &WbProtoList::tracker);
  }
}