/*
 * LensLink tray — a StatusNotifierItem for the lenslinkd daemon.
 * Talks to the daemon over its unix socket; never links the pipeline.
 */

#include "ipc_c.h"

#include <QAction>
#include <QApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>

#include <cstdio>

namespace {

QJsonObject obj(std::initializer_list<std::pair<QString, QJsonValue>> l)
{
	return QJsonObject(l);
}

class Tray : public QSystemTrayIcon {
    public:
	Tray()
	{
		setIcon(QIcon::fromTheme("camera-web"));
		setToolTip("LensLink");

		status_action_ = new QAction("Starting…", this);
		status_action_->setEnabled(false);

		stream_toggle_ = new QAction("Enabled", this);
		stream_toggle_->setCheckable(true);
		connect(stream_toggle_, &QAction::toggled, this,
			&Tray::on_stream_toggled);

		camera_start_ = new QAction("Start camera on phone", this);
		connect(camera_start_, &QAction::triggered, this, [this] {
			send(obj({{"cmd", "camera"}, {"on", true}}));
		});

		camera_stop_ = new QAction("Stop camera on phone", this);
		connect(camera_stop_, &QAction::triggered, this, [this] {
			send(obj({{"cmd", "camera"}, {"on", false}}));
		});

		phones_menu_ = new QMenu("Phone");
		connect(phones_menu_, &QMenu::aboutToShow, this,
			&Tray::refresh_phones);

		mic_status_ = new QAction("Phone microphone: off", this);
		mic_status_->setEnabled(false);

		QMenu *menu = new QMenu();
		menu->addAction(status_action_);
		menu->addSeparator();
		menu->addAction(stream_toggle_);
		menu->addAction(camera_start_);
		menu->addAction(camera_stop_);
		menu->addMenu(phones_menu_);
		menu->addAction(mic_status_);
		menu->addSeparator();
		QAction *quit = menu->addAction("Quit");
		connect(quit, &QAction::triggered, this,
			[] { QApplication::quit(); });

		setContextMenu(menu);
		show();

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, &Tray::poll);
		timer_->start(1000);
		poll();
	}

    private:
	QJsonObject request(const QJsonObject &o) const
	{
		char resp[8192] = {0};
		QByteArray req =
			QJsonDocument(o).toJson(QJsonDocument::Compact);
		if (!ipc_request(req.constData(), resp, sizeof(resp)))
			return {};
		return QJsonDocument::fromJson(QByteArray(resp)).object();
	}

	void send(const QJsonObject &o) { request(o); }

	void poll()
	{
		QJsonObject st = request(obj({{"cmd", "status"}}));
		if (st.isEmpty()) {
			status_action_->setText("Daemon not running");
			stream_toggle_->setEnabled(false);
			camera_start_->setEnabled(false);
			camera_stop_->setEnabled(false);
			setToolTip("LensLink — daemon not running");
			return;
		}
		stream_toggle_->setEnabled(true);

		bool enabled = st["enabled"].toBool();
		bool connected = st["connected"].toBool();
		bool standby = st["standby"].toBool();
		bool streaming = st["streaming"].toBool();
		QString name = st["name"].toString();

		stream_toggle_->blockSignals(true);
		stream_toggle_->setChecked(enabled);
		stream_toggle_->blockSignals(false);

		bool mic_on = connected && st["mic_on"].toBool();
		mic_status_->setText(mic_on
					     ? "Phone microphone: on"
					     : "Phone microphone: off — set "
					       "in the app's Options");

		camera_start_->setEnabled(connected && standby);
		camera_stop_->setEnabled(connected && !standby);

		QString line;
		if (!enabled) {
			line = "Disabled";
		} else if (!connected) {
			line = "Looking for a phone…";
		} else if (standby) {
			line = name + " — idle";
		} else if (streaming) {
			line = QString("%1 · %2x%3 · %4 fps · %5 kb/s · %6 ms")
				       .arg(name)
				       .arg(st["width"].toInt())
				       .arg(st["height"].toInt())
				       .arg(st["fps"].toInt())
				       .arg(st["kbps"].toInt())
				       .arg(st["latency"].toInt());
		} else {
			line = name + " — starting…";
		}
		status_action_->setText(line);
		setToolTip("LensLink — " + line);
	}

	void on_stream_toggled(bool on)
	{
		send(obj({{"cmd", "enabled"}, {"on", on}}));
		poll();
	}

	void refresh_phones()
	{
		phones_menu_->clear();
		QJsonObject st = request(obj({{"cmd", "status"}}));
		QString cur_host = st["host"].toString();
		QString cur_udid = st["udid"].toString();
		bool cur_usb = st["usb"].toBool();

		QJsonObject phones = request(obj({{"cmd", "phones"}}));
		if (phones.isEmpty()) {
			QAction *a =
				phones_menu_->addAction("daemon not running");
			a->setEnabled(false);
			return;
		}

		bool any = false;
		const auto wifi = phones["wifi"].toArray();
		for (const auto &v : wifi) {
			QJsonObject p = v.toObject();
			QString host = p["host"].toString();
			QAction *a = phones_menu_->addAction(
				p["name"].toString() + " (Wi-Fi)");
			a->setCheckable(true);
			a->setChecked(!cur_usb && host == cur_host);
			connect(a, &QAction::triggered, this, [this, host] {
				send(obj({{"cmd", "use"},
					  {"usb", false},
					  {"host", host}}));
				poll();
			});
			any = true;
		}
		const auto usb = phones["usb"].toArray();
		for (const auto &v : usb) {
			QJsonObject p = v.toObject();
			QString udid = p["udid"].toString();
			QAction *a =
				phones_menu_->addAction("USB · " + udid);
			a->setCheckable(true);
			a->setChecked(cur_usb && udid == cur_udid);
			connect(a, &QAction::triggered, this, [this, udid] {
				send(obj({{"cmd", "use"},
					  {"usb", true},
					  {"udid", udid},
					  {"host", QString()}}));
				poll();
			});
			any = true;
		}
		if (!any)
			phones_menu_->addAction("none found");

		phones_menu_->addSeparator();
		QAction *manual = phones_menu_->addAction("Manual IP…");
		connect(manual, &QAction::triggered, this, [this, cur_host] {
			bool ok = false;
			QString ip = QInputDialog::getText(
				nullptr, "LensLink", "Phone IP address:",
				QLineEdit::Normal, cur_host, &ok);
			if (ok && !ip.isEmpty()) {
				send(obj({{"cmd", "use"},
					  {"usb", false},
					  {"host", ip}}));
				poll();
			}
		});
	}

	QAction *status_action_;
	QAction *stream_toggle_;
	QAction *camera_start_;
	QAction *camera_stop_;
	QAction *mic_status_;
	QMenu *phones_menu_;
	QTimer *timer_;
};

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setApplicationName("LensLink");
	app.setQuitOnLastWindowClosed(false);
	Tray tray;
	return app.exec();
}
