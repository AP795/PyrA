#pragma once

#include "render_evaluator.h"

#include <QMainWindow>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>
#include <optional>

class QAction;
class QComboBox;
class QDockWidget;
class QPlainTextEdit;
class QProgressBar;
class QSplitter;

namespace pyra::test {

class Viewport;
class Minimap;

class ViewerWindow : public QMainWindow {
public:
	explicit ViewerWindow ( const QString &image_directory = {} );
	~ViewerWindow () override;
	void open ( const QString &path, pyra_backend backend = PYRA_BACKEND_NATIVE );
	void open ( const QString &path, const pyra_context_options &options );
	void smoke_test ( const QString &output_directory );
	void report_message ( const QString &message );
	const QString &last_loaded_image () const { return last_loaded_image_; }

private:
	void select_plane ( int index );
	void open_dialog ( pyra_backend backend );
	void schedule_render ();
	void dispatch_render ();
	void update_controls ();
	void record ();
	void stop ();
	void replay ();
	void show_replay_report ( const QString &text );
	void smoke_frame ();
	bool smoke_minimap ( bool drag );
	void smoke_finish ( int code );

	Viewport *viewport_ = nullptr;
	Minimap *minimap_ = nullptr;
	QComboBox *planes_ = nullptr;
	QPlainTextEdit *metadata_ = nullptr;
	QPlainTextEdit *report_ = nullptr;
	QDockWidget *inspector_ = nullptr;
	QSplitter *inspector_splitter_ = nullptr;
	QProgressBar *busy_ = nullptr;
	QAction *open_ = nullptr;
	QAction *fit_ = nullptr;
	QAction *native_ = nullptr;
	QAction *record_ = nullptr;
	QAction *stop_ = nullptr;
	QAction *replay_ = nullptr;
	QThread thread_;
	QObject *worker_ = nullptr;
	std::shared_ptr<RenderEvaluator> evaluator_;
	QTimer debounce_;
	std::vector<PlaneInfo> plane_info_;
	std::optional<RenderRequest> pending_;
	std::optional<RenderRequest> active_request_;
	std::optional<RenderRequest> displayed_request_;
	std::optional<RenderRequest> latest_view_;
	std::optional<double> overscan_failed_mpp_;
	std::size_t recorded_count_ = 0;
	unsigned generation_ = 0;
	std::atomic<unsigned> overview_generation_{ 0 };
	bool opening_ = false;
	bool overview_loading_ = false;
	bool rendering_ = false;
	bool recording_ = false;
	bool stopping_ = false;
	bool replaying_ = false;
	QString smoke_output_;
	QString image_directory_;
	QString last_loaded_image_;
	pyra_context_options reader_options_ = reader_options();
	int smoke_step_ = -1;
};

} // namespace pyra::test
