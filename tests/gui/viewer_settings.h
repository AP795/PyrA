#pragma once

#include <QString>
#include <QStringList>

namespace pyra::test {

class ViewerSettings {
public:
	explicit ViewerSettings ( const QString &directory = default_directory() );
	static QString default_directory ();
	QStringList load ();
	QString config_path () const;
	QString status_path () const;
	const QString &image_directory () const { return image_directory_; }
	const QString &last_image () const { return last_image_; }
	QString startup_image ( const QString &explicit_image = {} ) const;
	void image_loaded ( const QString &path );
	bool save_status ( QString *error = nullptr ) const;

private:
	QString directory_;
	QString image_directory_;
	QString last_image_;
};

} // namespace pyra::test
