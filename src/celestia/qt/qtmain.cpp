/***************************************************************************
                          qtmain.cpp  -  description
                             -------------------
    begin                : Tue Jul 16 22:28:19 CEST 2002
    copyright            : (C) 2002 by Christophe Teyssier
    email                : chris@teyssier.org
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include <QTime>
#include <QApplication>
#include <QSplashScreen>
#include <QDesktopServices>
#include <QPixmap>
#include <QBitmap>
#include "qtgettext.h"
#include <QLocale>
#include <QLibraryInfo>
#include <vector>
#include "qtappwin.h"
#include <qtextcodec.h>
#include <fmt/printf.h>

#ifndef SPLASH_DIR
#define SPLASH_DIR
#endif

using namespace std;

//extern "C" { FILE __iob_func[3] = { *stdin,*stdout,*stderr }; }
//static const char *description = "Celestia";

// Command line options
static bool startFullscreen = false;
static bool runOnce = false;
static QString startURL;
static QString startDirectory;
static QString startScript;
static QStringList extrasDirectories;
static QString configFileName;
static bool useAlternateConfigFile = false;
static bool skipSplashScreen = false;

static bool ParseCommandLine();

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QTranslator qtTranslator;
    qtTranslator.load("qt_" + QLocale::system().name(),
                      QLibraryInfo::location(QLibraryInfo::TranslationsPath));
    app.installTranslator(&qtTranslator);

    CelestiaQTranslator celestiaTranslator;
    app.installTranslator(&celestiaTranslator);

    Q_INIT_RESOURCE(icons);

    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QCoreApplication::setOrganizationName("Celestia Development Team");
    QCoreApplication::setApplicationName("Celestia QT");

    ParseCommandLine();

    QPixmap pixmap(SPLASH_DIR "splash.png");
    QSplashScreen splash(pixmap);
    splash.setMask(pixmap.mask());

    // TODO: resolve issues with pixmap alpha channel
    splash.show();

    // Gettext integration
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    bindtextdomain("celestia", LOCALEDIR);
    bind_textdomain_codeset("celestia", "UTF-8");
    bindtextdomain("celestia_constellations", LOCALEDIR);
    bind_textdomain_codeset("celestia_constellations", "UTF-8");
    textdomain("celestia");

    CelestiaAppWindow window;

    // Connect the splash screen to the main window so that it
    // can receive progress notifications as Celestia files required
    // for startup are loaded.
    QObject::connect(&window, SIGNAL(progressUpdate(const QString&, int, const QColor&)),
                     &splash, SLOT(showMessage(const QString&, int, const QColor&)));

    window.init(configFileName, extrasDirectories);
    window.show();

    splash.finish(&window);

    // Set the main window to be the cel url handler
    QDesktopServices::setUrlHandler("cel", &window, "handleCelUrl");

    return app.exec();
}



static void CommandLineError(const char* /*unused*/)
{
}



bool ParseCommandLine()
{
    QStringList args = QCoreApplication::arguments();

    int i = 1;

    while (i < args.size())
    {
        bool isLastArg = (i == args.size() - 1);
#if 0
        if (strcmp(argv[i], "--verbose") == 0)
        {
            SetDebugVerbosity(1);
        }
        else
#endif
        if (args.at(i) == "--fullscreen")
        {
            startFullscreen = true;
        }
        else if (args.at(i) == "--once")
        {
            runOnce = true;
        }
        else if (args.at(i) == "--dir")
        {
            if (isLastArg)
            {
                CommandLineError("Directory expected after --dir");
                return false;
            }
            i++;
            startDirectory = args.at(i);
        }
        else if (args.at(i) == "--conf")
        {
            if (isLastArg)
            {
                CommandLineError("Configuration file name expected after --conf");
                return false;
            }
            i++;
            configFileName = args.at(i);
            useAlternateConfigFile = true;
        }
        else if (args.at(i) == "--extrasdir")
        {
            if (isLastArg)
            {
                CommandLineError("Directory expected after --extrasdir");
                return false;
            }
            i++;
            extrasDirectories.push_back(args.at(i));
        }
        else if (args.at(i) == "-u" || args.at(i) == "--url")
        {
            if (isLastArg)
            {
                CommandLineError("URL expected after --url");
                return false;
            }
            i++;
            startURL = args.at(i);
        }
        else if (args.at(i) == "-s" || args.at(i) == "--nosplash")
        {
            skipSplashScreen = true;
        }
        else
        {
            string buf = fmt::sprintf("Invalid command line option '%s'", args.at(i).toUtf8().data());
            CommandLineError(buf.c_str());
            return false;
        }

        i++;
    }

    return true;
}
