#pragma once

#include "reader_options.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace pyra::test {

class OpenWsiDialog : public QDialog {
public:
	OpenWsiDialog ( const QString &directory, const QString &path,
		const pyra_context_options &options, QWidget *parent = nullptr );
	QString path () const;
	pyra_context_options options () const;

private:
	void update_backend ();
	pyra_context_options options_;
	QLineEdit *path_ = nullptr;
	QComboBox *backend_ = nullptr;
	QSpinBox *tbf_ = nullptr;
	QSpinBox *pbf_ = nullptr;
	QSpinBox *threads_ = nullptr;
	QLabel *policy_ = nullptr;
};

} // namespace pyra::test
