#include "PdfExportJob.h"
#include "control/Control.h"
#include "pdf/base/XojPdfExport.h"
#include "pdf/base/XojPdfExportFactory.h"

#include <i18n.h>

#include <boost/algorithm/string/predicate.hpp>


PdfExportJob::PdfExportJob(Control* control)
 : BaseExportJob(control, _("PDF Export"))
{
	XOJ_INIT_TYPE(PdfExportJob);
}

PdfExportJob::~PdfExportJob()
{
	XOJ_RELEASE_TYPE(PdfExportJob);
}

void PdfExportJob::addFilterToDialog()
{
	XOJ_CHECK_TYPE(PdfExportJob);

	addFileFilterToDialog(_("PDF files"), "*.pdf");
}

bool PdfExportJob::isUriValid(string& uri)
{
	XOJ_CHECK_TYPE(PdfExportJob);

	if (!BaseExportJob::isUriValid(uri))
	{
		return false;
	}

	// Remove any pre-existing extension and adds .pdf
	clearExtensions(filename);
	filename += ".pdf";
	
	return checkOverwriteBackgroundPDF(filename);
}


void PdfExportJob::run()
{
	XOJ_CHECK_TYPE(PdfExportJob);

	Document* doc = control->getDocument();

	doc->lock();
	XojPdfExport* pdfe = XojPdfExportFactory::createExport(doc, control);
	doc->unlock();

	if (!pdfe->createPdf(this->filename))
	{
		if (control->getWindow())
		{
			callAfterRun();
		}
		else
		{
			this->errorMsg = pdfe->getLastError();
		}
	}

	delete pdfe;
}

