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

class WbDownloader;
class WbProtoTreeItem;

#include <QtCore/QMap>
#include <QtCore/QObject>

class WbProtoTreeItem : public QObject {
  Q_OBJECT
public:
  WbProtoTreeItem(const QString &url, WbProtoTreeItem *parent);
  ~WbProtoTreeItem();

  const QString &url() { return mUrl; }

  void downloadAssets();
  bool isReadyForLoad();

  void generateProtoMap(QMap<QString, QString> &map);

signals:
  void protoTreeUpdated();

protected:
  void parseItem();

protected slots:
  void downloadUpdate();

private:
  QString mUrl;
  bool mIsAvailable;  // is it cached or otherwise accessible
  // bool mIsParsed;            // has the file been parsed in order to define if it references any sub-proto
  WbProtoTreeItem *mParent;  // proto that references this sub-proto
  WbDownloader *mDownloader;
  QString mName;  // TODO: tmp, not really needed

  QList<WbProtoTreeItem *> mSubProto;  // list of referenced sub-proto
};
