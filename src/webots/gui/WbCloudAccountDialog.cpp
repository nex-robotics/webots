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

#include "WbCloudAccountDialog.hpp"
#include "WbMessageBox.hpp"

#include "WbLineEdit.hpp"

#include <QtCore/QStringList>

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

static QStringList gStartupModes;

WbCloudAccountDialog::WbCloudAccountDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Webots.cloud Account"));

  QWidget *widget = new QWidget(this);
  QGridLayout *cloudAccount = new QGridLayout(widget);
  QGroupBox *logIn = new QGroupBox(tr("Log in"), this);
  logIn->setObjectName("networkGroupBox");
  QGroupBox *signUp = new QGroupBox(tr("Sign up"), this);
  signUp->setObjectName("networkGroupBox");

  cloudAccount->addWidget(logIn, 0, 1);
  cloudAccount->addWidget(signUp, 1, 1);

  // log in
  QGridLayout *layout = new QGridLayout(logIn);

  // row 0
  mEmail = new WbLineEdit(this);
  mEmail->setFixedWidth(300);
  //mEmail->setText(WbCloudAccount::instance()->value("Account/email").toString());
  QLabel *emailLabel = new QLabel(tr("E-mail:"), this);
  layout->addWidget(emailLabel, 1, 0);
  layout->addWidget(mEmail, 1, 1);

  // row 1
  mPassword = new WbLineEdit(this);
  QLabel *passwordLabel = new QLabel(tr("Password:"), this);
  //mPassword->setText(WbCloudAccount::instance()->value("Account/password").toString());
  mPassword->setEchoMode(QLineEdit::PasswordEchoOnEdit);
  layout->addWidget(passwordLabel, 2, 0);
  layout->addWidget(mPassword, 2, 1);

  // row 2
  mRememberAccount = new QCheckBox(tr("Remember me"), this);
  mRememberAccount->setChecked(false);
  layout->addWidget(mRememberAccount, 3, 1);

  // row 2
  QPushButton *logInButton = new QPushButton(QString("Log in"), this);
  logInButton->setFixedWidth(emailLabel->width());
  // connect(logInButton, &QPushButton::pressed, this, &WbCloudAccountDialog::logIn);
  layout->addWidget(logInButton, 4, 1);
  connect(logInButton, &QPushButton::pressed, this, &WbCloudAccountDialog::logIn);

  // sign up
  layout = new QGridLayout(signUp);

  // row 0
  mEmail = new WbLineEdit(this);
  emailLabel = new QLabel(tr("E-mail:"), this);
  layout->addWidget(emailLabel, 1, 0);
  layout->addWidget(mEmail, 1, 1);

  // row 2
  QPushButton *signUpButton = new QPushButton(QString("Sign up"), this);
  signUpButton->setFixedWidth(emailLabel->width());
  // connect(signUpButton, &QPushButton::pressed, this, &WbCloudAccountDialog::signUp);
  layout->addWidget(signUpButton, 2, 1);
  connect(signUpButton, &QPushButton::pressed, this, &WbCloudAccountDialog::signUp);

  mButtonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);

  QVBoxLayout *mainLayout = new QVBoxLayout(this);
  mainLayout->addWidget(widget);
  mainLayout->addWidget(mButtonBox);
}

void WbCloudAccountDialog::signUp() {
  //if (WbCloudAccount::instance()->value("Account/email").toString())
  WbMessageBox::info(tr("Signing up..."), this);
}

void WbCloudAccountDialog::logIn() {
  //if (WbCloudAccount::instance()->value("Account/email").toString())
  WbMessageBox::info(tr("Logging in..."), this);
}
