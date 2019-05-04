#include "LatexController.h"

#include "Control.h"

#include "gui/XournalView.h"
#include "gui/dialog/LatexDialog.h"
#include "undo/InsertUndoAction.h"

#include <i18n.h>
#include <Util.h>
#include <Stacktrace.h>
#include <XojMsgBox.h>
#include <StringUtils.h>

#include "pixbuf-utils.h"

/**
 * First half of the LaTeX template used to generate preview PDFs. User-supplied
 * formulas will be inserted between the two halves.
 *
 * This template is necessarily complicated because we need to cause an error if
 * the rendered formula is blank. Otherwise, a completely blank, sizeless PDF
 * will be generated, which Poppler will be unable to load.
 */
const char* LATEX_TEMPLATE_1 =
	R"(\documentclass[crop, border=5pt]{standalone})" "\n"
	R"(\usepackage{amsmath})" "\n"
	R"(\usepackage{ifthen})" "\n"
	R"(\begin{document})" "\n"
	R"(\def\preview{\(\displaystyle)" "\n";

const char* LATEX_TEMPLATE_2 =
	"\n\\)}\n"
	R"(\newlength{\pheight})" "\n"
	R"(\settoheight{\pheight}{\hbox{\preview}})" "\n"
	R"(\ifthenelse{\pheight=0.0pt})" "\n"
	R"({\GenericError{}{xournalpp: blank formula}{}{}})" "\n"
	R"({\preview})" "\n"
	R"(\end{document})" "\n";

LatexController::LatexController(Control* control)
	: control(control),
	  doc(control->getDocument()),
	  texTmp(Util::getConfigSubfolder("tex").str())
{
	XOJ_INIT_TYPE(LatexController);
}

LatexController::~LatexController()
{
	XOJ_CHECK_TYPE(LatexController);

	this->control = NULL;

	XOJ_RELEASE_TYPE(LatexController);
}

/**
 * Find the tex executable, return false if not found
 */
bool LatexController::findTexExecutable()
{
	XOJ_CHECK_TYPE(LatexController);

	gchar* pdflatex = g_find_program_in_path("pdflatex");
	if (!pdflatex)
	{
		return false;
	}

	binTex = pdflatex;
	g_free(pdflatex);

	return true;
}

GPid* LatexController::runCommandAsync()
{
	XOJ_CHECK_TYPE(LatexController);
	g_assert(!this->isUpdating);

	string texContents = LATEX_TEMPLATE_1;
	texContents += this->currentTex;
	texContents += LATEX_TEMPLATE_2;

	string texFile = texTmp + "/tex.tex";

	GError* err = NULL;
	if (!g_file_set_contents(texFile.c_str(), texContents.c_str(), texContents.length(), &err))
	{
		XojMsgBox::showErrorToUser(control->getGtkWindow(), FS(_F("Could not save .tex file: {1}") % err->message));
		g_error_free(err);
		return nullptr;
	}

	char* texFileEscaped = g_strescape(texFile.c_str(), NULL);
	char* cmd = g_strdup(binTex.c_str());

	static char* texFlag = g_strdup("-interaction=nonstopmode");
	char* argv[] = { cmd, texFlag, texFileEscaped, NULL };

	GPid* pdflatex_pid = reinterpret_cast<GPid*>(g_malloc(sizeof(GPid)));
	GSpawnFlags flags = GSpawnFlags(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD);
	this->setUpdating(true);
	this->lastPreviewedTex = this->currentTex;
	bool success = g_spawn_async(texTmp.c_str(), argv, nullptr, flags, nullptr, nullptr, pdflatex_pid, &err);
	if (!success)
	{
		string message = FS(_F("Could not start pdflatex: {1} (exit code: {2})") % err->message % err->code);
		g_warning(message.c_str());
		XojMsgBox::showErrorToUser(control->getGtkWindow(), message);

		g_error_free(err);
		g_free(pdflatex_pid);
		pdflatex_pid = nullptr;
		this->setUpdating(false);
	}

	g_free(texFileEscaped);
	g_free(cmd);

	return pdflatex_pid;
}

/**
 * Find a selected tex element, and load it
 */
void LatexController::findSelectedTexElement()
{
	XOJ_CHECK_TYPE(LatexController);

	doc->lock();
	int pageNr = control->getCurrentPageNo();
	if (pageNr == -1)
	{
		doc->unlock();
		return;
	}
	view = control->getWindow()->getXournal()->getViewFor(pageNr);
	if (view == NULL)
	{
		doc->unlock();
		return;
	}

	// we get the selection
	page = doc->getPage(pageNr);
	layer = page->getSelectedLayer();

	selectedTexImage = view->getSelectedTex();
	selectedText = view->getSelectedText();

	if (selectedTexImage || selectedText)
	{
		// this will get the position of the Latex properly
		EditSelection *theSelection = control->getWindow()->getXournal()->getSelection();
		posx = theSelection->getXOnView();
		posy = theSelection->getYOnView();

		if (selectedTexImage)
		{
			initalTex = selectedTexImage->getText();
			imgwidth = selectedTexImage->getElementWidth();
			imgheight = selectedTexImage->getElementHeight();
		}
		else
		{
			initalTex += "\\text{";
			initalTex += selectedText->getText();
			initalTex += "}";
			imgwidth = selectedText->getElementWidth();
			imgheight = selectedText->getElementHeight();
		}
	}
	if (initalTex.empty())
	{
		initalTex = "x^2";
	}
	currentTex = initalTex;
	doc->unlock();

	// need to do this otherwise we can't remove the image for its replacement
	control->clearSelectionEndText();
}

void LatexController::showTexEditDialog()
{
	XOJ_CHECK_TYPE(LatexController);

	dlg = new LatexDialog(control->getGladeSearchPath());

	// For 'real time' LaTex rendering in the dialog
	dlg->setTex(initalTex);
	g_signal_connect(dlg->getTextBuffer(), "changed", G_CALLBACK(handleTexChanged), this);


	// The controller owns the tempRender because, on signal changed, he has to handle the old/new renders
	if (temporaryRender != NULL)
	{
		dlg->setTempRender(temporaryRender->getPdf(), initalTex.size());
	}

	dlg->show(GTK_WINDOW(control->getWindow()->getWindow()));

	deletePreviousRender();
	currentTex = dlg->getTex();
	currentTex += " ";

	delete dlg;
}

void LatexController::triggerImageUpdate(bool isPreview)
{
	if (this->isUpdating)
	{
		return;
	}

	GPid* pid = this->runCommandAsync();
	if (pid != nullptr)
	{
		g_assert(this->isUpdating);
		auto data = new PdfRenderCallbackData(this, isPreview);
		XOJ_CHECK_TYPE_OBJ(data->first, LatexController);
		g_child_watch_add(*pid, reinterpret_cast<GChildWatchFunc>(onPdfRenderComplete), data);
		g_free(pid);
	}
}

/**
 * Text-changed handler: when the Buffer in the dialog changes, this handler
 * updates currentTex, removes the previous existing render and creates a new
 * one. We need to do it through 'self' because signal handlers cannot directly
 * access non-static methods and non-static fields such as 'dlg' so we need to
 * wrap all the dlg method inside small methods in 'self'. To improve
 * performance, we render the text asynchronously.
 */
void LatexController::handleTexChanged(GtkTextBuffer* buffer, LatexController* self)
{
	XOJ_CHECK_TYPE_OBJ(self, LatexController);

	// Right now, this is the only way I know to extract text from TextBuffer
	self->setCurrentTex(gtk_text_buffer_get_text(buffer, self->getStartIterator(buffer), self->getEndIterator(buffer), TRUE));


	self->triggerImageUpdate(true);
}

void LatexController::onPdfRenderComplete(GPid pid, gint returnCode, PdfRenderCallbackData* data)
{
	LatexController* self = data->first;
	bool isPreview = data->second;
	XOJ_CHECK_TYPE_OBJ(self, LatexController);
	g_assert(self->isUpdating);
	GError* err = nullptr;
	g_spawn_check_exit_status(returnCode, &err);
	g_spawn_close_pid(pid);
	bool shouldUpdate = self->lastPreviewedTex != self->currentTex;
	if (err != nullptr)
	{
		self->isValidTex = false;
		if (!g_error_matches(err, G_SPAWN_EXIT_ERROR, 1))
		{
			// The error was not caused by invalid LaTeX.
			string message = FS(_F("pdflatex encountered an error: {1} (exit code: {2})") % err->message % err->code);
			g_warning(message.c_str());
			XojMsgBox::showErrorToUser(self->control->getGtkWindow(), message);
		}
		Path pdfPath = self->texTmp + "/tex.pdf";
		if (pdfPath.exists())
		{
			// Delete the pdf to prevent more errors
			pdfPath.deleteFile();
		}
		g_error_free(err);
	}
	else
	{
		self->isValidTex = true;
		if (isPreview)
		{
			self->deletePreviousRender();
			self->temporaryRender = self->loadRendered();
			if (self->getTemporaryRender() != NULL)
			{
				self->setImageInDialog(self->getTemporaryRender()->getPdf());
			}
		}
		else
		{
			self->insertTexImage();
		}

	}
	self->setUpdating(false);
	if (shouldUpdate)
	{
		self->triggerImageUpdate(true);
	}
	data->first = nullptr;
	delete data;
}

void LatexController::setUpdating(bool newValue)
{
	GtkWidget* okButton = this->dlg->get("texokbutton");
	if ((!this->isUpdating && newValue) || (this->isUpdating && !newValue))
	{
		// Disable LatexDialog OK button while updating. This is a workaround
		// for the fact that 1) the LatexController only lives while the dialog
		// is open; 2) the preview is generated asynchronously; and 3) the `run`
		// method that inserts the TexImage object is called synchronously after
		// the dialog is closed with the OK button.
		gtk_widget_set_sensitive(okButton, !newValue);
	}

	// Invalid LaTeX will generate an invalid PDF, so disable the OK button if
	// needed.
	gtk_widget_set_sensitive(okButton, this->isValidTex);

	GtkLabel* errorLabel = GTK_LABEL(this->dlg->get("texErrorLabel"));
	gtk_label_set_text(errorLabel, this->isValidTex ? "" : "The formula is empty when rendered or invalid.");

	this->isUpdating = newValue;
}

TexImage* LatexController::getTemporaryRender()
{
	XOJ_CHECK_TYPE(LatexController);
	return this->temporaryRender;
}

void LatexController::setImageInDialog(PopplerDocument* pdf)
{
	XOJ_CHECK_TYPE(LatexController);
	dlg->setTempRender(pdf, currentTex.size());
}

void LatexController::deletePreviousRender()
{
	XOJ_CHECK_TYPE(LatexController);
	delete temporaryRender;
	temporaryRender = NULL;
}

void LatexController::setCurrentTex(string currentTex)
{
	XOJ_CHECK_TYPE(LatexController);
	this->currentTex = currentTex;
}

GtkTextIter* LatexController::getStartIterator(GtkTextBuffer* buffer)
{
	XOJ_CHECK_TYPE(LatexController);
	gtk_text_buffer_get_start_iter(buffer, &this->start);
	return &this->start;
}

GtkTextIter* LatexController::getEndIterator(GtkTextBuffer* buffer)
{
	XOJ_CHECK_TYPE(LatexController);
	gtk_text_buffer_get_end_iter(buffer, &this->end);
	return &this->end;
}

void LatexController::deleteOldImage()
{
	XOJ_CHECK_TYPE(LatexController);

	if (selectedTexImage)
	{
		EditSelection* selection = new EditSelection(control->getUndoRedoHandler(), selectedTexImage, view, page);
		view->getXournal()->deleteSelection(selection);
		delete selection;
		selectedTexImage = NULL;
	}
	else if (selectedText)
	{
		EditSelection* selection = new EditSelection(control->getUndoRedoHandler(), selectedText, view, page);
		view->getXournal()->deleteSelection(selection);
		delete selection;
		selectedText = NULL;
	}
}

TexImage* LatexController::convertDocumentToImage(PopplerDocument* doc)
{
	XOJ_CHECK_TYPE(LatexController);

	if (poppler_document_get_n_pages(doc) < 1)
	{
		return NULL;
	}

	PopplerPage* page = poppler_document_get_page(doc, 0);


	double pageWidth = 0;
	double pageHeight = 0;
	poppler_page_get_size(page, &pageWidth, &pageHeight);

	TexImage* img = new TexImage();
	img->setX(posx);
	img->setY(posy);
	img->setText(currentTex);

	if (imgheight)
	{
		double ratio = pageWidth / pageHeight;
		if (ratio == 0)
		{
			if (imgwidth == 0)
			{
				img->setWidth(10);
			}
			else
			{
				img->setWidth(imgwidth);
			}
		}
		else
		{
			img->setWidth(imgheight * ratio);
		}
		img->setHeight(imgheight);
	}
	else
	{
		img->setWidth(pageWidth);
		img->setHeight(pageHeight);
	}

	return img;
}

/**
 * Load PDF as TexImage
 */
TexImage* LatexController::loadRendered()
{
	XOJ_CHECK_TYPE(LatexController);

	Path pdfPath = texTmp + "/tex.pdf";
	GError* err = NULL;

	if (!pdfPath.exists())
	{
		g_warning("LaTeX preview PDF file does not exist");
		return nullptr;
	}

	gchar* fileContents = NULL;
	gsize fileLength = 0;
	if (!g_file_get_contents(pdfPath.c_str(), &fileContents, &fileLength, &err))
	{
		XojMsgBox::showErrorToUser(control->getGtkWindow(),
				FS(_F("Could not load LaTeX PDF file, File Error: {1}") % err->message));
		g_error_free(err);
		return NULL;
	}

	PopplerDocument* pdf = poppler_document_new_from_data(fileContents, fileLength, NULL, &err);
	if (err != NULL)
	{
		string message = FS(_F("Could not load LaTeX PDF file: {1}") % err->message);
		g_message(message.c_str());
		XojMsgBox::showErrorToUser(control->getGtkWindow(), message);
		g_error_free(err);
		return NULL;
	}

	if (pdf == NULL)
	{
		XojMsgBox::showErrorToUser(control->getGtkWindow(), FS(_F("Could not load LaTeX PDF file")));
		return NULL;
	}

	TexImage* img = convertDocumentToImage(pdf);
	g_object_unref(pdf);

	// Do not assign the PDF, theoretical it should work, but it gets a Poppler PDF error
	// img->setPdf(pdf);
	img->setBinaryData(string(fileContents, fileLength));

	g_free(fileContents);

	return img;
}

void LatexController::insertTexImage()
{
	XOJ_CHECK_TYPE(LatexController);

	TexImage* img = loadRendered();

	deleteOldImage();

	doc->lock();
	layer->addElement(img);
	view->rerenderElement(img);
	doc->unlock();

	control->getUndoRedoHandler()->addUndoAction(new InsertUndoAction(page, layer, img));

	// Select element
	EditSelection* selection = new EditSelection(control->getUndoRedoHandler(), img, view, page);
	view->getXournal()->setSelection(selection);

	return;
}

void LatexController::run()
{
	XOJ_CHECK_TYPE(LatexController);

	if (!findTexExecutable())
	{
		string msg = _("Could not find pdflatex in Path.\nPlease install pdflatex first and make sure it's in the PATH.");
		XojMsgBox::showErrorToUser(control->getGtkWindow(), msg);
		return;
	}

	findSelectedTexElement();
	showTexEditDialog();

	if (StringUtils::trim(currentTex).empty() || initalTex == currentTex)
	{
		// Nothing to insert / change
		return;
	}

	// now do all the LatexAction stuff
	this->insertTexImage();
}
