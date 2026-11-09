#include "test_fixture.h"

#include <QCoreApplication>

#include <exception>
#include <iostream>

int main ( int argc, char **argv )
{
	auto application = QCoreApplication( argc, argv );
	if (application.arguments().size() != 2)
		return 2;
	try {
		(void)pyra::test::write_test_slide( application.arguments()[1] );
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
