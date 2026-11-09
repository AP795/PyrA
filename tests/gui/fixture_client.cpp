#include "test_fixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>

#include <stdexcept>

namespace pyra::test {

QString write_test_wsi ( const QString &directory )
{
	const auto tool = QDir( QCoreApplication::applicationDirPath() ).filePath(
#ifdef _WIN32
		QStringLiteral( "pyra_fixture_writer.exe" ) );
#else
		QStringLiteral( "pyra_fixture_writer" ) );
#endif
	auto process = QProcess{};
	process.start( tool, { directory } );
	if (!process.waitForFinished() || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
		throw std::runtime_error( "Fixture writer failed: " + process.readAllStandardError().toStdString() );
	return QDir( directory ).filePath( QStringLiteral( "gui_fixture.ptf" ) );
}

} // namespace pyra::test
