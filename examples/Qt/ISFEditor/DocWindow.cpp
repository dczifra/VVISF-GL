#include "DocWindow.h"
#include "ui_DocWindow.h"

#include <QJsonDocument>
#include <QDebug>
#include <QTableView>
#include <QListWidget>
#include <QFileInfo>
#include <QDir>
#include <QFileDialog>

#include "SimpleSourceCodeEdit.h"
#include "LoadingWindow.h"
#include "ISFController.h"




#define VVDELETE(x) { if (x!=nullptr) { delete x; x=nullptr; } }
static DocWindow * globalDocWindow = nullptr;

using namespace std;
using namespace VVGL;
using namespace VVISF;




DocWindow::DocWindow(QWidget *parent) :
	QWidget(parent),
	ui(new Ui::DocWindow)
{
	globalDocWindow = this;
	
	ui->setupUi(this);
	
	//	tell the source code editors to load their language files
	QFile		tmpFile(":/shader language files/shader language files/ISF_GLSL_1_5.json");
	if (tmpFile.open(QFile::ReadOnly))	{
		QJsonDocument		tmpDoc = QJsonDocument::fromJson(tmpFile.readAll());
		if (tmpDoc.isEmpty())
			qDebug() << "\terr: doc was empty";
		tmpFile.close();
		
		ui->fragShaderEditor->loadSyntaxDefinitionDocument(tmpDoc);
		ui->vertShaderEditor->loadSyntaxDefinitionDocument(tmpDoc);
		ui->compiledVertShader->loadSyntaxDefinitionDocument(tmpDoc);
		ui->compiledFragShader->loadSyntaxDefinitionDocument(tmpDoc);
		
		ui->splitter->setCollapsible(2, false);
		QWidget		*jsonTableWidget = ui->splitter->widget(2);
		if (jsonTableWidget != nullptr)	{
			jsonTableWidget->setMinimumSize(QSize(250,250));
		}
		//int			tmpPos = ui->splitter->closestLegalPosition(999999,0);
		//ui->splitter->moveSplitter(0,tmpPos);
		QList<int>		tmpSizes;
		tmpSizes.append(99999);
		tmpSizes.append(0);
		tmpSizes.append(0);
		ui->splitter->setSizes(tmpSizes);
		
		ui->compiledVertShader->setReadOnly(true);
		ui->compiledFragShader->setReadOnly(true);
		ui->parsedJSON->setReadOnly(true);
		
		connect(ui->fragShaderEditor, &QPlainTextEdit::textChanged, [&]()	{
			lock_guard<recursive_mutex>		lock(propLock);
			//	update the ivar so we know there have been edits
			_fragEditsPerformed = true;
			//	kill the save timer if it exists
			if (_tmpFileSaveTimer != nullptr)	{
				_tmpFileSaveTimer->stop();
				delete _tmpFileSaveTimer;
				_tmpFileSaveTimer = nullptr;
			}
			//	if the file is saved in a tmp dir, start a timer to save it again in a couple seconds
			if (_fragFilePath!=nullptr && _fragFilePath->contains(QDir::tempPath()))	{
				_tmpFileSaveTimer = new QTimer(this);
				connect(_tmpFileSaveTimer, SIGNAL(timeout()), this, SLOT(tmpSaveTimerSlot()));
				_tmpFileSaveTimer->start(2000);
			}
		});
		connect(ui->vertShaderEditor, &QPlainTextEdit::textChanged, [&]()	{
			lock_guard<recursive_mutex>		lock(propLock);
			//	update the ivar so we know there have been edits
			_vertEditsPerformed = true;
			//	kill the save timer if it exists
			if (_tmpFileSaveTimer != nullptr)	{
				_tmpFileSaveTimer->stop();
				delete _tmpFileSaveTimer;
				_tmpFileSaveTimer = nullptr;
			}
			//	if the file is saved in a tmp dir, start a timer to save it again in a couple seconds
			if (_vertFilePath!=nullptr && _vertFilePath->contains(QDir::tempPath()))	{
				_tmpFileSaveTimer = new QTimer(this);
				connect(_tmpFileSaveTimer, SIGNAL(timeout()), this, SLOT(tmpSaveTimerSlot()));
				_tmpFileSaveTimer->start(2000);
			}
		});
	}
	else
		qDebug() << "ERR: couldn't open shader lang files, " << __PRETTY_FUNCTION__;
}
DocWindow::~DocWindow()	{
	delete ui;
	
	lock_guard<recursive_mutex>		lock(propLock);
	
	//	kill the save timer if it exists
	if (_tmpFileSaveTimer != nullptr)	{
		_tmpFileSaveTimer->stop();
		delete _tmpFileSaveTimer;
		_tmpFileSaveTimer = nullptr;
	}
	//	clear out the old paths and file contents
	VVDELETE(_fragFilePath);
	VVDELETE(_fragFilePathContentsOnOpen);
	VVDELETE(_vertFilePath);
	VVDELETE(_vertFilePathContentsOnOpen);
	//	delete any tmp files that may exist
	//VVDELETE(_tmpFragShaderFile);
	//VVDELETE(_tmpVertShaderFile);
	
}




void DocWindow::clearFileAndVars()	{
	lock_guard<recursive_mutex>		lock(propLock);
	
	//	kill the save timer if it exists
	if (_tmpFileSaveTimer != nullptr)	{
		_tmpFileSaveTimer->stop();
		delete _tmpFileSaveTimer;
		_tmpFileSaveTimer = nullptr;
	}
	//	clear out the old paths and file contents
	VVDELETE(_fragFilePath);
	VVDELETE(_fragFilePathContentsOnOpen);
	VVDELETE(_vertFilePath);
	VVDELETE(_vertFilePathContentsOnOpen);
}
void DocWindow::updateContentsFromISFController()	{
	qDebug() << __PRETTY_FUNCTION__;
	
	ISFSceneRef		scene = GetISFController()->getScene();
	ISFDocRef		doc = (scene==nullptr) ? nullptr : scene->getDoc();
	
	lock_guard<recursive_mutex>		lock(propLock);
	
	//	kill the save timer if it exists
	if (_tmpFileSaveTimer != nullptr)	{
		_tmpFileSaveTimer->stop();
		delete _tmpFileSaveTimer;
		_tmpFileSaveTimer = nullptr;
	}
	//	clear out the old paths and file contents
	VVDELETE(_fragFilePath);
	VVDELETE(_fragFilePathContentsOnOpen);
	VVDELETE(_vertFilePath);
	VVDELETE(_vertFilePathContentsOnOpen);
	
	if (doc != nullptr)	{
		//	get the frag file path from the doc
		_fragFilePath = new QString( QString::fromStdString(doc->getPath()) );
		//	check for a vert file by using the common recognized extensions for vert shaders
		QFileInfo		fragFileInfo(*_fragFilePath);
		QString			tmpPath = QString("%1/%2.vs").arg( fragFileInfo.dir().currentPath(), fragFileInfo.completeBaseName() );
		if (QFileInfo::exists(tmpPath))	{
			_vertFilePath = new QString(tmpPath);
		}
		else	{
			tmpPath = QString("%1/%2.vert").arg( fragFileInfo.dir().currentPath(), fragFileInfo.completeBaseName() );
			if (QFileInfo::exists(tmpPath))	{
				_vertFilePath = new QString(tmpPath);
			}
		}
		
		//	if there's a frag file, load its contents and populate the editor
		if (_fragFilePath != nullptr)	{
			QFile			tmpFragFile(*_fragFilePath);
			if (tmpFragFile.open(QFile::ReadOnly))	{
				QTextStream		rStream(&tmpFragFile);
				_fragFilePathContentsOnOpen = new QString( rStream.readAll() );
				tmpFragFile.close();
			}
		}
		if (_fragFilePathContentsOnOpen == nullptr)
			ui->fragShaderEditor->clear();
		else
			ui->fragShaderEditor->setPlainText(*_fragFilePathContentsOnOpen);
		
		//	if there's a vert file, load its contents and populate the editor
		if (_vertFilePath != nullptr)	{
			QFile			tmpVertFile(*_vertFilePath);
			if (tmpVertFile.open(QFile::ReadOnly))	{
				QTextStream		rStream(&tmpVertFile);
				_vertFilePathContentsOnOpen = new QString( rStream.readAll() );
				tmpVertFile.close();
			}
		}
		if (_vertFilePathContentsOnOpen == nullptr)
			ui->vertShaderEditor->clear();
		else
			ui->vertShaderEditor->setPlainText(*_vertFilePathContentsOnOpen);
		
		/*
		_fragFilePathContentsOnOpen = new QString( QString::fromStdString(*doc->getFragShaderSource()) );
		ui->fragShaderEditor->setPlainText(*_fragFilePathContentsOnOpen);
		_vertFilePathContentsOnOpen = new QString( QString::fromStdString(*doc->getVertShaderSource()) );
		ui->vertShaderEditor->setPlainText(*_vertFilePathContentsOnOpen);
		*/
		
		ui->compiledFragShader->setPlainText( QString::fromStdString(scene->getFragmentShaderString()) );
		ui->compiledVertShader->setPlainText( QString::fromStdString(scene->getVertexShaderString()) );
		ui->parsedJSON->setPlainText( QString::fromStdString(*doc->getJSONString()) );
	}
	else	{
		ui->fragShaderEditor->setPlainText(QString(""));
		ui->vertShaderEditor->setPlainText(QString(""));
		
		ui->compiledVertShader->setReadOnly(true);
		ui->compiledFragShader->setReadOnly(true);
		ui->parsedJSON->setReadOnly(true);
	}
}
void DocWindow::saveOpenFile()	{
	qDebug() << __PRETTY_FUNCTION__;
	
	QString			currentFragString = ui->fragShaderEditor->toPlainText();
	QString			currentVertString = ui->vertShaderEditor->toPlainText();
	
	{
		lock_guard<recursive_mutex>		lock(propLock);
		
		//	kill the save timer if it exists
		if (_tmpFileSaveTimer != nullptr)	{
			_tmpFileSaveTimer->stop();
			delete _tmpFileSaveTimer;
			_tmpFileSaveTimer = nullptr;
		}
		
		bool			fragContentsChanged = false;
		bool			vertContentsChanged = false;
		
		if ((_fragFilePathContentsOnOpen==nullptr && currentFragString.length()>0)	||
		(_fragFilePathContentsOnOpen!=nullptr && *_fragFilePathContentsOnOpen!=currentFragString))	{
			fragContentsChanged = true;
		}
		if ((_vertFilePathContentsOnOpen==nullptr && currentVertString.length()>0)	||
		(_vertFilePathContentsOnOpen!=nullptr && *_vertFilePathContentsOnOpen!=currentVertString))	{
			vertContentsChanged = true;
		}
		
		//	if the frag file is in the tmp dir, we're going to pretend that its contents have changed so it gets written to disk
		if (!fragContentsChanged && _fragFilePath!=nullptr && _fragFilePath->contains( QDir::tempPath() ))
			fragContentsChanged = true;
		if (!vertContentsChanged && _vertFilePath!=nullptr && _vertFilePath->contains( QDir::tempPath() ))
			vertContentsChanged = true;
		
		if (!fragContentsChanged && !vertContentsChanged)	{
			qDebug() << "\tbailing- neither frag nor vert shaders changed...";
			return;
		}
		
		//	if the file path is nil or this is a tmp file, open an alert so the user can supply a name and save location for the file
		if (_fragFilePath==nullptr || _fragFilePath->contains(QDir::tempPath()))	{
			qDebug() << "\tpresently-viewed file is a tmp file...";
			QString			pathToSave = QFileDialog::getSaveFileName(GetLoadingWindow(),
				tr("Save shader as:"),
				GetLoadingWindow()->getBaseDirectory(),
				tr("Text (*.fs)"));
			//qDebug() << "\tdestPath is " << pathToSave;
			QFileInfo		saveFileInfo(pathToSave);
			QString			noExtPathToSave = QString("%1/%2").arg( saveFileInfo.dir().absolutePath(), saveFileInfo.completeBaseName() );
			//qDebug() << "\tnoExtPathToSave is " << noExtPathToSave;
			
			
			if (fragContentsChanged)	{
				QString			localWritePath = QString("%1.fs").arg(noExtPathToSave);
				qDebug() << "\tsaving frag to file " << localWritePath;
				VVDELETE(_fragFilePath);
				_fragFilePath = new QString(localWritePath);
				VVDELETE(_fragFilePathContentsOnOpen)
				_fragFilePathContentsOnOpen = new QString(currentFragString);
				
				QFile			wFile(localWritePath);
				if (wFile.open(QIODevice::WriteOnly))	{
					QTextStream		wStream(&wFile);
					wStream << currentFragString;
					wFile.close();
				}
			}
			if (vertContentsChanged)	{
				QString			localWritePath = QString("%1.vs").arg(noExtPathToSave);
				qDebug() << "\tsaving vert to file " << localWritePath;
				VVDELETE(_vertFilePath);
				_vertFilePath = new QString(localWritePath);
				VVDELETE(_vertFilePathContentsOnOpen)
				_vertFilePathContentsOnOpen = new QString(currentVertString);
				
				QFile			wFile(localWritePath);
				if (wFile.open(QIODevice::WriteOnly))	{
					QTextStream		wStream(&wFile);
					wStream << currentVertString;
					wFile.close();
				}
			}
			
			if (fragContentsChanged || vertContentsChanged)
				GetLoadingWindow()->on_loadFile(pathToSave);
			
		}
		//	else the file path is non-nil and not in tmp, so just save it to disk
		else	{
			qDebug() << "\tpresently-viewed file is NOT a tmp file...";
			
			if (fragContentsChanged)	{
				if (_fragFilePath != nullptr)	{
					qDebug() << "\tsaving frag file to " << *_fragFilePath;
					VVDELETE(_fragFilePathContentsOnOpen);
					_fragFilePathContentsOnOpen = new QString(currentFragString);
					
					QFile		wFile(*_fragFilePath);
					if (wFile.open(QIODevice::WriteOnly))	{
						QTextStream		wStream(&wFile);
						wStream << currentFragString;
						wFile.close();
						_fragEditsPerformed = false;
					}
				}
			}
			if (vertContentsChanged)	{
				if (_vertFilePath != nullptr)	{
					qDebug() << "\tsaving vert file to " << *_vertFilePath;
					VVDELETE(_vertFilePathContentsOnOpen);
					_vertFilePathContentsOnOpen = new QString(currentVertString);
					
					QFile		wFile(*_vertFilePath);
					if (wFile.open(QIODevice::WriteOnly))	{
						QTextStream		wStream(&wFile);
						wStream << currentVertString;
						wFile.close();
						_vertEditsPerformed = false;
					}
				}
			}
		}
	}
}
void DocWindow::reloadFileFromTableView()	{
}
bool DocWindow::contentsNeedToBeSaved()	{
	lock_guard<recursive_mutex>		lock(propLock);
	bool			returnMe = false;
	if (_fragEditsPerformed)	{
		QString			currentContents = ui->fragShaderEditor->toPlainText();
		if (currentContents.length() > 0)	{
			if (_fragFilePathContentsOnOpen==nullptr || *_fragFilePathContentsOnOpen!=currentContents)	{
				returnMe = true;
			}
		}
	}
	if (_vertEditsPerformed)	{
		QString			currentContents = ui->vertShaderEditor->toPlainText();
		if (currentContents.length() > 0)	{
			if (_vertFilePathContentsOnOpen==nullptr || *_vertFilePathContentsOnOpen!=currentContents)	{
				returnMe = true;
			}
		}
	}
	return returnMe;
}
QString DocWindow::fragFilePath()	{
	lock_guard<recursive_mutex>		lock(propLock);
	QString		returnMe = (_fragFilePath==nullptr) ? QString("") : QString(*_fragFilePath);
	return returnMe;
}








void DocWindow::tmpSaveTimerSlot()	{
	qDebug() << __PRETTY_FUNCTION__;
	
	QString			currentFragString = ui->fragShaderEditor->toPlainText();
	QString			currentVertString = ui->vertShaderEditor->toPlainText();
	
	{
		lock_guard<recursive_mutex>		lock(propLock);
		
		//	kill the save timer if it exists
		if (_tmpFileSaveTimer != nullptr)	{
			_tmpFileSaveTimer->stop();
			delete _tmpFileSaveTimer;
			_tmpFileSaveTimer = nullptr;
		}
		
		if (_fragFilePath!=nullptr && _fragFilePath->contains( QDir::tempPath() ))	{
			VVDELETE(_fragFilePathContentsOnOpen)
			_fragFilePathContentsOnOpen = new QString(currentFragString);
			
			QFile			wFile(*_fragFilePath);
			if (wFile.open(QIODevice::WriteOnly))	{
				QTextStream		wStream(&wFile);
				wStream << currentFragString;
				wFile.close();
			}
		}
		
		if (_vertFilePath!=nullptr && _vertFilePath->contains( QDir::tempPath() ))	{
			VVDELETE(_vertFilePathContentsOnOpen)
			_vertFilePathContentsOnOpen = new QString(currentVertString);
			
			QFile			wFile(*_vertFilePath);
			if (wFile.open(QIODevice::WriteOnly))	{
				QTextStream		wStream(&wFile);
				wStream << currentVertString;
				wFile.close();
			}
		}
	}
}








DocWindow * GetDocWindow()	{
	return globalDocWindow;
}