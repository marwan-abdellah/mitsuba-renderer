/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2011 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "rendersettingsdlg.h"
#include "previewsettingsdlg.h"
#include "programsettingsdlg.h"
#include "sceneinfodlg.h"
#include "ui_wiscombeBRDFWidget.h"
#include "ui_hkBRDFWidget.h"
#include "ui_dipoleBSSRDFWidget.h"
#include "ui_multipoleBSSRDFWidget.h"
#include "ui_adipoleBSSRDFWidget.h"
#include "ui_shahRealtimeWidget.h"
#include "sceneloader.h"
#include "logwidget.h"
#include "aboutdlg.h"
#include "importdlg.h"
#include "loaddlg.h"
#include "updatedlg.h"
#include "server.h"
#include "save.h"
#include <QtNetwork>
#include <mitsuba/core/sched_remote.h>
#include <mitsuba/core/sstream.h>
#include <mitsuba/core/mstream.h>
#include <mitsuba/core/sshstream.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/fstream.h>
#include <QString>
#include "math/snowmath.h"
#include <QtGlobal>
#include <boost/filesystem/fstream.hpp>
#include "snowmaterialmanager.h"

#if !defined(WIN32)
#include <QX11Info>
#include <pwd.h>
#endif

#if defined(__OSX__)
#include "previewsettingsdlg_cocoa.h"
#else
#include "previewsettingsdlg.h"
#endif

static int localWorkerCtr = 0, remoteWorkerCtr = 0;

MainWindow::MainWindow(QWidget *parent) :
	QMainWindow(parent), ui(new Ui::MainWindow), 
	m_networkReply(NULL), m_activeWindowHack(false),
    m_wiscombeWidget(new QGroupBox()), m_wiscombeSettings(new Ui_WiscombeBRDFSettings()),
    m_hkWidget(new QGroupBox()), m_hkSettings(new Ui_HanrahanKruegerBRDFSettings()),
    m_dipoleWidget(new QGroupBox()), m_dipoleSettings(new Ui_DipoleBSSRDFSettings()),
    m_multipoleWidget(new QGroupBox()), m_multipoleSettings(new Ui_MultipoleBSSRDFSettings()),
    m_adipoleWidget(new QGroupBox()), m_adipoleSettings(new Ui_AdipoleBSSRDFSettings()),
    m_shahRTWidget(new QGroupBox()), m_shahRTSettings(new Ui_ShahRealtimeSettings()) {
	Logger *logger = Thread::getThread()->getLogger();

#if defined(__OSX__)
	m_previewSettings = NULL;
#endif

	QSettings settings("mitsuba-renderer.org", "qtgui");
	logger->setLogLevel((ELogLevel) settings.value("verbosity", EDebug).toInt());

	m_logWidget = new LogWidget(NULL);

	m_consoleAppender = new QConsoleAppender();
	logger->addAppender(m_consoleAppender);
	connect(m_consoleAppender, SIGNAL(textMessage(ELogLevel, const QString &)), 
		m_logWidget, SLOT(onTextMessage(ELogLevel, const QString &)), Qt::QueuedConnection);
	connect(m_consoleAppender, SIGNAL(criticalError(const QString &)), 
		m_logWidget, SLOT(onCriticalError(const QString &)), Qt::QueuedConnection);

	SLog(EInfo, "Mitsuba version " MTS_VERSION ", Copyright (c) " MTS_YEAR " Wenzel Jakob");

	m_currentChild = NULL;
	ui->setupUi(this);
	isActive();
	m_lastTab = NULL;
	ui->glView->setScrollBars(ui->hScrollBar, ui->vScrollBar);
	ui->hScrollBar->setVisible(false);
	ui->vScrollBar->setVisible(false);
	ui->actionUpdateCheck->setMenuRole(QAction::ApplicationSpecificRole);
	ui->actionFeedback->setMenuRole(QAction::ApplicationSpecificRole);
	ui->actionReportBug->setMenuRole(QAction::ApplicationSpecificRole);
	m_progressWidget = new QWidget(centralWidget());
	m_progressLabel = new QLabel(m_progressWidget);
	m_progress = new QProgressBar(m_progressWidget);
	m_progressWidget->setObjectName("progressWidget");
	QHBoxLayout *hlayout = new QHBoxLayout();
	hlayout->addWidget(m_progressLabel);
	hlayout->addWidget(m_progress);
	m_progressWidget->setLayout(hlayout);

#if defined(__OSX__)
	m_progressWidget->setStyleSheet("QWidget#progressWidget {background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
		"stop:0 palette(dark), stop: 1 palette(mid)); border-top: 1px solid palette(mid); margin: 0px; spacing: 15px; }");
	m_progress->setMaximumSize(SHRT_MAX,18);
	m_progress->setMinimumSize(10,18);
	hlayout->setContentsMargins(10, 8, 10, 3);
	m_progress->setAttribute(Qt::WA_MacSmallSize, true);
	m_progressLabel->setAttribute(Qt::WA_MacSmallSize, true);
#endif

	m_serverWidget = NULL;
	m_contextIndex = -1;

	for (int i = 0; i < MAX_RECENT_FILES; ++i) {
		m_actRecent[i] = new QAction(this);
		m_actRecent[i]->setVisible(false);
		connect(m_actRecent[i], SIGNAL(triggered()),
			this, SLOT(onOpenRecent()));
		ui->menuOpen_Recent->addAction(m_actRecent[i]);
	}

	m_clearRecent = new QAction(tr("Clear Menu"), this);
	connect(m_clearRecent, SIGNAL(triggered()),
		this, SLOT(onClearRecent()));
	ui->menuOpen_Recent->addAction(m_clearRecent);

#if defined(__OSX__)
	/* Make this the default menu bar */
	ui->menuBar->setParent(NULL);
#endif

	updateRecentFileActions();

	ui->tabBar->setDocumentMode(true);
	ui->tabBar->setTabsClosable(true);
	ui->tabBar->setMovable(true);
	ui->tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
	on_tabBar_currentChanged(-1);

	connect(ui->glView, SIGNAL(quit()), this, SLOT(on_actionExit_triggered()));
	connect(ui->glView, SIGNAL(beginRendering()), this, SLOT(on_actionRender_triggered()));
	connect(ui->glView, SIGNAL(stopRendering()), this, SLOT(updateUI()));
	connect(ui->glView, SIGNAL(statusMessage(const QString &)), this, SLOT(onStatusMessage(const QString &)));

	/* Load defaults from app settings file */
	ui->glView->setInvertMouse(settings.value("invertMouse", false).toBool());
	ui->glView->setMouseSensitivity(settings.value("mouseSensitivity", 3).toInt());
	ui->glView->setNavigationMode((ENavigationMode) settings.value("navMode", 
		EStandard).toInt());
	m_searchPaths = settings.value("searchPaths", QStringList()).toStringList();
	m_blockSize = settings.value("blockSize", 32).toInt();
	m_listenPort = settings.value("listenPort", MTS_DEFAULT_PORT).toInt();
	m_nodeName = settings.value("nodeName", getFQDN().c_str()).toString();

    // create render queue that manages incoming jobs one after another
	m_renderQueue = new RenderQueue(RenderQueue::ESerial);
	m_renderListener = new QRenderListener();
	m_renderQueue->registerListener(m_renderListener);

	connect(m_renderListener, SIGNAL(jobFinished(const RenderJob *, bool)), 
		this, SLOT(onJobFinished(const RenderJob *, bool)), Qt::QueuedConnection);
	connect(m_renderListener, SIGNAL(refresh(const RenderJob *, const Bitmap *)), 
		this, SLOT(onRefresh(const RenderJob *, const Bitmap *)), Qt::BlockingQueuedConnection);
	connect(m_renderListener, SIGNAL(workEnd(const RenderJob *, const ImageBlock *)), 
		this, SLOT(onWorkEnd(const RenderJob *, const ImageBlock *)), Qt::DirectConnection);
	connect(m_renderListener, SIGNAL(workBegin(const RenderJob *, const RectangularWorkUnit *, int)),
        this, SLOT(onWorkBegin(const RenderJob *, const RectangularWorkUnit *, int)), Qt::DirectConnection);
	connect(m_consoleAppender, 
		SIGNAL(progressMessage(const RenderJob *, const QString &, float, const QString &)), 
		this, SLOT(onProgressMessage(const RenderJob *, const QString &, float, const QString &)), 
		Qt::QueuedConnection);
	connect(this, SIGNAL(updateView()), ui->glView, SLOT(onUpdateView()));
	
	QPoint windowPos;
	if (settings.contains("pos")) {
		windowPos = settings.value("pos").toPoint();
	} else {
		QDesktopWidget *desktop = QApplication::desktop();
		windowPos = QPoint((desktop->width() - width()) / 2, (desktop->height() - height())/2);
	}

    /* shape properties */
    connect(ui->shapeComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSelectedShapeChanged(int)));
    connect(ui->shapeSnowCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onToggleSnowMaterial(int)));

    /* snow properties */
    connect(ui->snowtypeComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSnowTypeChanged(int)));
    connect(ui->grainsizeSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onGrainSizeChanged(double)));
    connect(ui->densitySpinBox, SIGNAL(valueChanged(double)), this, SLOT(onDensityChanged(double)));
    connect(ui->iorSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onIorChanged(double)));
    connect(ui->asymmetrySpinBox, SIGNAL(valueChanged(double)), this, SLOT(onAsymmetryFactorChanged(double)));
    connect(ui->calculationComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onCalculationTypeChanged(int)));
    connect(ui->ssAlbedoSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSsAlbedoChanged(double)));
    connect(ui->snowCoeffComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSnowComponents()));
    connect(ui->wl435SpinBox, SIGNAL(valueChanged(double)), this, SLOT(on435nmCoeffChanged(double)));
    connect(ui->wl545SpinBox, SIGNAL(valueChanged(double)), this, SLOT(on545nmCoeffChanged(double)));
    connect(ui->wl700SpinBox, SIGNAL(valueChanged(double)), this, SLOT(on700nmCoeffChanged(double)));

    /* snow render mode */
    m_shahRTSettings->setupUi(m_shahRTWidget);
    m_wiscombeSettings->setupUi(m_wiscombeWidget);
    m_hkSettings->setupUi(m_hkWidget);
    m_dipoleSettings->setupUi(m_dipoleWidget);
    m_multipoleSettings->setupUi(m_multipoleWidget);
    m_adipoleSettings->setupUi(m_adipoleWidget);

    ui->materialSettingsLayout->setAlignment(Qt::AlignTop);
    ui->materialSettingsLayout->addWidget(m_shahRTWidget);
    ui->materialSettingsLayout->addWidget(m_wiscombeWidget);
    ui->materialSettingsLayout->addWidget(m_hkWidget);
    ui->materialSettingsLayout->addWidget(m_dipoleWidget);
    ui->materialSettingsLayout->addWidget(m_multipoleWidget);
    ui->materialSettingsLayout->addWidget(m_adipoleWidget);

    connect(ui->renderModeComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(ui->surfaceComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(ui->subsurfaceComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSnowRenderModelChange()));

    
    connect(m_shahRTSettings->albedoComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->albedoPathButton, SIGNAL(pressed()), this, SLOT(onShahAlbedoMapPathRequest()));
    connect(m_shahRTSettings->diffusionProfileComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->expandSilhouetteCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->showSplatOriginsCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->showLightCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->rMaxSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->mcIterationsSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->mcRadioButton, SIGNAL(toggled(bool)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->rMaxRadioButton, SIGNAL(toggled(bool)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->specularSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->nSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->viewBufferWSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->viewBufferHSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_shahRTSettings->refreshSnowButton, SIGNAL(pressed()), this, SLOT(onRefreshShahSnowParameters()));
    connect(m_shahRTSettings->refreshSnowButton2, SIGNAL(pressed()), this, SLOT(onRefreshShahSnowParameters()));
    connect(m_shahRTSettings->weightSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));

    connect(m_wiscombeSettings->depthSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));

    connect(m_hkSettings->singleScatteringSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_hkSettings->multipleScatteringSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_hkSettings->multipleScatteringCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));

    connect(m_dipoleSettings->subsurfaceSizeSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->subsurfaceSampleFactorSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->singleScatteringCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->roughSurfaceCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->martelliDCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->textureCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onDipoleTextureSettingsChanged()));
    connect(m_dipoleSettings->textureZrButton, SIGNAL(pressed()), this, SLOT(onDipoleZrTextureLoad()));
    connect(m_dipoleSettings->textureSigmaTrButton, SIGNAL(pressed()), this, SLOT(onDipoleSigmaTrTextureLoad()));
    connect(m_dipoleSettings->textureUSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onDipoleTextureSettingsChanged()));
    connect(m_dipoleSettings->textureVSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onDipoleTextureSettingsChanged()));
    connect(m_dipoleSettings->irrDumpCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->irrDumpPathEdit, SIGNAL(textChanged(const QString&)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->irrDumpPathButton, SIGNAL(pressed()), this, SLOT(onDipoleIrrtrrDumpPathRequest()));
    connect(m_dipoleSettings->useLutCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->lutResolutionSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->lutIterationsSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->lutRMaxSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->lutMCRadioButton, SIGNAL(toggled(bool)), this, SLOT(onSnowRenderModelChange()));
    connect(m_dipoleSettings->lutRMaxRadioButton, SIGNAL(toggled(bool)), this, SLOT(onSnowRenderModelChange()));

    connect(m_multipoleSettings->subsurfaceSizeSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->subsurfaceSampleFactorSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->extraDipolesSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->slabThicknessSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->singleScatteringCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->martelliDCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->useLutCheckBox, SIGNAL(stateChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->lutResolutionSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->lutIterationsSpinBox, SIGNAL(valueChanged(int)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->lutRMaxSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->lutMCRadioButton, SIGNAL(toggled(bool)), this, SLOT(onSnowRenderModelChange()));
    connect(m_multipoleSettings->lutRMaxRadioButton, SIGNAL(toggled(bool)), this, SLOT(onSnowRenderModelChange()));

    connect(m_adipoleSettings->subsurfaceSizeSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_adipoleSettings->subsurfaceSampleFactorSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_adipoleSettings->sigmaTnSpinBox, SIGNAL(valueChanged(double)), this, SLOT(onSnowRenderModelChange()));
    connect(m_adipoleSettings->dLineEdit, SIGNAL(textEdited(const QString&)), this, SLOT(onSnowRenderModelChange()));

#if defined(__OSX__)
	ui->toolBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
	QToolButton *previewButton = static_cast<QToolButton *>(ui->toolBar->widgetForAction(ui->actionPreviewSettings));
	previewButton->setStyleSheet("margin-left: -5px; margin-right:-5px");
	
	/* Weird Qt/OSX bug -- moving while a window while it is invisible causes 
	   it to appear move up by 65 pixels (this is related to the unified toolbar) */
	move(windowPos + QPoint(0, 65));
#else
	move(windowPos);
#endif
	show();
	/* Move again just to be sure.. */
	move(windowPos);

	updateUI();
	adjustSize();

	m_networkManager = new QNetworkAccessManager(this);

#if defined(__OSX__)
	/* Submit crash reports on OSX */
	QDir crashDir = QDir::home();
	crashDir.cd("Library/Logs/CrashReporter");
	QFileInfoList crashReports = crashDir.entryInfoList(QStringList("mtsgui_*"), 
			QDir::Files, QDir::Name);

	if (crashReports.size() > 0) {
		if (QMessageBox::question(this, tr("Crash reports detected"),
			QString("<p>It appears that Mitsuba has crashed on a previous run!</p><p>"
				"%1 crash reports were found. Would you like to submit them?").arg(crashReports.size()),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
			bool failure = false;
			for (int i=0; i<crashReports.size(); ++i) {
				QFile file(crashReports[i].absoluteFilePath());
				if (!file.open(QIODevice::ReadOnly)) {
					failure = true;
					QMessageBox::critical(this, tr("Unable to read crash report"),
						tr("Unable to read crash report -- please check the file permissions in ~/Library/Logs/CrashReporter."), QMessageBox::Ok);
					break;
				}

				QString username("unknown");
				uid_t uid = getuid();
				struct passwd *info = getpwuid(uid);
				if (info) 
					username = QString(info->pw_name);

				QString boundary("5da85133908e2"); // some arbitrary value
				QByteArray data(QString("--" + boundary + "\r\n").toAscii());
				data.append(QString("Content-Disposition: form-data; name=\"bugreport\"; filename=\"%1\"\r\n").arg(file.fileName()));
				data.append("Content-Type: application/octet-stream\r\n\r\n");
				QString header = QString("Bug report from machine \"%1\", user \"%2\", filename \"%3\""
					", Mitsuba version " MTS_VERSION).arg(getFQDN().c_str()).arg(username).arg(file.fileName());
				data.append(header + "\r\n");
				for (int j=0; j<header.length(); j++)
					data.append('=');
				data.append("\r\n\r\n");
				data.append(file.readAll());
				data.append("\r\n--" + boundary + "--\r\n");

				QNetworkRequest reqHeader;
				reqHeader.setHeader(QNetworkRequest::ContentTypeHeader, QString("multipart/form-data; boundary=\"%1\"").arg(boundary));
				reqHeader.setHeader(QNetworkRequest::ContentLengthHeader, QString::number(data.length()));
				reqHeader.setUrl(QUrl("http://www.mitsuba-renderer.org/bugreporter.php"));
				m_bugStatus = 0;
				QNetworkReply *reply = m_networkManager->post(reqHeader, data);
				connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(onBugReportError()));
				connect(reply, SIGNAL(finished()), this, SLOT(onBugReportSubmitted()));
				while (m_bugStatus == 0) {
					qApp->processEvents();
					Thread::sleep(50);
				}
				if (m_bugStatus != 1) {
					failure = true;
					QMessageBox::critical(this, tr("Unable to submit"),
						tr("Unable to submit crash report -- are you connected to the internet?"), QMessageBox::Ok);
					break;
				}
				reply->deleteLater();
				if (!file.remove()) {
					failure = true;
					QMessageBox::critical(this, tr("Unable to delete submitted crash report"),
						tr("Unable to delete a submitted crash report -- please check the file permissions in ~/Library/Logs/CrashReporter."), QMessageBox::Ok);
					break;
				}
			}
			if (!failure)
				QMessageBox::information(this, tr("Crash reporter"),
					tr("All crash reports have been submitted. Thank you!"),
					QMessageBox::Ok);
		} else {
			for (int i=0; i<crashReports.size(); ++i) {
				QFile file(crashReports[i].absoluteFilePath());
				if (!file.remove()) {
					QMessageBox::critical(this, tr("Unable to submitted crash report"),
						tr("Unable to delete a crash report -- please check the file permissions in ~/Library/Logs/CrashReporter."), QMessageBox::Ok);
					break;
				}
			}
		}
	}
#endif

	if (ui->glView->isUsingSoftwareFallback())
		QMessageBox::warning(this, tr("Insufficient OpenGL capabilities"),
			ui->glView->getErrorString(), QMessageBox::Ok);

	connect(m_networkManager, SIGNAL(finished(QNetworkReply *)), this, SLOT(onNetworkFinished(QNetworkReply *)));
	m_checkForUpdates = settings.value("checkForUpdates", true).toBool();
	if (m_checkForUpdates)
		checkForUpdates(false);
}

MainWindow::~MainWindow() {
	m_renderQueue->unregisterListener(m_renderListener);
	ref<Scheduler> scheduler = Scheduler::getInstance();
	scheduler->pause();
	for (int i=0; i<m_connections.size(); ++i) {
		ServerConnection &c = m_connections[i];
		scheduler->unregisterWorker(c.worker);
	}
	if (m_networkReply)
		m_networkReply->abort();
#if defined(__OSX__)
	delete ui->menuBar;
#endif
    delete ui;
}

void MainWindow::initWorkers() {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	ref<Scheduler> scheduler = Scheduler::getInstance();
	int localWorkerCount = settings.value("localWorkers", getProcessorCount()).toInt();
	m_workerPriority = (Thread::EThreadPriority)
		settings.value("workerPriority", (int) Thread::ELowPriority).toInt();
	for (int i=0; i<localWorkerCount; ++i)
		scheduler->registerWorker(new LocalWorker(formatString("wrk%i", localWorkerCtr++), m_workerPriority));

	int networkConnections = 0;
	QList<QVariant> connectionData = settings.value("connections").toList();
	if (connectionData.size() > 0) {
		QDialog *dialog = new NonClosableDialog(this);
		dialog->setWindowModality(Qt::WindowModal);
		QVBoxLayout *layout = new QVBoxLayout(dialog);
		QLabel *label = new QLabel(tr("Establishing network connections .."), dialog);
		label->setAlignment(Qt::AlignCenter);
		layout->addWidget(label);
		QProgressBar *progressBar = new QProgressBar(dialog);
		progressBar->setTextVisible(false);
		dialog->resize(200, 50);
		layout->addWidget(progressBar);
		progressBar->setTextVisible(false);
		// weird, Qt/Win needs this to get a busy indicator
		progressBar->setValue(1);
		progressBar->setRange(0, 0);
		dialog->show();

		for (int i=0; i<connectionData.size(); ++i) {
			ServerConnection conn;
			conn.fromByteArray(connectionData[i].toByteArray());
			for (int j=0; j<10; ++j)
				qApp->processEvents();
			if (conn.createWorker(this)) {
				++networkConnections;
				conn.isRegistered = true;
				scheduler->registerWorker(conn.worker);
				m_connections.append(conn);
			}
		}

		/* Update, this removes connection failures */
		connectionData.clear();
		for (int i=0; i<m_connections.size(); ++i)
			connectionData.append(m_connections[i].toByteArray());
		settings.setValue("connections", connectionData);
	
		dialog->hide();
		delete dialog;
	}

	if (networkConnections + localWorkerCount == 0) {
		QMessageBox::warning(this, tr("Scheduler warning"),
			tr("There must be at least one worker thread -- forcing creation of one."),
			QMessageBox::Ok);
		scheduler->registerWorker(new LocalWorker(formatString("wrk%i", localWorkerCtr++), m_workerPriority));
	}

	QStringList args = qApp->arguments();
	for (int i=1; i<args.count(); ++i)
		loadFile(args[i]);

	scheduler->start();
	raise();
}


void MainWindow::adjustSize() {
	/* Like QWidget::adjustSize, but clamps the size to 4/5ths of the screen */
	ensurePolished();
    QSize s = sizeHint();

#if defined(Q_WS_X11)
        QRect screen = QApplication::desktop()->screenGeometry(x11Info().screen());
#else // all others
        QRect screen = QApplication::desktop()->screenGeometry(pos());
#endif

	s.setWidth(qMin(s.width(), screen.width()*4/5));
	s.setHeight(qMin(s.height(), screen.height()*4/5));
	layout()->activate();

    if (s.isValid())
        resize(s);
}

void MainWindow::checkForUpdates(bool manualUpdateCheck) {
	m_manualUpdateCheck = manualUpdateCheck;
	m_networkReply = m_networkManager->get(QNetworkRequest(QUrl("http://www.mitsuba-renderer.org/version")));
}

void MainWindow::onNetworkFinished(QNetworkReply *reply) {
	if (reply->error() == QNetworkReply::NoError) {
		try {
			QSettings settings("mitsuba-renderer.org", "qtgui");
			ProgramVersion remote(QString(reply->readAll()));
			ProgramVersion ignoredVersion(settings.value("ignoredVersion", "0.0.0").toString());
			ProgramVersion local(MTS_VERSION);

			if (local < remote) {
				if (!m_manualUpdateCheck && remote == ignoredVersion)
					return;
				UpdateDialog *dialog = new UpdateDialog(this, local, remote);
				dialog->setAttribute(Qt::WA_DeleteOnClose);
				dialog->setWindowModality(Qt::WindowModal);
				dialog->show();
			} else if (m_manualUpdateCheck) {
				QMessageBox::information(this, tr("Installed version is current"),
					QString("<p>You're up to date!</p>"
						"<p>Mitsuba <b>%1</b> is still the newest version available.</p>")
						.arg(local.toString()), QMessageBox::Ok);
			}
		} catch (const std::exception &e) {
			/* Got something weird and couldn't parse the version string -- 
			   very likely due to some Internet cafe login page. Ignore unless
			   a manual update check was done. */
			if (m_manualUpdateCheck)
				throw e;
		}
	} else {
		if (m_manualUpdateCheck)
			QMessageBox::warning(this, tr("Unable to determine the newest version"),
				QString("<p>Unable to determine the newest Mitsuba version.</p><p>"
					"Perhaps you are not connected to the Internet?</p>"),
					QMessageBox::Ok);
	}
}

void MainWindow::onBugReportError() {
	m_bugStatus = 2;
}

void MainWindow::onBugReportSubmitted() {
	if (m_bugStatus == 0)
		m_bugStatus = 1;
}

void MainWindow::on_actionImport_triggered() {
#if defined(MTS_HAS_COLLADA)
	ref<FileResolver> resolver = Thread::getThread()->getFileResolver();
	ref<FileResolver> newResolver = resolver->clone();
	for (int i=0; i<m_searchPaths.size(); ++i)
		newResolver->addPath(m_searchPaths[i].toStdString());

	ImportDialog *dialog = new ImportDialog(this, newResolver);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onImportDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
#else
	QMessageBox::critical(this, tr("Importer disabled"),
		tr("The importer is disabled in this build. To use it, you will need "
		"to install COLLADA-DOM and recompile Mitsuba -- please see the "
		"documentation for more details."), 
		QMessageBox::Ok);
#endif
}

void MainWindow::onImportDialogClose(int reason) {
	m_currentChild = NULL;
}

void MainWindow::on_actionNavigationControls_triggered() {

}

void MainWindow::on_actionDuplicateTab_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
	if (m_contextIndex != -1)
		currentIndex = m_contextIndex;
	SceneContext *currentContext = m_context[currentIndex];
	SceneContext *newContext = new SceneContext(currentContext);

	m_contextMutex.lock();
	m_context.append(newContext);
	m_contextMutex.unlock();
	ui->tabBar->addTab(newContext->shortName);

	int index = ui->tabBar->count()-1;
	if (ui->tabBar->currentIndex() != index)
		ui->tabBar->setCurrentIndex(index);
}

void MainWindow::on_actionUpdateCheck_triggered() {
	checkForUpdates(true);
}

void MainWindow::setNormalScaling(Float scaling) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
	currentContext->normalScaling = scaling;
    resetPreview(currentContext);
}

void MainWindow::resetPreview(SceneContext* context) {
	if (context->previewMethod != EOpenGL &&
		context->previewMethod != EOpenGLSinglePass &&
		context->previewMethod != EOpenGLRealtime)
		ui->glView->setPreviewMethod(EOpenGL);
	else
		ui->glView->resetPreview();
}

void MainWindow::onSnowTypeChanged(int index) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];

    if (index == 0)
        currentContext->snow.loadFreshNewSnowPreset();
    else if (index == 1)
        currentContext->snow.loadDryOlderSnowPreset();
    else if (index == 2)
        currentContext->snow.loadWetOldSnowPreset();
    else
        currentContext->snow.lastPreset = SnowProperties::ECustom;

    updateSnowComponents();
    resetPreview(currentContext);
}

void MainWindow::onGrainSizeChanged(double size) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
    currentContext->snow.grainsize = size / 1000; // mm -> m
    // recalculate coefficients, as they depend on grain size
    currentContext->snow.configure();
    resetPreview(currentContext);
    updateSnowComponents();
}

void MainWindow::onDensityChanged(double density) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
    currentContext->snow.density = density;
    // recalculate coefficients, as they depend on density
    currentContext->snow.configure();
    resetPreview(currentContext);
    updateSnowComponents();
}

void MainWindow::onIorChanged(double ior) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
    currentContext->snow.ior = ior;
    resetPreview(currentContext);
}

void MainWindow::onAsymmetryFactorChanged(double g) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
    currentContext->snow.g = g;
    resetPreview(currentContext);
}

void MainWindow::onCalculationTypeChanged(int index) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];

    if (index == 0)
        currentContext->snow.calcMode = SnowProperties::EPhenomenological;
    else if (index == 1)
        currentContext->snow.calcMode = SnowProperties::EAsymptotic;
    else if (index == 2)
        currentContext->snow.calcMode = SnowProperties::ESnowPack;
    else if (index == 3)
        currentContext->snow.calcMode = SnowProperties::ELargeParticle;
    else
        SLog(EWarn, "Unknown snow coefficient calculation mode encountered");

    currentContext->snow.ssOverride = false;
    // recalculate coefficients, as they depend on density
    currentContext->snow.configure();
    resetPreview(currentContext);
    updateSnowComponents();
}

void MainWindow::onSsAlbedoChanged(double ssAlbedo) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;

	SceneContext *currentContext = m_context[currentIndex];
    currentContext->snow.ssAlbedoOverride = ssAlbedo;
    currentContext->snow.ssOverride = true;

    currentContext->snow.configure();
    resetPreview(currentContext);
    updateSnowComponents();
}

void MainWindow::changeSnowCoefficient(int wlIndex, double value) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;

	SceneContext *currentContext = m_context[currentIndex];
    SnowProperties& snow = currentContext->snow;

    int coeff = ui->snowCoeffComboBox->currentIndex();
    if (coeff == 0) {
        /* absorbtion coefficient of snow */
        snow.sigmaA[wlIndex] = value;
        snow.sigmaT = snow.sigmaA + snow.sigmaS;
    } else if (coeff == 1) {
        /* scattering coefficient of snow */
        snow.sigmaS[wlIndex] = value;
        snow.sigmaT = snow.sigmaA + snow.sigmaS;
    } else if (coeff == 3) {
        /* absorbtion coefficient of ice */
        snow.iceSigmaA[wlIndex] = value;
    }
    resetPreview(currentContext);
}

void MainWindow::on435nmCoeffChanged(double coeff) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
    changeSnowCoefficient(2, coeff);
    updateSnowComponents();
}

void MainWindow::on545nmCoeffChanged(double coeff) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;

    changeSnowCoefficient(1, coeff);
    updateSnowComponents();
}

void MainWindow::on700nmCoeffChanged(double coeff) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;

    changeSnowCoefficient(0, coeff);
    updateSnowComponents();
}

void MainWindow::onSelectedShapeChanged(int shape) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1 || ui->shapeComboBox->count() == 0) 
		return;

	SceneContext *currentContext = m_context[currentIndex];

    int currentIdx = ui->shapeComboBox->currentIndex();
    Shape *currentShape =  ui->shapeComboBox->itemData(currentIdx).value<Shape *>();
    currentContext->currentlySelectedShape = currentShape;
    updateShapeComponents();
}

void MainWindow::onToggleSnowMaterial(int state) {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1 || ui->shapeComboBox->count() == 0) 
		return;

	SceneContext *currentContext = m_context[currentIndex];

    Shape *currentShape = currentContext->currentlySelectedShape;
    if (!currentShape)
        return;

    bool hasSnow = (state != 0);

    updateSnowOnShape(currentContext, currentShape, hasSnow);
    resetPreview(currentContext);
}

void MainWindow::onDipoleTextureSettingsChanged() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1 || ui->shapeComboBox->count() == 0) 
		return;

	SceneContext *context = m_context[currentIndex];
    SnowRenderSettings &srs = context->snowRenderSettings;

    const bool useTextures = m_dipoleSettings->textureCheckBox->isChecked();
    const bool usageChange = !(useTextures == srs.dipoleTexture);

    srs.dipoleTexture = useTextures;
    srs.dipoleTextureUScaling = m_dipoleSettings->textureUSpinBox->value();
    srs.dipoleTextureVScaling = m_dipoleSettings->textureVSpinBox->value();
    srs.dipoleZrTexture = m_dipoleSettings->textureZrEdit->text().toStdString();
    srs.dipoleSigmaTrTexture = m_dipoleSettings->textureSigmaTrEdit->text().toStdString();

    /* only update on checkbox state change */
    if (usageChange) {
        updateSnowOnAllShapes(context, true);
        resetPreview(context);
    }
}

void MainWindow::onDipoleZrTextureLoad() {
	QFileDialog *dialog = new QFileDialog(this, Qt::Sheet);
	dialog->setNameFilter(tr("EXR Images (*.exr)"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setAcceptMode(QFileDialog::AcceptOpen);
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setWindowModality(Qt::WindowModal);

	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onDipoleZrTextureOpenDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onDipoleSigmaTrTextureLoad() {
	QFileDialog *dialog = new QFileDialog(this, Qt::Sheet);
	dialog->setNameFilter(tr("EXR Images (*.exr)"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setAcceptMode(QFileDialog::AcceptOpen);
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setWindowModality(Qt::WindowModal);

	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onDipoleSigmaTrTextureOpenDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onDipoleZrTextureOpenDialogClose(int reason) {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;
	if (reason == QDialog::Accepted) {
	    int currentIndex = ui->tabBar->currentIndex();
    	if (currentIndex == -1)
	    	return;
    	SceneContext *context = m_context[currentIndex];

		QStringList fileNames = dialog->selectedFiles();
		settings.setValue("fileDialogState", dialog->saveState());
		settings.setValue("lastDipoleZrTexture", fileNames[0]);

        context->snowRenderSettings.dipoleZrTexture
            = fileNames[0].toStdString();
        updateSnowRenderingComponents();
	}
}

void MainWindow::onDipoleSigmaTrTextureOpenDialogClose(int reason) {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;
	if (reason == QDialog::Accepted) {
	    int currentIndex = ui->tabBar->currentIndex();
    	if (currentIndex == -1)
	    	return;
    	SceneContext *context = m_context[currentIndex];

		QStringList fileNames = dialog->selectedFiles();
		settings.setValue("fileDialogState", dialog->saveState());
		settings.setValue("lastDipoleSigmaTrTexture", fileNames[0]);

        context->snowRenderSettings.dipoleSigmaTrTexture
            = fileNames[0].toStdString();
        updateSnowRenderingComponents();
	}
}

void MainWindow::onDipoleIrrtrrDumpPathRequest() {
	QFileDialog *dialog = new QFileDialog(this, tr("Dump irradiance tree .."),
		"", tr("OBJ mesh (*.obj)"));

	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setAcceptMode(QFileDialog::AcceptSave);

#if defined(__OSX__)
	dialog->setOption(QFileDialog::DontUseNativeDialog, true);
#endif

	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowModality(Qt::WindowModal);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onDipoleIrrtrrDumpPathDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onDipoleIrrtrrDumpPathDialogClose(int reason) {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *ctx = m_context[currentIndex];

	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;

    if (reason == QDialog::Accepted) {
		settings.setValue("fileDialogState", dialog->saveState());
        QString fileName = dialog->selectedFiles().value(0);
        ctx->snowRenderSettings.dipoleDumpIrrtreePath = fileName.toStdString();
        updateSnowRenderingComponents();
    }
}

void MainWindow::onShahAlbedoMapPathRequest() {
	QFileDialog *dialog = new QFileDialog(this, tr("Load albedo map"),
		"", tr("PNG files (*.png)"));

	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->setViewMode(QFileDialog::Detail);

#if defined(__OSX__)
	dialog->setOption(QFileDialog::DontUseNativeDialog, true);
#endif

	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowModality(Qt::WindowModal);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onShahAlbedoMapPathDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onShahAlbedoMapPathDialogClose(int reason) {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *ctx = m_context[currentIndex];

	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;

    if (reason == QDialog::Accepted) {
		settings.setValue("fileDialogState", dialog->saveState());
        QString fileName = dialog->selectedFiles().value(0);
        ctx->snowRenderSettings.shahAlbedoMapCustomPath = fileName.toStdString();
        // reflect in GUI
        updateSnowRenderingComponents();
        loadShahCustomAlbedoMap();
    }
}

void MainWindow::loadShahCustomAlbedoMap() {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *ctx = m_context[currentIndex];

    SnowRenderSettings &srs = ctx->snowRenderSettings;
    
    if (boost::filesystem::exists(srs.shahAlbedoMapCustomPath)) {
        ref<FileStream> fs = new FileStream(srs.shahAlbedoMapCustomPath, FileStream::EReadOnly);
        ref<Bitmap> map = new Bitmap(Bitmap::EPNG, fs);
        if (map != NULL)
            srs.shahAlbedoMap = map;
    } else {
        std::cerr << "Could not find file \"" << srs.shahAlbedoMapCustomPath << "\"" << std::endl;
    }
}

/* Recalculates shah realtime rMax and diffusion profile based on snow
 * properties. */
void MainWindow::onRefreshShahSnowParameters() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *context = m_context[currentIndex];

    context->snowMaterialManager.refreshDiffusionProfile(context);
    
    std::pair< ref<Bitmap>, Float > data = context->snowMaterialManager.getCachedDiffusionProfile();
    context->snowRenderSettings.shahDiffusionProfile = data.first;
    context->snowRenderSettings.shahRmax = data.second;

    resetPreview(context);
}

void MainWindow::onSnowRenderModelChange() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *context = m_context[currentIndex];

    int generalIdx = ui->renderModeComboBox->currentIndex();
    int surfaceIdx = ui->surfaceComboBox->currentIndex();
    int subsurfaceIdx = ui->subsurfaceComboBox->currentIndex();

    // save proberties;
    SnowRenderSettings &srs = context->snowRenderSettings;

    EGeneralRenderMode generalRenderMode;
    if (generalIdx == 0) {
        generalRenderMode = ERealtime;
        context->previewMethod = EOpenGLRealtime;
    } else {
        generalRenderMode = EOffline;
        context->previewMethod = EOpenGL;
    }

    srs.generalRenderMode = generalRenderMode;

    ESurfaceRenderMode surfaceRenderMode;
    if (surfaceIdx == 1)
        surfaceRenderMode = EWiscombeWarrenAlbedo;
    else if (surfaceIdx == 2)
        surfaceRenderMode = EWiscombeWarrenBRDF;
    else if (surfaceIdx == 3)
        surfaceRenderMode = EHanrahanKruegerBRDF;
    else if (surfaceIdx == 4)
        surfaceRenderMode = EMicrofacetBRDF;
    else
        surfaceRenderMode = ENoSurface;

    srs.surfaceRenderMode = surfaceRenderMode;

    ESubSurfaceRenderMode subsurfaceRenderMode;
    if (subsurfaceIdx == 1)
        subsurfaceRenderMode = EJensenDipoleBSSRDF;
    else if (subsurfaceIdx == 2)
        subsurfaceRenderMode = EJensenMultipoleBSSRDF;
    else if (subsurfaceIdx == 3)
        subsurfaceRenderMode = EJakobADipoleBSSRDF;
    else
        subsurfaceRenderMode = ENoSubSurface;

    srs.subsurfaceRenderMode = subsurfaceRenderMode;

    /* actual material settings */

    SnowRenderSettings::EShahAlbedoType shahAlbedoType =
         static_cast<SnowRenderSettings::EShahAlbedoType>(m_shahRTSettings->albedoComboBox->currentIndex());

    bool shahAlbedoTypeChanged = (srs.shahAlbedoMapType != shahAlbedoType);
    srs.shahAlbedoMapType = shahAlbedoType;
    bool shahHasCustomAlbedo = false;
    if (shahAlbedoTypeChanged) {
        if (srs.shahAlbedoMapType == SnowRenderSettings::EWiscombeWarrenAlbedo) { // Wiscombe-Warren
            /* do nothing, this is handled in the preview class */ 
        } else if (srs.shahAlbedoMapType == SnowRenderSettings::EWhiteAlbedo) { // White
             /* Load a white albedo map */
            QResource res("/resources/snow/white.bmp");
            SAssert(res.isValid());
            ref<Stream> mStream = new MemoryStream(res.size());
            mStream->write(res.data(), res.size());
            mStream->setPos(0);
            ref<Bitmap> bitmap = new Bitmap(Bitmap::EBMP, mStream);
            srs.shahAlbedoMap = bitmap; 
        } else { // Custom
            shahHasCustomAlbedo = true;
            loadShahCustomAlbedoMap();
        }
    }
        
    static bool shahInitialized = false;
	int shahDiffusionExample = m_shahRTSettings->diffusionProfileComboBox->currentIndex();
    bool shahDiffusionProfileChanged = (srs.shahDiffusionExample != shahDiffusionExample) || !shahInitialized;
    srs.shahMCIterations = m_shahRTSettings->mcIterationsSpinBox->value() * 1000.0;
    srs.shahPredefineRmax = m_shahRTSettings->rMaxRadioButton->isChecked();
    srs.shahMaxLightViewResolution = m_shahRTSettings->nSpinBox->value();
    srs.shahBackbufferWidth = m_shahRTSettings->viewBufferWSpinBox->value();
    srs.shahBackbufferHeight = m_shahRTSettings->viewBufferHSpinBox->value();
    srs.shahWeight = m_shahRTSettings->weightSpinBox->value();

    SnowRenderSettings::EShahDiffusionPrType shahDiffusionProfile = 
        static_cast<SnowRenderSettings::EShahDiffusionPrType>( std::min(1, shahDiffusionExample) );

    Float rMax = m_shahRTSettings->rMaxSpinBox->value();
    bool rMaxManuallyChanged = (rMax != srs.shahRmax);

    if (shahDiffusionProfileChanged) {
		shahInitialized = true;
        srs.shahDiffusionExample = shahDiffusionExample;
        srs.shahDiffusionProfileType = shahDiffusionProfile;

        if (shahDiffusionProfile == SnowRenderSettings::ESnowProfile) {
            // Check if valid profile available and calculate one if not
            if (!context->snowMaterialManager.hasCachedDiffusionProfile()) {
                onRefreshShahSnowParameters();
            }

            std::pair< ref<Bitmap>, Float > data = context->snowMaterialManager.getCachedDiffusionProfile();
            srs.shahDiffusionProfile = data.first;
            rMax = data.second;
        } else {
            // load sample diffusion profile
            int exampleIdx = srs.shahDiffusionExample;
            std::ostringstream name; name << "/resources/snow/diffProfExample" << exampleIdx << ".bmp";
            QResource res(name.str().c_str());
            SAssert(res.isValid());
            ref<Stream> mStream = new MemoryStream(res.size());
            mStream->write(res.data(), res.size());
            mStream->setPos(0);
            ref<Bitmap> bitmap = new Bitmap(Bitmap::EBMP, mStream);
            srs.shahDiffusionProfile = bitmap;
            /* Set the appropriate maximum influence distance rMax.
             * But do it only if the user did not change the value himself */
            if (!rMaxManuallyChanged) {
                if (exampleIdx == 1 || exampleIdx == 2)
                    rMax = 0.09f;
                else if (exampleIdx == 2 || exampleIdx == 3)
                    rMax = 0.5f;
                else if (exampleIdx == 4)
                    rMax = 0.6f;
                else
                    rMax = 0.4f;
            }
        }
    }

    srs.shahRmax = rMax;
    srs.shahExpandSilhouette = m_shahRTSettings->expandSilhouetteCheckBox->isChecked();
    srs.shahShowSplatOrigins = m_shahRTSettings->showSplatOriginsCheckBox->isChecked();
    srs.shahShowLight = m_shahRTSettings->showLightCheckBox->isChecked();
    for (int i=0; i<SPECTRUM_SAMPLES; ++i)
        srs.shahSpecularColor[i] = m_shahRTSettings->specularSpinBox->value();

    m_shahRTSettings->albedoPathWidget->setEnabled(shahHasCustomAlbedo);

    srs.wiscombeDepth = m_wiscombeSettings->depthSpinBox->value();

    srs.hkSingleScatteringFactor = m_hkSettings->singleScatteringSpinBox->value();
    srs.hkMultipleScatteringFactor = m_hkSettings->multipleScatteringSpinBox->value();
    srs.hkUseMultipleScattering = m_hkSettings->multipleScatteringCheckBox->isChecked();

    srs.dipoleDensityFactor = m_dipoleSettings->subsurfaceSizeSpinBox->value();
    srs.dipoleSampleFactor = m_dipoleSettings->subsurfaceSampleFactorSpinBox->value();
    srs.dipoleUseSingleScattering = m_dipoleSettings->singleScatteringCheckBox->isChecked();
    srs.dipoleHasRoughSurface = m_dipoleSettings->roughSurfaceCheckBox->isChecked();
    srs.dipoleMartelliDC = m_dipoleSettings->martelliDCheckBox->isChecked();
    srs.dipoleDumpIrrtree = m_dipoleSettings->irrDumpCheckBox->isChecked();
    srs.dipoleDumpIrrtreePath = m_dipoleSettings->irrDumpPathEdit->text().toStdString();
    srs.dipoleUseLut = m_dipoleSettings->useLutCheckBox->isChecked();
    srs.dipoleLutResolution = m_dipoleSettings->lutResolutionSpinBox->value();
    srs.dipoleLutMCIterations = m_dipoleSettings->lutIterationsSpinBox->value();
    srs.dipoleLutRmax = m_dipoleSettings->lutRMaxSpinBox->value();
    srs.dipoleLutPredefineRmax = m_dipoleSettings->lutRMaxRadioButton->isChecked();

    srs.multipoleDensityFactor = m_multipoleSettings->subsurfaceSizeSpinBox->value();
    srs.multipoleSampleFactor = m_multipoleSettings->subsurfaceSampleFactorSpinBox->value();
    srs.multipoleExtraDipoles = m_multipoleSettings->extraDipolesSpinBox->value();
    srs.multipoleSlabThickness = m_multipoleSettings->slabThicknessSpinBox->value();
    srs.multipoleUseSingleScattering = m_multipoleSettings->singleScatteringCheckBox->isChecked();
    srs.multipoleMartelliDC = m_multipoleSettings->martelliDCheckBox->isChecked();
    srs.multipoleUseLut = m_multipoleSettings->useLutCheckBox->isChecked();
    srs.multipoleLutResolution = m_multipoleSettings->lutResolutionSpinBox->value();
    srs.multipoleLutRmax = m_multipoleSettings->lutRMaxSpinBox->value();
    srs.multipoleLutPredefineRmax = m_multipoleSettings->lutRMaxRadioButton->isChecked();

    srs.adipoleDensityFactor = m_adipoleSettings->subsurfaceSizeSpinBox->value();
    srs.adipoleSampleFactor = m_adipoleSettings->subsurfaceSampleFactorSpinBox->value();
    srs.adipoleSigmaTn = m_adipoleSettings->sigmaTnSpinBox->value();
    srs.adipoleD = m_adipoleSettings->dLineEdit->text().toStdString();

    /* In realtime mode there is no need for this kind of update.
     * There we only need to restart the preview. The material set
     * are not yet of relevance there. */ 
    if (generalRenderMode != ERealtime) {
        updateSnowOnAllShapes(context, true);
    }

    updateUI();
    resetPreview(context);
}

void MainWindow::updateSnowOnAllShapes(SceneContext *context, bool visible) {
    const shapeListType meshes = context->scene->getShapes();
    for (shapeListType::const_iterator it = meshes.begin(); it != meshes.end(); it++)
        if (context->snowMaterialManager.isMadeOfSnow(*it))
            updateSnowOnShape(context, *it, visible);
}

void MainWindow::on_actionShowKDTree_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
	bool checked = ui->actionShowKDTree->isChecked();
	currentContext->showKDTree = checked;
	if (currentContext->previewMethod != EOpenGL &&
		currentContext->previewMethod != EOpenGLSinglePass &&
		currentContext->previewMethod != EOpenGLRealtime)
		ui->glView->setPreviewMethod(EOpenGL);
	else
		ui->glView->resetPreview();
}

void MainWindow::on_actionSceneDescription_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *context= m_context[currentIndex];
	SceneInformationDialog *dialog = new SceneInformationDialog(this,
		context->scene);
	QDesktopWidget *desktop = QApplication::desktop();
	dialog->move(QPoint((desktop->width() - dialog->width()) / 2, (desktop->height() - dialog->height())/2));
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onSceneInformationClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onSceneInformationClose(int reason) {
	m_currentChild = NULL;
}

void MainWindow::on_actionShowNormals_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1)
		return;
	SceneContext *currentContext = m_context[currentIndex];
	bool checked = ui->actionShowNormals->isChecked();
	currentContext->showNormals = checked;
	if (currentContext->previewMethod != EOpenGL &&
		currentContext->previewMethod != EOpenGLSinglePass &&
		currentContext->previewMethod != EOpenGLRealtime)
		ui->glView->setPreviewMethod(EOpenGL);
	else
		ui->glView->resetPreview();
}

void MainWindow::on_actionSerialJobsRendering_triggered() {
    m_renderQueue->setManagedExecutionStrategy(RenderQueue::ESerial);
}

void MainWindow::on_actionSerialSubjobsRendering_triggered() {
    m_renderQueue->setManagedExecutionStrategy(RenderQueue::ETransparent);
}

void MainWindow::changeEvent(QEvent *e) {
    QMainWindow::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}
	
SceneContext *MainWindow::getContext(const RenderJob *job, bool failIfNotFound) {
	m_contextMutex.lock();
	for (int i=0; i<m_context.size(); ++i) {
		if (m_context[i]->renderJob == job) {
			m_contextMutex.unlock();
			return m_context[i];
		}
	}
	m_contextMutex.unlock();
	if (failIfNotFound)
		SLog(EError, "Internal error: could not find render context!");
	return NULL;
}

void MainWindow::onProgressMessage(const RenderJob *job, const QString &name, 
	float progress, const QString &eta) {
	SceneContext *context = getContext(job, false);
	if (context == NULL)
		return;
	context->eta = eta;
	context->progress = progress;
	context->progressName = name + ": ";
	updateUI();
}

void MainWindow::on_actionOpen_triggered() {
	QFileDialog *dialog = new QFileDialog(this, Qt::Sheet);
	dialog->setNameFilter(tr("All supported formats (*.xml *.exr);;"
			"Mitsuba scenes (*.xml);;EXR images (*.exr)"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setAcceptMode(QFileDialog::AcceptOpen);
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setWindowModality(Qt::WindowModal);
	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onOpenDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onOpenDialogClose(int reason) {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;
	if (reason == QDialog::Accepted) {
		QStringList fileNames = dialog->selectedFiles();
		settings.setValue("fileDialogState", dialog->saveState());
		for (int i=0; i<fileNames.size(); ++i)
			loadFile(fileNames[i]);
	}
}

void MainWindow::on_actionExit_triggered() {
	qApp->closeAllWindows();
}

void MainWindow::onOpenRecent() {
	QAction *action = qobject_cast<QAction *>(sender());
	if (action)
		loadFile(action->data().toString());
}

void MainWindow::onClearRecent() {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	for (int j = 0; j < MAX_RECENT_FILES; ++j)
		m_actRecent[j]->setVisible(false);
	settings.setValue("recentFileList", QStringList());
}

SceneContext *MainWindow::loadScene(const QString &qFileName) {
	ref<FileResolver> resolver = Thread::getThread()->getFileResolver();
	fs::path filename = resolver->resolve(qFileName.toStdString());
	fs::path filePath = fs::complete(filename).parent_path();
	ref<FileResolver> newResolver = resolver->clone();
	newResolver->addPath(filePath);
	for (int i=0; i<m_searchPaths.size(); ++i)
		newResolver->addPath(m_searchPaths[i].toStdString());

	ref<SceneLoader> loadingThread 
		= new SceneLoader(newResolver, filename.file_string());
	LoadDialog *loaddlg = new LoadDialog(this);
	loaddlg->setAttribute(Qt::WA_DeleteOnClose);
	loaddlg->setWindowModality(Qt::ApplicationModal);
	loaddlg->setWindowTitle("Loading ..");
	loaddlg->show();
	loadingThread->start();

	while (loadingThread->isRunning()) {
		QCoreApplication::processEvents();
		loadingThread->wait(20);
	}
	loadingThread->join();
	loaddlg->close();

	SceneContext *result = loadingThread->getResult();
	if (result == NULL) {
		QMessageBox::critical(this, tr("Unable to load %1").arg(qFileName),
			QString(loadingThread->getError().c_str()),
			QMessageBox::Ok);
	}

	return result;
}

void MainWindow::loadFile(QString filename) {
	QFileInfo fInfo(filename);
	fInfo.makeAbsolute();

	SceneContext *context = loadScene(filename);
	if (context == NULL) 
		return;
	m_contextMutex.lock();
	m_context.append(context);
	m_contextMutex.unlock();

	addRecentFile(context->fileName);

	ui->tabBar->addTab(context->shortName);

	/* Select the newly loaded scene */
	int index = ui->tabBar->count()-1;
	if (ui->tabBar->currentIndex() != index)
		ui->tabBar->setCurrentIndex(index);
	updateUI();
	adjustSize();
}

void MainWindow::addRecentFile(QString fileName) {
	/* Update recent files list */
	QSettings settings("mitsuba-renderer.org", "qtgui");
	QStringList files = settings.value("recentFileList").toStringList();
	files.removeAll(fileName);
	files.prepend(fileName);

	while (files.size() > MAX_RECENT_FILES)
		files.removeLast();

	settings.setValue("recentFileList", files);
	foreach (QWidget *widget, QApplication::topLevelWidgets()) {
		MainWindow *mainWin = qobject_cast<MainWindow *>(widget);
		if (mainWin)
			mainWin->updateRecentFileActions();
	}
}

void MainWindow::updateRecentFileActions() {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	QStringList files = settings.value("recentFileList").toStringList();

	int numRecentFiles = qMin(files.size(), MAX_RECENT_FILES);

	for (int i = 0; i < numRecentFiles; ++i) {
		m_actRecent[i]->setText(QFileInfo(files[i]).fileName());
		m_actRecent[i]->setData(files[i]);
		m_actRecent[i]->setVisible(true);
	}

	for (int j = numRecentFiles; j < MAX_RECENT_FILES; ++j)
		m_actRecent[j]->setVisible(false);
}

void MainWindow::updateUI() {
	int index = ui->tabBar->currentIndex();
	bool hasTab = (index != -1);

	SceneContext *context = hasTab ? m_context[index] : NULL;
	bool isRendering = hasTab ? context->renderJob != NULL : false;
	bool isShowingRendering = hasTab ? context->mode == ERender : false;
	bool hasScene = hasTab && context->scene != NULL;
	bool isInactiveScene = (hasTab && hasScene) ? context->renderJob == NULL : false;
	bool fallback = ui->glView->isUsingSoftwareFallback();

	ui->actionStop->setEnabled(isShowingRendering);
	if (isShowingRendering && !isRendering)
		ui->actionStop->setToolTip(tr("Return to the realtime preview"));
	else
		ui->actionStop->setToolTip(tr("Stop rendering"));

	ui->actionRender->setEnabled(isInactiveScene);
	ui->actionRefresh->setEnabled(isInactiveScene);
	ui->actionRenderSettings->setEnabled(isInactiveScene);
	ui->actionSave->setEnabled(hasScene);
	ui->actionSaveAs->setEnabled(hasScene);
	ui->actionExportImage->setEnabled(hasTab);
	ui->actionExportShape->setEnabled(hasScene);
	ui->actionClose->setEnabled(hasTab);
	ui->actionDuplicateTab->setEnabled(hasTab);
	ui->actionAdjustSize->setEnabled(hasTab);
	ui->actionShowKDTree->setEnabled(hasTab);
	ui->actionShowKDTree->setChecked(hasTab && context->showKDTree);
	ui->actionSceneDescription->setEnabled(hasTab);
	ui->actionShowNormals->setEnabled(hasTab);
	ui->actionShowNormals->setChecked(hasTab && context->showNormals);
    ui->menuAllShapes->setEnabled(hasScene);
#if !defined(__OSX__)
	ui->actionPreviewSettings->setEnabled(!fallback && hasTab);
#else
	bool isVisible = m_previewSettings != NULL && m_previewSettings->isVisible();
	ui->actionPreviewSettings->setEnabled(hasTab && !isVisible && !fallback);
#endif

    /* update shape related settings */
    updateShapeComponents();

    /* update snow related components */
    updateSnowComponents();

    /* update snow rendering components */
    updateSnowRenderingComponents();

	if (isRendering) {
		if (!m_progress->isVisible()) {
			QGridLayout *centralLayout = static_cast<QGridLayout *>(centralWidget()->layout());
			centralLayout->addWidget(m_progressWidget, 3, 0, 1, 3);
			m_progressWidget->show();
		}
		m_progress->setValue(context->progress);
		QString etaString;

		if (context->eta != "")
			etaString = QString(" (ETA: ") + context->eta  + QString(")");

		if (context->progressName != "")
			m_progressLabel->setText(QString("<b>%1</b> %2%%3")
				.arg(context->progressName)
				.arg(context->progress, 0, 'f', 1)
				.arg(etaString));
		else
			m_progressLabel->setText("");
	} else {
		if (m_progress->isVisible()) {
			QLayout *centralLayout = centralWidget()->layout();
			m_progressWidget->hide();
			centralLayout->removeWidget(m_progressWidget);
		}
	}
	centralWidget()->updateGeometry();
	layout()->activate();
}

void MainWindow::updateShapeComponents() {
	int index = ui->tabBar->currentIndex();
	bool hasTab = (index != -1);
    SceneContext *context = hasTab ? m_context[index] : NULL;
	bool hasScene = hasTab && context->scene != NULL;

    if (hasScene) {
        // block all signals of involved components
        ui->shapeComboBox->blockSignals(true);
        ui->shapeSnowCheckBox->blockSignals(true);

        ui->shapeComboBox->clear();
        /* update list of shapas */
        const shapeListType meshes = context->scene->getShapes();
        for (shapeListType::const_iterator it = meshes.begin(); it != meshes.end(); it++) {
            Shape *shape = *it;
            ui->shapeComboBox->addItem( QString::fromStdString(shape->getName()), qVariantFromValue(shape) );
        }

        /* try to select currently selected shape in combo box */
        Shape *currentShape = context->currentlySelectedShape;
        if (!currentShape && meshes.size() > 0)
                context->currentlySelectedShape = currentShape = meshes[0];

        if (currentShape) {
            /* find index of shape */
            int shapeIndex = -1;
            for (int i=0; i < ui->shapeComboBox->count(); ++i) {
                Shape *shape =  ui->shapeComboBox->itemData(i).value<Shape *>();
                if (shape == currentShape) {
                    shapeIndex = i;
                    break;
                }
            }
            if (shapeIndex > -1) {
                ui->shapeComboBox->setCurrentIndex(shapeIndex);
                ui->shapeSnowCheckBox->setChecked(context->snowMaterialManager.isMadeOfSnow(currentShape));
            } else {
                std::cerr << "MainWindow: This should not happen" << std::endl;
            }
        }
        // unblock all signals of involved components
        ui->shapeComboBox->blockSignals(false);
        ui->shapeSnowCheckBox->blockSignals(false);
    }

    ui->shapeComboBox->setEnabled(hasScene);
    ui->shapeLabel->setEnabled(hasScene);
    ui->shapeSnowCheckBox->setEnabled(hasScene);
    ui->flipNormalsCheckBox->setEnabled(hasScene);
}

void MainWindow::updateSnowComponents() {
	int index = ui->tabBar->currentIndex();
	bool hasTab = (index != -1);
    SceneContext *context = hasTab ? m_context[index] : NULL;
	bool hasScene = hasTab && context->scene != NULL;
   
    ui->snowtypeLabel->setEnabled(hasScene);
    ui->snowtypeComboBox->setEnabled(hasScene);
    ui->snowGeneralConfigGroup->setEnabled(hasScene);
    ui->snowInteractionGroup->setEnabled(hasScene);
    ui->snowInfoGroup->setEnabled(hasScene);

    if (!hasScene)
        return;

    SnowProperties& snow = context->snow;

    /* forbid feedback */
    ui->snowtypeComboBox->blockSignals(true);
    ui->grainsizeSpinBox->blockSignals(true);
    ui->densitySpinBox->blockSignals(true);
    ui->iorSpinBox->blockSignals(true);
    ui->asymmetrySpinBox->blockSignals(true);
    ui->calculationComboBox->blockSignals(true);
    ui->ssAlbedoSpinBox->blockSignals(true);
    ui->snowCoeffComboBox->blockSignals(true);
    ui->wl435SpinBox->blockSignals(true);
    ui->wl545SpinBox->blockSignals(true);
    ui->wl700SpinBox->blockSignals(true);

    ui->snowtypeComboBox->setCurrentIndex( (int) snow.lastPreset );

    ui->grainsizeSpinBox->setValue(snow.grainsize * 1000);
    ui->densitySpinBox->setValue(snow.density);
    ui->iorSpinBox->setValue(snow.ior);

    /* media interaction */
    ui->asymmetrySpinBox->setValue(snow.g);
    ui->calculationComboBox->setCurrentIndex( (int) snow.calcMode );
    ui->ssAlbedoSpinBox->setValue(snow.ssAlbedoOverride);

    int coeff = ui->snowCoeffComboBox->currentIndex();
    if (coeff == 0) {
        /* absorbtion coefficient of snow */
        ui->wl435SpinBox->setValue(snow.sigmaA[2]);
        ui->wl545SpinBox->setValue(snow.sigmaA[1]);
        ui->wl700SpinBox->setValue(snow.sigmaA[0]);
    } else if (coeff == 1) {
        /* scattering coefficient of snow */
        ui->wl435SpinBox->setValue(snow.sigmaS[2]);
        ui->wl545SpinBox->setValue(snow.sigmaS[1]);
        ui->wl700SpinBox->setValue(snow.sigmaS[0]);
    } else if (coeff == 2) {
        /* absorbtion coefficient of ice */
        ui->wl435SpinBox->setValue(snow.sigmaT[2]);
        ui->wl545SpinBox->setValue(snow.sigmaT[1]);
        ui->wl700SpinBox->setValue(snow.sigmaT[0]);
    } else if (coeff == 3) {
        /* absorbtion coefficient of ice */
        ui->wl435SpinBox->setValue(snow.iceSigmaA[2]);
        ui->wl545SpinBox->setValue(snow.iceSigmaA[1]);
        ui->wl700SpinBox->setValue(snow.iceSigmaA[0]);
    } else {
        std::cerr << "[GUI] Encountered unknown coefficient selection" << std::endl;
    }

    bool enabled = (coeff == 0) || (coeff == 1);
    ui->wl435SpinBox->setEnabled(enabled);
    ui->wl545SpinBox->setEnabled(enabled);
    ui->wl700SpinBox->setEnabled(enabled);

    /* info panels */
    Spectrum albedo = getAlbedo(snow.iceSigmaA, snow.grainsize);
    ui->albedoLabel->setText(QString::number(albedo.average(), 'g', 2));
    ui->ssalbedoLabel->setText(QString::number(snow.singleScatteringAlbedo.average(), 'g', 2));
    Float mfp = 1.0f / snow.sigmaT.average();
    if (mfp > 0.1) {
        ui->mfpLabel->setText(QString::number(mfp, 'g', 2) + "m" );
    } else if (mfp > 0.01) {
        ui->mfpLabel->setText(QString::number(mfp * 100.0f, 'g', 2) + "cm" );
    } else {
        ui->mfpLabel->setText(QString::number(mfp * 1000.0f, 'g', 2) + "mm" );
    }
    bool showSsAlbedoInput = context->snow.calcMode == SnowProperties::EPhenomenological;
    ui->ssAlbedoSpinBox->setEnabled(showSsAlbedoInput);
    ui->ssAlbedoLabel->setEnabled(showSsAlbedoInput);

    ui->snowtypeComboBox->blockSignals(false);
    ui->grainsizeSpinBox->blockSignals(false);
    ui->densitySpinBox->blockSignals(false);
    ui->iorSpinBox->blockSignals(false);
    ui->asymmetrySpinBox->blockSignals(false);
    ui->calculationComboBox->blockSignals(false);
    ui->ssAlbedoSpinBox->blockSignals(false);
    ui->snowCoeffComboBox->blockSignals(false);
    ui->wl435SpinBox->blockSignals(false);
    ui->wl545SpinBox->blockSignals(false);
    ui->wl700SpinBox->blockSignals(false);
}

void MainWindow::updateSnowOnShape(SceneContext* context, Shape* shape, bool hasSnow, bool updateUi) {
	on_tabBar_currentChanged(-1);
	qApp->processEvents();

    if (hasSnow) {
        context->snowMaterialManager.replaceMaterial(shape, context);
    } else {
        context->snowMaterialManager.resetMaterial(shape, context);
    }

    if (updateUi) {
        /* expects allowed on control on context and preview data (i.e. should not be active) */
        /* Reset preview data */
        context->previewBuffer.vplSampleOffset = 0;
        context->pathLength = context->detectPathLength();
        int index = ui->tabBar->currentIndex();
        if (index != -1)
            on_tabBar_currentChanged(index);
    }
}

void MainWindow::blockRenderComponentsSignals(bool block) {
    ui->renderModeComboBox->blockSignals(block);
    ui->surfaceComboBox->blockSignals(block);
    ui->subsurfaceComboBox->blockSignals(block);
    m_shahRTSettings->albedoComboBox->blockSignals(block);
    m_shahRTSettings->diffusionProfileComboBox->blockSignals(block);
    m_shahRTSettings->expandSilhouetteCheckBox->blockSignals(block);
    m_shahRTSettings->showSplatOriginsCheckBox->blockSignals(block);
    m_shahRTSettings->showLightCheckBox->blockSignals(block);
    m_shahRTSettings->rMaxSpinBox->blockSignals(block);
    m_shahRTSettings->mcIterationsSpinBox->blockSignals(block);
    m_shahRTSettings->mcRadioButton->blockSignals(block);
    m_shahRTSettings->rMaxRadioButton->blockSignals(block);
    m_shahRTSettings->specularSpinBox->blockSignals(block);
    m_shahRTSettings->nSpinBox->blockSignals(block);
    m_shahRTSettings->viewBufferWSpinBox->blockSignals(block);
    m_shahRTSettings->viewBufferHSpinBox->blockSignals(block);
    m_shahRTSettings->weightSpinBox->blockSignals(block);
    m_wiscombeSettings->depthSpinBox->blockSignals(block);
    m_hkSettings->singleScatteringSpinBox->blockSignals(block);
    m_hkSettings->multipleScatteringSpinBox->blockSignals(block);
    m_hkSettings->multipleScatteringCheckBox->blockSignals(block);
    m_dipoleSettings->subsurfaceSizeSpinBox->blockSignals(block);
    m_dipoleSettings->subsurfaceSampleFactorSpinBox->blockSignals(block);
    m_dipoleSettings->singleScatteringCheckBox->blockSignals(block);
    m_dipoleSettings->roughSurfaceCheckBox->blockSignals(block);
    m_dipoleSettings->martelliDCheckBox->blockSignals(block);
    m_dipoleSettings->textureCheckBox->blockSignals(block);
    m_dipoleSettings->textureZrEdit->blockSignals(block);
    m_dipoleSettings->textureSigmaTrEdit->blockSignals(block);
    m_dipoleSettings->textureUSpinBox->blockSignals(block);
    m_dipoleSettings->textureVSpinBox->blockSignals(block);
    m_dipoleSettings->irrDumpCheckBox->blockSignals(block);
    m_dipoleSettings->irrDumpPathEdit->blockSignals(block);
    m_dipoleSettings->useLutCheckBox->blockSignals(block);
    m_dipoleSettings->lutResolutionSpinBox->blockSignals(block);
    m_dipoleSettings->lutIterationsSpinBox->blockSignals(block);
    m_dipoleSettings->lutRMaxSpinBox->blockSignals(block);
    m_dipoleSettings->lutRMaxRadioButton->blockSignals(block);
    m_dipoleSettings->lutMCRadioButton->blockSignals(block);
    m_multipoleSettings->subsurfaceSizeSpinBox->blockSignals(block);
    m_multipoleSettings->subsurfaceSampleFactorSpinBox->blockSignals(block);
    m_multipoleSettings->extraDipolesSpinBox->blockSignals(block);
    m_multipoleSettings->slabThicknessSpinBox->blockSignals(block);
    m_multipoleSettings->singleScatteringCheckBox->blockSignals(block);
    m_multipoleSettings->martelliDCheckBox->blockSignals(block);
    m_multipoleSettings->useLutCheckBox->blockSignals(block);
    m_multipoleSettings->lutResolutionSpinBox->blockSignals(block);
    m_multipoleSettings->lutRMaxSpinBox->blockSignals(block);
    m_multipoleSettings->lutRMaxRadioButton->blockSignals(block);
    m_multipoleSettings->lutMCRadioButton->blockSignals(block);
    m_adipoleSettings->subsurfaceSizeSpinBox->blockSignals(block);
    m_adipoleSettings->subsurfaceSampleFactorSpinBox->blockSignals(block);
    m_adipoleSettings->sigmaTnSpinBox->blockSignals(block);
    m_adipoleSettings->dLineEdit->blockSignals(block);
}

void MainWindow::updateSnowRenderingComponents() {
	int index = ui->tabBar->currentIndex();
	bool hasTab = (index != -1);
    SceneContext *context = hasTab ? m_context[index] : NULL;
	bool hasScene = hasTab && context->scene != NULL;
 
    ui->renderModeLabel->setEnabled(hasScene); 
    ui->renderModeComboBox->setEnabled(hasScene);
    ui->surfaceLabel->setEnabled(hasScene);
    ui->surfaceComboBox->setEnabled(hasScene);
    ui->subsurfaceLabel->setEnabled(hasScene);
    ui->subsurfaceComboBox->setEnabled(hasScene);

    /* Hide all material widgets */
    m_shahRTWidget->hide();
    m_wiscombeWidget->hide();
    m_hkWidget->hide();
    m_dipoleWidget->hide();
    m_multipoleWidget->hide();
    m_adipoleWidget->hide();
    
    if (!hasScene)
        return;

    // get current modes
    EGeneralRenderMode generalRenderMode
        = context->snowRenderSettings.generalRenderMode;
    ESurfaceRenderMode surfaceRenderMode
        = context->snowRenderSettings.surfaceRenderMode;
    ESubSurfaceRenderMode subsurfaceRenderMode
        = context->snowRenderSettings.subsurfaceRenderMode;

    int generalIdx = -1;
    int surfaceIdx = -1;
    int subsurfaceIdx = -1;

    // save proberties;
    if (generalRenderMode == ERealtime) {
        generalIdx = 0;
        m_shahRTWidget->show();
    } else if (generalRenderMode == EOffline) {
        generalIdx = 1;
    }
    bool offlineRendering = (generalIdx == 1);

    if (surfaceRenderMode == ENoSurface)
        surfaceIdx = 0;
    else if (surfaceRenderMode == EWiscombeWarrenAlbedo) {
        surfaceIdx = 1;
        if (offlineRendering) m_wiscombeWidget->show();
    } else if (surfaceRenderMode == EWiscombeWarrenBRDF) {
        surfaceIdx = 2;
        if (offlineRendering) m_wiscombeWidget->show();
    } else if (surfaceRenderMode == EHanrahanKruegerBRDF) {
        surfaceIdx = 3;
        if (offlineRendering) m_hkWidget->show();
    } else if (surfaceRenderMode == EMicrofacetBRDF) {
        surfaceIdx = 4;
        //if (offlineRendering) m_mfWidget->show();
    }

    if (subsurfaceRenderMode == ENoSubSurface)
        subsurfaceIdx = 0;
    else if (subsurfaceRenderMode == EJensenDipoleBSSRDF) {
        subsurfaceIdx = 1;
        if (offlineRendering) m_dipoleWidget->show();
    } else if (subsurfaceRenderMode == EJensenMultipoleBSSRDF) {
        subsurfaceIdx = 2;
        if (offlineRendering) m_multipoleWidget->show();
    } else if (subsurfaceRenderMode == EJakobADipoleBSSRDF) {
        subsurfaceIdx = 3;
        if (offlineRendering) m_adipoleWidget->show();
    }

    /* block signals to avoid endless loop */
    blockRenderComponentsSignals(true);

    /* set new data */
    SnowRenderSettings &srs = context->snowRenderSettings;

    if (generalIdx != -1)
        ui->renderModeComboBox->setCurrentIndex(generalIdx);
    if (surfaceIdx != -1)
        ui->surfaceComboBox->setCurrentIndex(surfaceIdx);
    if (subsurfaceIdx != -1)
        ui->subsurfaceComboBox->setCurrentIndex(subsurfaceIdx);

    ui->subsurfaceComboBox->setEnabled(offlineRendering);
    ui->subsurfaceLabel->setEnabled(offlineRendering);
    ui->surfaceComboBox->setEnabled(offlineRendering);
    ui->surfaceLabel->setEnabled(offlineRendering);

    // Shah
    m_shahRTSettings->albedoComboBox->setCurrentIndex( (int) srs.shahAlbedoMapType );
    m_shahRTSettings->albedoPathEdit->setText( QString::fromStdString(srs.shahAlbedoMapCustomPath ) );
    m_shahRTSettings->diffusionProfileComboBox->setCurrentIndex( srs.shahDiffusionExample );
    m_shahRTSettings->expandSilhouetteCheckBox->setChecked( srs.shahExpandSilhouette );
    m_shahRTSettings->showSplatOriginsCheckBox->setChecked( srs.shahShowSplatOrigins );
    m_shahRTSettings->showLightCheckBox->setChecked( srs.shahShowLight );
    m_shahRTSettings->mcIterationsSpinBox->setValue( srs.shahMCIterations / 1000.0 );
    m_shahRTSettings->rMaxSpinBox->setValue( srs.shahRmax );
    m_shahRTSettings->specularSpinBox->setValue( srs.shahSpecularColor.average() );
    m_shahRTSettings->refreshSnowButton->setEnabled(srs.shahDiffusionProfileType == SnowRenderSettings::ESnowProfile);
    m_shahRTSettings->mcRadioButton->setChecked( !srs.shahPredefineRmax );
    m_shahRTSettings->rMaxRadioButton->setChecked( srs.shahPredefineRmax );
    m_shahRTSettings->nSpinBox->setValue( srs.shahMaxLightViewResolution );
    m_shahRTSettings->viewBufferWSpinBox->setValue( srs.shahBackbufferWidth );
    m_shahRTSettings->viewBufferHSpinBox->setValue( srs.shahBackbufferHeight );
    m_shahRTSettings->weightSpinBox->setValue( srs.shahWeight );

    // Wiscombe
    Float wiscombeDepth = srs.wiscombeDepth;
    m_wiscombeSettings->depthSpinBox->setValue(wiscombeDepth);

    // Hanrahan-Krueger
    Float hkSingleScatteringFactor = srs.hkSingleScatteringFactor;
    Float hkMultipleScatteringFactor = srs.hkMultipleScatteringFactor;
    bool hkUseMultipleScattering = srs.hkUseMultipleScattering;

    m_hkSettings->singleScatteringSpinBox->setValue(hkSingleScatteringFactor);
    m_hkSettings->multipleScatteringSpinBox->setValue(hkMultipleScatteringFactor);
    m_hkSettings->multipleScatteringCheckBox->setChecked(hkUseMultipleScattering);

    // Jensen dipole
    Float dipoleDensityFactor = srs.dipoleDensityFactor;
    Float dipoleSampleFactor = srs.dipoleSampleFactor;
    bool dipoleUseSingleScattering = srs.dipoleUseSingleScattering;
    bool dipoleUseMartelliDC = srs.dipoleMartelliDC;
    QString dipoleZrTex = QString::fromStdString(srs.dipoleZrTexture);
    QString dipoleSigmaTrTex = QString::fromStdString(srs.dipoleSigmaTrTexture);

    m_dipoleSettings->subsurfaceSizeSpinBox->setValue(dipoleDensityFactor);
    m_dipoleSettings->subsurfaceSampleFactorSpinBox->setValue(dipoleSampleFactor);
    m_dipoleSettings->singleScatteringCheckBox->setChecked(dipoleUseSingleScattering);
    m_dipoleSettings->roughSurfaceCheckBox->setChecked(srs.dipoleHasRoughSurface);
    m_dipoleSettings->martelliDCheckBox->setChecked(dipoleUseMartelliDC);
    m_dipoleSettings->textureCheckBox->setChecked(srs.dipoleTexture);
    m_dipoleSettings->textureZrEdit->setText(dipoleZrTex);
    m_dipoleSettings->textureSigmaTrEdit->setText(dipoleSigmaTrTex);
    m_dipoleSettings->textureUSpinBox->setValue(srs.dipoleTextureUScaling);
    m_dipoleSettings->textureVSpinBox->setValue(srs.dipoleTextureVScaling);
    m_dipoleSettings->irrDumpCheckBox->setChecked(srs.dipoleDumpIrrtree);
    m_dipoleSettings->irrDumpPathEdit->setText(QString::fromStdString(srs.dipoleDumpIrrtreePath));
    m_dipoleSettings->useLutCheckBox->setChecked(srs.dipoleUseLut);
    m_dipoleSettings->lutResolutionSpinBox->setValue(srs.dipoleLutResolution);
    m_dipoleSettings->lutIterationsSpinBox->setValue(srs.dipoleLutMCIterations);
    m_dipoleSettings->lutRMaxSpinBox->setValue(srs.dipoleLutRmax);
    m_dipoleSettings->lutMCRadioButton->setChecked(!srs.dipoleLutPredefineRmax);
    m_dipoleSettings->lutRMaxRadioButton->setChecked(srs.dipoleLutPredefineRmax);

    // Jensen multipole
    Float multipoleDensityFactor = srs.multipoleDensityFactor;
    Float multipoleSampleFactor = srs.multipoleSampleFactor;
    int multipoleExtraDipoles = srs.multipoleExtraDipoles;
    Float multipoleSlabThickness = srs.multipoleSlabThickness;
    bool multipoleUseSingleScattering = srs.multipoleUseSingleScattering;
    bool multipoleUseMartelliDC = srs.multipoleMartelliDC;

    m_multipoleSettings->subsurfaceSizeSpinBox->setValue(multipoleDensityFactor);
    m_multipoleSettings->subsurfaceSampleFactorSpinBox->setValue(multipoleSampleFactor);
    m_multipoleSettings->extraDipolesSpinBox->setValue(multipoleExtraDipoles);
    m_multipoleSettings->slabThicknessSpinBox->setValue(multipoleSlabThickness);
    m_multipoleSettings->singleScatteringCheckBox->setChecked(multipoleUseSingleScattering );
    m_multipoleSettings->martelliDCheckBox->setChecked(multipoleUseMartelliDC);
    m_multipoleSettings->useLutCheckBox->setChecked(srs.multipoleUseLut);
    m_multipoleSettings->lutResolutionSpinBox->setValue(srs.multipoleLutResolution);
    m_multipoleSettings->lutRMaxSpinBox->setValue(srs.multipoleLutRmax);
    m_multipoleSettings->lutMCRadioButton->setChecked(!srs.multipoleLutPredefineRmax);
    m_multipoleSettings->lutRMaxRadioButton->setChecked(srs.multipoleLutPredefineRmax);

    // Jakob anisotropic multipole
    Float adipoleDensityFactor = srs.adipoleDensityFactor;
    Float adipoleSampleFactor = srs.adipoleSampleFactor;
    Float adipoleSigmaTn = srs.adipoleSigmaTn;
    QString adipoleD = QString::fromStdString(srs.adipoleD);

    m_adipoleSettings->subsurfaceSizeSpinBox->setValue(adipoleDensityFactor);
    m_adipoleSettings->subsurfaceSampleFactorSpinBox->setValue(adipoleSampleFactor);
    m_adipoleSettings->sigmaTnSpinBox->setValue(adipoleSigmaTn);
    m_adipoleSettings->dLineEdit->setText(adipoleD);

    /* unblock signals */
    blockRenderComponentsSignals(false);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
	QMainWindow::resizeEvent(event);
	ui->glView->updateScrollBars();
}
	
void MainWindow::on_tabBar_customContextMenuRequested(const QPoint &pt) {
	if (pt.isNull())
		return;
	int tabIndex = ui->tabBar->tabAt(pt);
	if (tabIndex == -1)
		return;
	m_contextIndex = tabIndex;
	QMenu menu(this);
	menu.addAction(ui->actionDuplicateTab);
	if (tabIndex == ui->tabBar->currentIndex())
		menu.addAction(ui->actionAdjustSize);
	menu.addAction(ui->actionClose);
	menu.exec(ui->tabBar->mapToGlobal(pt));
	m_contextIndex = -1;
}

void MainWindow::on_tabBar_tabMoved(int from, int to) {
	m_contextMutex.lock();
	m_context.move(from, to);
	m_contextMutex.unlock();
}

void MainWindow::on_tabBar_currentChanged(int index) {
	if (m_lastTab != NULL) 
		m_lastTab->windowSize = size();

	ui->glView->ignoreResizeEvents(true);
	if (ui->tabBar->currentIndex() != -1)
		ui->tabBar->show();
	else
		ui->tabBar->hide();
	ui->glView->ignoreResizeEvents(false);

	if (index != -1) {
		if (m_context[index] != m_lastTab) {
			ui->glView->setScene(m_context[index]);
#if defined(__OSX__)
			if (m_previewSettings)
				m_previewSettings->setContext(m_context[index]);
#endif
		}
	} else {
		ui->glView->setScene(NULL);
#if defined(__OSX__)
		if (m_previewSettings && m_previewSettings->isVisible())
			m_previewSettings->hide();
#endif
	}
	m_statusMessage = "";
	updateStatus();
	updateUI();

	if (index != -1) {
		const QSize &windowSize = m_context[index]->windowSize;
		if (windowSize.isValid()) {
#if defined(__LINUX__)
			int error = (sizeHint()-windowSize).height();
			if (error > 0 && error <= 5)
				resize(windowSize + QSize(0, error));
			else
				resize(windowSize);
#else
			resize(windowSize);
#endif
		} else {
			adjustSize();
		}

		m_lastTab = m_context[index];
	} else {
		adjustSize();
		m_lastTab = NULL;
	}
	ui->glView->setFocus();
}

void MainWindow::on_actionClose_triggered() {
	int index = ui->tabBar->currentIndex();
	if (m_contextIndex != -1)
		index = m_contextIndex;
	on_tabBar_tabCloseRequested(index);
}

void MainWindow::on_actionAdjustSize_triggered() {
	adjustSize();
}

bool MainWindow::on_tabBar_tabCloseRequested(int index) {
	SceneContext *context = m_context[index];
	bool hasScene = context->scene != NULL;

	if (context->renderJob != NULL) {
		QMessageBox box(QMessageBox::Question, tr("Really close?"), 
			tr("Rendering of scene \"%1\" is unfinished - all progress "
				"will be lost. Are you sure you want to continue?").arg(context->shortName),
			QMessageBox::Yes | QMessageBox::No, this,
			Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint | Qt::Sheet);
		box.setWindowModality(Qt::WindowModal);
		if (box.exec() == QMessageBox::No)
			return false;
	}

    if (hasScene) {
        /* Check if a shape is in use by another context. If not remole it from
         * snow material manager.
         */
        shapeListType shapes(context->scene->getShapes());
        std::map<Shape*, bool> removalCandidates;
        for (shapeListType::const_iterator it = shapes.begin(); it != shapes.end(); it++) {
            removalCandidates[*it] = true;
            for(QList<SceneContext *>::Iterator ctxIt = m_context.begin(); ctxIt != m_context.end(); ++ctxIt ) {
                SceneContext* testCtx = *ctxIt;
                if (testCtx != context && testCtx->scene != NULL) {
                    const shapeListType testShapes = testCtx->scene->getShapes();
                     /* look for shape in this context */
                     shapeListType::const_iterator findIt = std::find(testShapes.begin(), testShapes.end(), *it);
                     /* if found, don't delete it from material manager */
                     if (findIt != testShapes.end())
                         removalCandidates[*it] = false;
                }
            }
        }
        for (std::map<Shape*, bool>::const_iterator it = removalCandidates.begin();
                it != removalCandidates.end(); it++) {
            if (it->second)
                context->snowMaterialManager.removeShape(it->first);
        }
    }

	m_contextMutex.lock();
	m_context.removeAt(index);
	m_contextMutex.unlock();
	ui->tabBar->removeTab(index);
	if (context->renderJob != NULL) {
		context->renderJob->cancel();
		context->renderJob->join();
	}
	ui->glView->makeCurrent();
	delete context;
	return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
	QSettings settings("mitsuba-renderer.org", "qtgui");
	settings.setValue("pos", pos());
	for (int i=ui->tabBar->count()-1; i>=0; --i) {
		if (!on_tabBar_tabCloseRequested(i)) {
			event->ignore();
			return;
		}
	}
	ui->glView->shutdown();
	QMainWindow::closeEvent(event);
	m_logWidget->hide();
	Logger *logger = Thread::getThread()->getLogger();
	logger->removeAppender(m_consoleAppender);
	event->accept();
}

bool MainWindow::isActive() {
	if (isActiveWindow() || m_activeWindowHack)
		return true;
	else if (m_currentChild != NULL && m_currentChild->isActiveWindow())
		return true;
#if defined(__OSX__)
	if (m_previewSettings != NULL && m_previewSettings->isActiveWindow()) 
		return true;
#endif
	return false;
}

void MainWindow::drawHLine(SceneContext *ctx, int x1, int y, int x2, const float *color) {
	float *framebuffer = ctx->framebuffer->getFloatData();
	int fbOffset = (x1 + y*ctx->framebuffer->getWidth())*4;
	for (int x=x1; x<=x2; x++) {
		framebuffer[fbOffset] = color[0];
		framebuffer[fbOffset+1] = color[1];
		framebuffer[fbOffset+2] = color[2];
		fbOffset+=4;
	}
}

void MainWindow::drawVLine(SceneContext *ctx, int x, int y1, int y2, const float *color) {
	float *framebuffer = ctx->framebuffer->getFloatData();
	int width = ctx->framebuffer->getWidth(), fbOffset = (x + y1*width)*4;
	for (int y=y1; y<=y2; y++) {
		framebuffer[fbOffset] = color[0];
		framebuffer[fbOffset+1] = color[1];
		framebuffer[fbOffset+2] = color[2];
		fbOffset += width * 4;
	}
}

void MainWindow::on_actionRenderSettings_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *context = m_context[currentIndex];
	RenderSettingsDialog *dialog = new RenderSettingsDialog(this);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowModality(Qt::WindowModal);
	dialog->load(context);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onRenderSettingsClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onRenderSettingsClose(int reason) {
	RenderSettingsDialog *dialog = static_cast<RenderSettingsDialog *>(sender());
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *context = m_context[currentIndex];
	m_currentChild = NULL;
	if (reason == QDialog::Accepted) {
		Scene *scene = context->scene;
		scene->incRef();
		dialog->apply(context);
		ui->glView->setPathLength(context->detectPathLength());
		if (dialog->resolutionHasChanged()) {
			ui->glView->refreshScene();
			updateUI();
			adjustSize();
		}
		scene->decRef();
#if defined(__OSX__)
		if (m_previewSettings)
			m_previewSettings->setContext(context);
#endif
	}
}

void MainWindow::on_actionPreviewSettings_triggered() {
#if !defined(__OSX__)
	SceneContext *context = m_context[ui->tabBar->currentIndex()];
	PreviewSettingsDialog d(this, context, ui->glView->getRendererCapabilities());
	connect(&d, SIGNAL(pathLengthChanged(int)), ui->glView, SLOT(setPathLength(int)));
	connect(&d, SIGNAL(clampingChanged(Float)), ui->glView, SLOT(setClamping(Float)));
	connect(&d, SIGNAL(shadowMapResolutionChanged(int)), ui->glView, SLOT(setShadowMapResolution(int)));
	connect(&d, SIGNAL(gammaChanged(bool, Float)), ui->glView, SLOT(setGamma(bool, Float)));
	connect(&d, SIGNAL(exposureChanged(Float)), ui->glView, SLOT(setExposure(Float)));
	connect(&d, SIGNAL(reinhardKeyChanged(Float)), ui->glView, SLOT(setReinhardKey(Float)));
	connect(&d, SIGNAL(reinhardBurnChanged(Float)), ui->glView, SLOT(setReinhardBurn(Float)));
	connect(&d, SIGNAL(previewMethodChanged(EPreviewMethod)), ui->glView, SLOT(setPreviewMethod(EPreviewMethod)));
	connect(&d, SIGNAL(toneMappingMethodChanged(EToneMappingMethod)), ui->glView, SLOT(setToneMappingMethod(EToneMappingMethod)));
	connect(&d, SIGNAL(diffuseReceiversChanged(bool)), ui->glView, SLOT(setDiffuseReceivers(bool)));
	connect(&d, SIGNAL(diffuseSourcesChanged(bool)), ui->glView, SLOT(setDiffuseSources(bool)));
	connect(&d, SIGNAL(normalScalingChanged(Float)), this, SLOT(setNormalScaling(Float)));
	d.setMaximumSize(d.minimumSize());
	d.exec();
	QSettings settings("mitsuba-renderer.org", "qtgui");
	settings.setValue("preview_sRGB", context->srgb);
	settings.setValue("preview_gamma", context->gamma);
	settings.setValue("preview_exposure", context->exposure);
	settings.setValue("preview_shadowMapResolution", context->shadowMapResolution);
	settings.setValue("preview_reinhardKey", context->reinhardKey);
	settings.setValue("preview_reinhardBurn", context->reinhardBurn);
	settings.setValue("preview_clamping", context->clamping);
	settings.setValue("preview_method", context->previewMethod);
	settings.setValue("preview_toneMappingMethod", context->toneMappingMethod);
	settings.setValue("preview_diffuseReceivers", context->diffuseReceivers);
	settings.setValue("preview_diffuseSources", context->diffuseSources);
	settings.setValue("preview_normalScaling", context->normalScaling);
#else
	if (!m_previewSettings) {
		m_previewSettings = new PreviewSettingsDlg(this);
		connect(m_previewSettings, SIGNAL(pathLengthChanged(int)), ui->glView, SLOT(setPathLength(int)));
		connect(m_previewSettings, SIGNAL(clampingChanged(Float)), ui->glView, SLOT(setClamping(Float)));
		connect(m_previewSettings, SIGNAL(shadowMapResolutionChanged(int)), ui->glView, SLOT(setShadowMapResolution(int)));
		connect(m_previewSettings, SIGNAL(gammaChanged(bool, Float)), ui->glView, SLOT(setGamma(bool, Float)));
		connect(m_previewSettings, SIGNAL(exposureChanged(Float)), ui->glView, SLOT(setExposure(Float)));
		connect(m_previewSettings, SIGNAL(reinhardKeyChanged(Float)), ui->glView, SLOT(setReinhardKey(Float)));
		connect(m_previewSettings, SIGNAL(reinhardBurnChanged(Float)), ui->glView, SLOT(setReinhardBurn(Float)));
		connect(m_previewSettings, SIGNAL(previewMethodChanged(EPreviewMethod)), ui->glView, SLOT(setPreviewMethod(EPreviewMethod)));
		connect(m_previewSettings, SIGNAL(toneMappingMethodChanged(EToneMappingMethod)), ui->glView, SLOT(setToneMappingMethod(EToneMappingMethod)));
		connect(m_previewSettings, SIGNAL(close()), this, SLOT(onPreviewSettingsClose()));
		connect(m_previewSettings, SIGNAL(diffuseReceiversChanged(bool)), ui->glView, SLOT(setDiffuseReceivers(bool)));
		connect(m_previewSettings, SIGNAL(diffuseSourcesChanged(bool)), ui->glView, SLOT(setDiffuseSources(bool)));
		connect(m_previewSettings, SIGNAL(normalScalingChanged(Float)), this, SLOT(setNormalScaling(Float)));
	}
	SceneContext *ctx = NULL;
	if (ui->tabBar->currentIndex() != -1)
		ctx = m_context[ui->tabBar->currentIndex()];
	m_previewSettings->setContext(ctx);
	m_previewSettings->show();
	ui->actionPreviewSettings->setEnabled(false);
#endif
}

void MainWindow::onPreviewSettingsClose() {
	int index = ui->tabBar->currentIndex();
	bool hasTab = index != -1;
	ui->actionPreviewSettings->setEnabled(hasTab);
	if (hasTab) {
		SceneContext *context = m_context[index];
		QSettings settings("mitsuba-renderer.org", "qtgui");
		settings.setValue("preview_sRGB", context->srgb);
		settings.setValue("preview_gamma", context->gamma);
		settings.setValue("preview_exposure", context->exposure);
		settings.setValue("preview_shadowMapResolution", context->shadowMapResolution);
		settings.setValue("preview_reinhardKey", context->reinhardKey);
		settings.setValue("preview_reinhardBurn", context->reinhardBurn);
		settings.setValue("preview_clamping", context->clamping);
		settings.setValue("preview_method", context->previewMethod);
		settings.setValue("preview_toneMappingMethod", context->toneMappingMethod);
		settings.setValue("preview_diffuseSources", context->diffuseSources);
		settings.setValue("preview_diffuseReceivers", context->diffuseReceivers);
	}
}

void MainWindow::on_actionSettings_triggered() {
	Logger *logger = Thread::getThread()->getLogger();
	ref<Scheduler> sched = Scheduler::getInstance();
	std::vector<Worker *> localWorkers;
	
	if (m_renderQueue->getJobCount() != 0) {
		QMessageBox::warning(this, tr("Rendering in progress"),
			tr("The program settings cannot be changed while a rendering is in progress."),
			QMessageBox::Ok);
		return;
	}

	size_t workerCount = sched->getWorkerCount();
	for (size_t i=0; i<workerCount; ++i) {
		Worker *worker = sched->getWorker((int) i);
		if (worker->getClass()->derivesFrom(MTS_CLASS(LocalWorker)))
			localWorkers.push_back(worker);
	}

	ProgramSettingsDialog d(this);
	d.setWindowModality(Qt::ApplicationModal);
	d.setLogLevel(logger->getLogLevel());
	d.setWorkerPriority(m_workerPriority);
	d.setInvertMouse(ui->glView->getInvertMouse());
	d.setNavigationMode(ui->glView->getNavigationMode());
	d.setMouseSensitivity(ui->glView->getMouseSensitivity());
	d.setBlockSize(m_blockSize);
	d.setSearchPaths(m_searchPaths);
	d.setLocalWorkerCount(localWorkers.size());
	d.setConnections(m_connections);
	d.setCheckForUpdates(m_checkForUpdates);
	d.setListenPort(m_listenPort);
	d.setNodeName(m_nodeName);

	if (d.exec()) {
		QList<ServerConnection> &connections = d.getConnections();
		QList<QVariant> connectionData;
		for (int i=0; i<connections.size(); ++i)
			connectionData.append(connections[i].toByteArray());

		QSettings settings("mitsuba-renderer.org", "qtgui");
		settings.setValue("verbosity", d.getLogLevel());
		settings.setValue("invertMouse", d.getInvertMouse());
		settings.setValue("blockSize", d.getBlockSize());
		settings.setValue("searchPaths", d.getSearchPaths());
		settings.setValue("localWorkers", d.getLocalWorkerCount());
		settings.setValue("connections", connectionData);
		settings.setValue("checkForUpdates", d.getCheckForUpdates());
		settings.setValue("mouseSensitivity", d.getMouseSensitivity());
		settings.setValue("listenPort", d.getListenPort());
		settings.setValue("nodeName", d.getNodeName());
		settings.setValue("navMode", (int) d.getNavigationMode());
		settings.setValue("workerPriority", d.getWorkerPriority());

		logger->setLogLevel(d.getLogLevel());
		ui->glView->setInvertMouse(d.getInvertMouse());
		ui->glView->setMouseSensitivity(d.getMouseSensitivity());
		ui->glView->setNavigationMode(d.getNavigationMode());
		m_blockSize = d.getBlockSize();
		m_searchPaths = d.getSearchPaths();
		m_checkForUpdates = d.getCheckForUpdates();
		m_listenPort = d.getListenPort();
		m_nodeName = d.getNodeName();
		m_workerPriority = d.getWorkerPriority();

		bool localWorkersChanged = (int) localWorkers.size() != d.getLocalWorkerCount();

		if (localWorkersChanged || m_connections != d.getConnections()) {
			ref<Scheduler> sched = Scheduler::getInstance();
			sched->pause();
			while (d.getLocalWorkerCount() > (int) localWorkers.size()) {
				LocalWorker *worker = new LocalWorker(formatString("wrk%i", localWorkerCtr++), m_workerPriority);
				sched->registerWorker(worker);
				localWorkers.push_back(worker);
			}
			while (d.getLocalWorkerCount() < (int) localWorkers.size()) {
				Worker *worker = localWorkers.back();
				sched->unregisterWorker(worker);
				localWorkers.pop_back();
			}
			QList<ServerConnection> removeList, 
				&newConnections = d.getConnections();
			for (int i=0; i<m_connections.size(); ++i) {
				ServerConnection &c = m_connections[i];
				if (!newConnections.contains(c)) 
					removeList.append(c);
			}
			for (int i=0; i<newConnections.size(); ++i) {
				ServerConnection &c = newConnections[i];
				if (!m_connections.contains(c)) {
					sched->registerWorker(c.worker);
					c.isRegistered = true;
					c.worker->decRef();
					m_connections.append(c);
				}
			}
			for (int i=0; i<removeList.size(); ++i) {
				ServerConnection &c = removeList[i];
				sched->unregisterWorker(c.worker);
				m_connections.removeAll(c);
			}
			sched->start();
		}
	}
}


void MainWindow::on_actionStop_triggered() {
	SceneContext *context = m_context[ui->tabBar->currentIndex()];
	m_contextMutex.lock();
	context->workUnits.clear();
	if (context->renderJob) {
		context->cancelled = true;
		ref<RenderJob> renderJob = context->renderJob;
		m_contextMutex.unlock();
		renderJob->cancel();
	} else if (context->mode == ERender) {
		context->mode = EPreview;
		m_contextMutex.unlock();
	}
	ui->glView->resumePreview();

	updateUI();
}

void MainWindow::on_actionShowLog_triggered() {
	m_logWidget->show();
}

void MainWindow::on_actionRender_triggered() {
	int index = ui->tabBar->currentIndex();
	if (index == -1)
		return;
	SceneContext *context = m_context[index];
	if (context->renderJob != NULL)
		return;

	Scene *scene = context->scene;
	scene->setBlockSize(m_blockSize);

    /* Avoids a segfault that misteriously appears sometimes. */
    if (context->sceneResID != -1) {
        Scheduler::getInstance()->unregisterResource(context->sceneResID);
        context->sceneResID = Scheduler::getInstance()->registerResource(scene);
    }

	context->renderJob = new RenderJob("rend", scene, m_renderQueue, NULL, 
		context->sceneResID, -1, -1, false, true);
	context->cancelMode = ERender;
	if (context->mode != ERender)
		ui->glView->downloadFramebuffer();
	context->cancelled = false;
	context->progress = 0;
	context->eta = "";
	context->progressName = "";
	context->mode = ERender;
	m_statusMessage = "";
	updateUI();
	context->sizeIncrease = QSize(0, m_progressWidget->sizeHint().height());

#if defined(__LINUX__)
	/* Workaround: on Linux, a few pixels get lost somehow, which
	   otherwise causes an unnecessary scrollbar to appear */
	int error = (sizeHint()-size()).height() - context->sizeIncrease.height();
	if (error > 0 && error <= 5)
		resize(size() + context->sizeIncrease + QSize(0, error));
	else
		resize(size() + context->sizeIncrease);
#else
	resize(size() + context->sizeIncrease);
#endif

    std::cerr << context->snowMaterialManager.toString() << std::endl
              << context->snow.toString() << std::endl;
	updateStatus();
    m_renderQueue->managedExecution(context->renderJob);
}

void MainWindow::on_actionRefresh_triggered() {
	int index = ui->tabBar->currentIndex();
	if (index < 0)
		return;
	SceneContext *context = m_context[index];
	on_tabBar_currentChanged(-1);
	SceneContext *newContext = loadScene(context->fileName);
	if (newContext == NULL) 
		return;
	delete context;
	m_contextMutex.lock();
	m_context[index] = newContext;
	m_contextMutex.unlock();
	qApp->processEvents();
	on_tabBar_currentChanged(index);
}

inline float toSRGB(float value) {
	if (value < 0.0031308f)
		return 12.92f * value;
	return 1.055f * std::pow(value, 0.41666f) - 0.055f;
}

void MainWindow::on_actionExportImage_triggered() {
	QFileDialog *dialog = new QFileDialog(this, tr("Export image .."),
		"", tr("All supported formats (*.exr *.png *.jpg *.jpeg);;Linear EXR image (*.exr)"
			";; Tonemapped 8-bit image (*.png *.jpg *.jpeg)"));

	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setAcceptMode(QFileDialog::AcceptSave);

#if defined(__OSX__)
	dialog->setOption(QFileDialog::DontUseNativeDialog, true);
#endif

	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowModality(Qt::WindowModal);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onExportDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onExportDialogClose(int reason) {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *ctx = m_context[currentIndex];

	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;

    if (reason == QDialog::Accepted) {
        QString fileName = dialog->selectedFiles().value(0);
		Bitmap::EFileFormat format;
		settings.setValue("fileDialogState", dialog->saveState());

		if (fileName.endsWith(".exr")) {
			format = Bitmap::EEXR;
		} else if (fileName.endsWith(".png")) {
			format = Bitmap::EPNG;
		} else if (fileName.endsWith(".jpg") || fileName.endsWith(".jpeg")) {
			format = Bitmap::EJPEG;
		} else {
			SLog(EError, "Unknown file type -- the filename must end in either .exr, .png, .jpg, or .jpeg");
			return;
		}

		ref<FileStream> fs = new FileStream(qPrintable(fileName), 
			FileStream::ETruncReadWrite);

		if (ctx->mode == EPreview)
			ui->glView->downloadFramebuffer();

		if (format == Bitmap::EEXR) {
			ctx->framebuffer->save(format, fs);
		} else {
			int width = ctx->framebuffer->getWidth();
			int height = ctx->framebuffer->getHeight();
			ref<Bitmap> temp = new Bitmap(width, height, 32);
			float *source = ctx->framebuffer->getFloatData();
			uint8_t *target = temp->getData();
			float invGamma = 1.0f/(float) ctx->gamma;
			float exposure = std::pow(2.0f, (float) ctx->exposure);
			Float reinhardKey = 0;
			Float invWpSqr = std::pow((Float) 2, (Float) ctx->reinhardBurn);
			if (ctx->toneMappingMethod == EReinhard) {
				Float avgLogLuminance = 0;
				for (int y=0; y<height; ++y) {
					for (int x=0; x<width; ++x) {
						Spectrum spec;
						spec.fromLinearRGB(source[(y*width+x)*4+0], source[(y*width+x)*4+1], source[(y*width+x)*4+2]);
						avgLogLuminance += std::log(0.001f+spec.getLuminance());
					}
				}
				avgLogLuminance = std::exp(avgLogLuminance/(width*height));
				reinhardKey = ctx->reinhardKey / avgLogLuminance;
			}

			for (int y=0; y<height; ++y) {
				for (int x=0; x<width; ++x) {
					float r, g, b, a = source[3];

					if (ctx->toneMappingMethod == EGamma) {
						r = source[0]*exposure;
						g = source[1]*exposure;
						b = source[2]*exposure;
					} else {
						Spectrum spec;
						Float X, Y, Z;
						spec.fromLinearRGB(source[0], source[1], source[2]);
						spec.toXYZ(X, Y, Z);
						Float normalization = 1/(X + Y + Z);
						Float x = X*normalization, y = Y*normalization;
						Float Lp = Y * reinhardKey;
						Y = Lp * (1.0f + Lp*invWpSqr) / (1.0f + Lp);
						X = x * (Y/y); 
						Z = (Y/y) * (1.0 - x - y);
						spec.fromXYZ(X, Y, Z);
						Float rF, gF, bF;
						spec.toLinearRGB(rF, gF, bF);
						r = rF; g = gF; b = bF; 
					}

					if (ctx->srgb) {
						r = toSRGB(r);
						g = toSRGB(g);
						b = toSRGB(b);
					} else {
						r = std::pow(r, invGamma);
						g = std::pow(g, invGamma);
						b = std::pow(b, invGamma);
					}

					*target++ = (uint8_t) std::min(255, std::max(0, (int) (r*255)));
					*target++ = (uint8_t) std::min(255, std::max(0, (int) (g*255)));
					*target++ = (uint8_t) std::min(255, std::max(0, (int) (b*255)));
					*target++ = (uint8_t) std::min(255, std::max(0, (int) (a*255)));
					source += 4;
				}
			}
			temp->setGamma(ctx->srgb ? -1 : ctx->gamma);
			temp->save(format, fs);
		}
	}
}


void MainWindow::on_actionExportShape_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
	if (currentIndex == -1 || ui->shapeComboBox->count() == 0) 
		return;

	SceneContext *context = m_context[currentIndex];
    Shape *shape = context->currentlySelectedShape;

    if (shape == NULL) {
        SLog(EError, "No shape selected, nothing to export");
        return;
    }

	QFileDialog *dialog = new QFileDialog(this, tr("Export shape .."),
		"", tr("Object file (*.obj)"));
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setAcceptMode(QFileDialog::AcceptSave);

#if defined(__OSX__)
	dialog->setOption(QFileDialog::DontUseNativeDialog, true);
#endif

	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowModality(Qt::WindowModal);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onExportShapeDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onExportShapeDialogClose(int reason) {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *ctx = m_context[currentIndex];

	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;

    if (reason != QDialog::Accepted)
        return;

    QString fileName = dialog->selectedFiles().value(0);
    fs::path path(fileName.toStdString());
    settings.setValue("fileDialogState", dialog->saveState());

    if (!fileName.endsWith(".obj")) {
        SLog(EError, "Unknown file type -- the filename must end in .obj");
        return;
    }

    /* get shape */
    Shape *shape = ctx->currentlySelectedShape;

    if (shape == NULL) {
        SLog(EError, "No shape selected, nothing to export");
        return;
    }

    ref<TriMesh> mesh = shape->createTriMesh();
    mesh->writeOBJ(path);
    SLog(EInfo, "Obj export of shape finished");
}

void MainWindow::on_actionSave_triggered() {
	SceneContext *context = m_context[ui->tabBar->currentIndex()];
	saveScene(this, context, context->fileName);
}

void MainWindow::on_actionSaveAs_triggered() {
	QFileDialog *dialog = new QFileDialog(this, tr("Save as .."),
		"", tr("Mitsuba scenes (*.xml)"));

	m_currentChild = dialog;
	QSettings settings("mitsuba-renderer.org", "qtgui");
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setViewMode(QFileDialog::Detail);
	dialog->setAcceptMode(QFileDialog::AcceptSave);
	dialog->restoreState(settings.value("fileDialogState").toByteArray());
	dialog->setWindowModality(Qt::WindowModal);
	connect(dialog, SIGNAL(finished(int)), this, SLOT(onSaveAsDialogClose(int)));
	m_currentChild = dialog;
	// prevent a tab drawing artifact on Qt/OSX
	m_activeWindowHack = true;
	dialog->show();
	qApp->processEvents();
	m_activeWindowHack = false;
}

void MainWindow::onSaveAsDialogClose(int reason) {
	int currentIndex = ui->tabBar->currentIndex();
	SceneContext *context = m_context[currentIndex];

	QSettings settings("mitsuba-renderer.org", "qtgui");
	QFileDialog *dialog = static_cast<QFileDialog *>(sender());
	m_currentChild = NULL;
	if (reason == QDialog::Accepted) {
        QString fileName = dialog->selectedFiles().value(0);
		settings.setValue("fileDialogState", dialog->saveState());
		saveScene(this, context, fileName);
		fs::path pathName = fileName.toStdString(),
			     baseName = fs::basename(pathName);
		context->fileName = fileName;
		context->shortName = QFileInfo(fileName).fileName();
		context->scene->setSourceFile(pathName);
		context->scene->setDestinationFile(baseName.file_string());
		ui->tabBar->setTabText(currentIndex, context->shortName);
		addRecentFile(fileName);
	}
}

void MainWindow::on_actionReferenceManual_triggered() {
	QDesktopServices::openUrl(QUrl("http://www.mitsuba-renderer.org/docs.html"));
}
	
void MainWindow::on_actionAbout_triggered() {
	AboutDialog about(this);
	about.exec();
}

void MainWindow::onJobFinished(const RenderJob *job, bool cancelled) {
	SceneContext *context = getContext(job, false);
	if (context == NULL)
		return;
	m_renderQueue->join();
	context->workUnits.clear();
	if (cancelled) {
		if (!context->cancelled) {
			QMessageBox::critical(this, tr("Error while rendering"),
				tr("The rendering job did not complete successfully. Please check the log."), 
				QMessageBox::Ok);
		} else {
			context->mode = context->cancelMode;
			if (ui->tabBar->currentIndex() != -1 &&
				m_context[ui->tabBar->currentIndex()] == context)
				ui->glView->resumePreview();
		}
	}
	onRefresh(job, NULL);
	context->renderJob = NULL;
	updateUI();
	if (ui->tabBar->currentIndex() != -1 &&
		m_context[ui->tabBar->currentIndex()] == context)
		resize(size() - context->sizeIncrease);
}
	
void MainWindow::onStatusMessage(const QString &status) {
	m_statusMessage = status;
	updateStatus();
}

void MainWindow::updateStatus() {
	if (m_statusMessage == "")
		setWindowTitle(tr("Copo renderer"));
	else
		setWindowTitle(tr("Copo renderer [%1]").arg(m_statusMessage));
}
    
void MainWindow::on_actionStartServer_triggered() {
	m_serverWidget = new ServerWidget(NULL, m_nodeName, m_listenPort);
	ui->actionStartServer->setEnabled(false);
	connect(m_serverWidget, SIGNAL(closed()), this, SLOT(onServerClosed()));
	m_serverWidget->show();
}

void MainWindow::on_actionAllShapesSnow_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
    if (currentIndex == -1)
        return;
	SceneContext *context = m_context[currentIndex];
    const shapeListType shapes = context->scene->getShapes();
    for (shapeListType::const_iterator it = shapes.begin(); it != shapes.end(); it++)
            updateSnowOnShape(context, *it, true);
    updateUI();
}

void MainWindow::on_actionAllShapesNoSnow_triggered() {
	int currentIndex = ui->tabBar->currentIndex();
    if (currentIndex == -1)
        return;
	SceneContext *context = m_context[currentIndex];
    const shapeListType shapes = context->scene->getShapes();
    for (shapeListType::const_iterator it = shapes.begin(); it != shapes.end(); it++)
            updateSnowOnShape(context, *it, false, false);

    /* expects allowed on control on context and preview data (i.e. should not be active) */
    /* Reset preview data */
    context->previewBuffer.vplSampleOffset = 0;
    context->pathLength = context->detectPathLength();
    int index = ui->tabBar->currentIndex();
    if (index != -1)
        on_tabBar_currentChanged(index);
    updateUI();
}

void MainWindow::on_actionReportBug_triggered() {
	QDesktopServices::openUrl(QUrl("https://www.mitsuba-renderer.org/bugtracker/projects/mitsuba"));
}

void MainWindow::on_actionFeedback_triggered() {
	QDesktopServices::openUrl(QUrl("mailto:Wenzel%20Jakob%20<wenzel@cs.cornell.edu>?subject=Feedback%20on%20Mitsuba"));
}

void MainWindow::onServerClosed() {
	delete m_serverWidget;
	m_serverWidget = NULL;
	ui->actionStartServer->setEnabled(true);
}

void MainWindow::onWorkBegin(const RenderJob *job, const RectangularWorkUnit *wu, int worker) {
	SceneContext *context = getContext(job, false);
	if (context == NULL)
		return;
	VisualWorkUnit vwu;
	/* This is not executed in the event loop -- take some precautions */
	m_contextMutex.lock();
	vwu.offset = wu->getOffset();
	vwu.size = wu->getSize();
	vwu.worker = worker;
	context->workUnits.insert(vwu);
	drawVisualWorkUnit(context, vwu);
	bool isCurrentView = ui->tabBar->currentIndex() < m_context.size() &&
		m_context[ui->tabBar->currentIndex()] == context;
	m_contextMutex.unlock();
	if (isCurrentView)
		emit updateView();
}

void MainWindow::drawVisualWorkUnit(SceneContext *context, const VisualWorkUnit &vwu) {
	Film *film = context->scene->getFilm();
	Point2i co = film->getCropOffset();
	int ox = vwu.offset.x - co.x, oy = vwu.offset.y - co.y,
		ex = ox + vwu.size.x, ey = oy + vwu.size.y;
	const float *color = NULL;

	/* Use desaturated colors to highlight the host
	   responsible for rendering the current image vwu */
	const float white[]     = { 1.0f, 1.0f, 1.0f };
	const float red[]       = { 1.0f, 0.3f, 0.3f };
	const float green[]     = { 0.3f, 1.0f, 0.3f };
	const float blue[]      = { 0.3f, 0.3f, 1.0f };
	const float gray[]      = { 0.5f, 0.5f, 0.5f };
	const float yellow[]    = { 1.0f, 1.0f, 0.0f };
	const float magenta[]   = { 1.0f, 0.3f, 1.0f };
	const float turquoise[] = { 0.3f, 1.0f, 1.0f };

	switch (vwu.worker % 8) {
		case 1: color = green; break;
		case 2: color = yellow; break;
		case 3: color = blue; break;
		case 4: color = gray; break;
		case 5: color = red; break;
		case 6: color = magenta; break;
		case 7: color = turquoise; break;
		case 0:
		default:
			color = white;
			break;
	}

	if (vwu.size.x < 3 || vwu.size.y < 3)
		return;

	drawHLine(context, ox, oy, ox + 3, color);
	drawHLine(context, ex - 4, oy, ex - 1, color);
	drawHLine(context, ox, ey - 1, ox + 3, color);
	drawHLine(context, ex - 4, ey - 1, ex - 1, color);
	drawVLine(context, ox, oy, oy + 3, color);
	drawVLine(context, ex - 1, oy, oy + 3, color);
	drawVLine(context, ex - 1, ey - 4, ey - 1, color);
	drawVLine(context, ox, ey - 4, ey - 1, color);
}

void MainWindow::onWorkEnd(const RenderJob *job, const ImageBlock *block) {
	int ox = block->getOffset().x, oy = block->getOffset().y,
		ex = ox + block->getSize().x, ey = oy + block->getSize().y;
	VisualWorkUnit vwu;
	vwu.offset = block->getOffset();
	vwu.size = block->getSize();
	SceneContext *context = getContext(job, false);
	if (context == NULL)
		return;
	Film *film = context->scene->getFilm();
	Point2i co = film->getCropOffset();
	Bitmap *bitmap = context->framebuffer;
	float *framebuffer = bitmap->getFloatData();
	Float r, g, b;

	for (int y = oy; y < ey; ++y) {
		int fbOffset = (ox - co.x + (y - co.y)*bitmap->getWidth())*4;
		for (int x = ox; x < ex; ++x) {
			film->getValue(x, y).toLinearRGB(r, g, b);
			framebuffer[fbOffset] = (float) r;
			framebuffer[fbOffset+1] = (float) g;
			framebuffer[fbOffset+2] = (float) b;
			framebuffer[fbOffset+3] = 1;
			fbOffset += 4;
		}
	}

	/* This is executed by worker threads -- take some precautions */
	m_contextMutex.lock();
	bool isCurrentView = ui->tabBar->currentIndex() < m_context.size() &&
		m_context[ui->tabBar->currentIndex()] == context;
	if (context->workUnits.find(vwu) != context->workUnits.end()) {
		context->workUnits.erase(vwu);
	} else if (!context->cancelled) {
		SLog(EWarn, "Internal error: unable to find previously scheduled"
				" rectangular work unit.");
	}
	m_contextMutex.unlock();
	if (isCurrentView)
		emit updateView();
}

void MainWindow::onRefresh(const RenderJob *job, const Bitmap *_bitmap) {
	SceneContext *context = getContext(job, false);
	if (context == NULL)
		return;
	Film *film = context->scene->getFilm();
	Point2i co = film->getCropOffset();
	Bitmap *bitmap = context->framebuffer;
	float *framebuffer = bitmap->getFloatData();

	if (_bitmap != NULL) {
		SAssert(bitmap->getWidth() == _bitmap->getWidth());
		SAssert(bitmap->getHeight() == _bitmap->getHeight());
		SAssert(bitmap->getBitsPerPixel() == _bitmap->getBitsPerPixel());
		memcpy(framebuffer, _bitmap->getFloatData(),
			bitmap->getWidth() * bitmap->getHeight() * 4 * sizeof(float));
	} else {
		Float r, g, b;
		for (int y = 0; y < bitmap->getHeight(); ++y) {
			int fbOffset = y*bitmap->getWidth()*4;
			for (int x = 0; x < bitmap->getWidth(); ++x) {
				film->getValue(x + co.x, y + co.y).toLinearRGB(r, g, b);
				framebuffer[fbOffset] = (float) r;
				framebuffer[fbOffset+1] = (float) g;
				framebuffer[fbOffset+2] = (float) b;
				fbOffset += 4;
			}
		}
	}

	/* This is executed by worker threads -- take some precautions */
	m_contextMutex.lock();
	bool isCurrentView = ui->tabBar->currentIndex() < m_context.size() &&
		m_context[ui->tabBar->currentIndex()] == context;
	for (std::set<VisualWorkUnit, block_comparator>::const_iterator it =
		context->workUnits.begin(); it != context->workUnits.end(); ++it) 
		drawVisualWorkUnit(context, *it);
	m_contextMutex.unlock();
	if (isCurrentView)
		emit updateView();
}

void MainWindow::on_glView_loadFileRequest(const QString &string) {
	loadFile(string);
}

bool ServerConnection::createWorker(QWidget *parent) {
	ref<Stream> stream;
	try {
		if (type == EDirectConnection) {
			stream = new SocketStream(hostName.toStdString(), port);
		} else {
			std::vector<std::string> cmdLine;
			cmdLine.push_back(formatString("bash -c 'cd %s; . setpath.sh; mtssrv -ls'", instDir.toLatin1().constData()));
			stream = new SSHStream(userName.toStdString(), 
				hostName.toStdString(), cmdLine, port);
		}
		worker = new RemoteWorker(formatString("net%i", remoteWorkerCtr++), stream);
		return true;
	} catch (const std::exception &e) {
		QString extra;
		if (type == ESSHConnection && !QString(e.what()).contains("configuration mismatch"))
			extra = parent->tr(" - Please make sure that you can log in manually using the "
				"command line and that <a href=\"http://www.debian-administration.org/articles/152\">"
				"passwordless authentication</a> is active.");
		QMessageBox::critical(parent, parent->tr("Unable to connect to %1").arg(hostName),
			QString("Unable to create a connection to \"%1\": %2%3")
			.arg(hostName).arg(e.what()).arg(extra), QMessageBox::Ok);
		return false;
	}
}

QSize MainWindow::sizeHint() const {
	QSize hint = QMainWindow::sizeHint();
	/* Don't include scroll bars in the size hint */
	if (ui->hScrollBar->isVisible())
		hint -= QSize(0, ui->hScrollBar->sizeHint().height());
	if (ui->vScrollBar->isVisible())
		hint -= QSize(ui->vScrollBar->sizeHint().width(), 0);
	return hint;
}

QString ServerConnection::toString() const {
	return QString("%1 (%2, %3 cores)")
		.arg(worker->getNodeName().c_str())
		.arg(type == EDirectConnection ? "direct" : "ssh")
		.arg(worker->getCoreCount());
}

SceneContext::SceneContext(SceneContext *ctx) {
	if (ctx->scene) {
		/* Temporarily set up a new file resolver */
		ref<Thread> thread = Thread::getThread();
		ref<FileResolver> oldResolver = thread->getFileResolver();
		ref<FileResolver> newResolver = oldResolver->clone();
		newResolver->addPath(fs::complete(ctx->scene->getSourceFile()).parent_path());
		thread->setFileResolver(newResolver);

		scene = new Scene(ctx->scene);
		ref<PluginManager> pluginMgr = PluginManager::getInstance();
		ref<PerspectiveCamera> oldCamera = static_cast<PerspectiveCamera *>(ctx->scene->getCamera());
		ref<PerspectiveCamera> camera = static_cast<PerspectiveCamera *> 
			(pluginMgr->createObject(MTS_CLASS(Camera), oldCamera->getProperties()));
		ref<Sampler> sampler = static_cast<Sampler *> 
			(pluginMgr->createObject(MTS_CLASS(Sampler), ctx->scene->getSampler()->getProperties()));
		ref<Film> film = static_cast<Film *> 
			(pluginMgr->createObject(MTS_CLASS(Film), oldCamera->getFilm()->getProperties()));
		const Integrator *oldIntegrator = ctx->scene->getIntegrator();
		ref<Integrator> currentIntegrator;

		int depth = 0;
		std::vector<Integrator *> integratorList;
		while (oldIntegrator != NULL) {
			ref<Integrator> integrator = static_cast<Integrator *> (pluginMgr->createObject(
				MTS_CLASS(Integrator), oldIntegrator->getProperties()));
			if (depth++ == 0) 
				scene->setIntegrator(integrator);
			else 
				currentIntegrator->addChild("", integrator);
			currentIntegrator = integrator;
			integratorList.push_back(integrator);
			oldIntegrator = oldIntegrator->getSubIntegrator();
		}

		for (int i=(int) integratorList.size()-1; i>=0; --i)
			integratorList[i]->configure();

		ref<ReconstructionFilter> rfilter = static_cast<ReconstructionFilter *> 
			(pluginMgr->createObject(MTS_CLASS(ReconstructionFilter), oldCamera->getFilm()->
				getReconstructionFilter()->getProperties()));

		rfilter->configure();
		film->addChild("", rfilter);
		film->configure();
		sampler->configure();
		camera->addChild("", sampler);
		camera->addChild("", film);
		camera->setViewTransform(oldCamera->getViewTransform());
		camera->setFov(oldCamera->getFov());
		camera->configure();
		scene->setCamera(camera);
		scene->setSampler(sampler);
		scene->configure();
		sceneResID = ctx->sceneResID;
		Scheduler::getInstance()->retainResource(sceneResID);
		thread->setFileResolver(oldResolver);
	} else {
		sceneResID = -1;
		renderJob = NULL;
	}
	fileName = ctx->fileName;
	shortName = ctx->shortName;
	movementScale = ctx->movementScale;
	up = ctx->up;
	renderJob = NULL;
	cancelled = false;
	progress = 0.0f;
	framebuffer = ctx->framebuffer->clone();
	mode = ctx->renderJob ? EPreview : ctx->mode;
	gamma = ctx->gamma;
	exposure = ctx->exposure;
	clamping = ctx->clamping;
	srgb = ctx->srgb;
	pathLength = ctx->pathLength;
	shadowMapResolution = ctx->shadowMapResolution;
	previewMethod = ctx->previewMethod;
	toneMappingMethod = ctx->toneMappingMethod;
	windowSize = ctx->windowSize;
	sizeIncrease = ctx->sizeIncrease;
	scrollOffset = ctx->scrollOffset;
	reinhardKey = ctx->reinhardKey;
	reinhardBurn = ctx->reinhardBurn;
	diffuseReceivers = ctx->diffuseReceivers;
	diffuseSources = ctx->diffuseSources;
	showKDTree = ctx->showKDTree;
	shownKDTreeLevel = ctx->shownKDTreeLevel;
	selectedShape = ctx->selectedShape;
	selectionMode = ctx->selectionMode;
	showNormals = ctx->showNormals;
    normalScaling = ctx->normalScaling;
    snow = ctx->snow;
    snowRenderSettings = ctx->snowRenderSettings;
    currentlySelectedShape = NULL;
}

SceneContext::~SceneContext() {
	if (scene && sceneResID != -1)
		Scheduler::getInstance()->unregisterResource(sceneResID);
	if (previewBuffer.buffer) {
		previewBuffer.buffer->disassociate();
		previewBuffer.buffer->decRef();
	}
	if (previewBuffer.sync) 
		previewBuffer.sync->decRef();
}

int SceneContext::detectPathLength() const {
	if (!scene)
		return 2;

	const Integrator *integrator = scene->getIntegrator();
	int extraDepth = 0;

	while (integrator->getSubIntegrator() != NULL) {
		if (integrator->getClass()->getName() == "IrradianceCacheIntegrator")
			extraDepth = 1;
		integrator = integrator->getSubIntegrator();
	}
	const Properties &integratorProps = integrator->getProperties();
	int maxDepth = -1;

	if (integratorProps.hasProperty("maxDepth"))
		maxDepth = integratorProps.getInteger("maxDepth");

	if (maxDepth == -1) {
		if (integratorProps.getPluginName() == "direct")
			maxDepth = 2;
		else
			maxDepth = 5;
	}

	return std::max(2, std::min(maxDepth + extraDepth, 6));
}

MTS_IMPLEMENT_CLASS(QRenderListener, false, RenderListener)
