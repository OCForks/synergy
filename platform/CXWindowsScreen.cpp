#include "CXWindowsScreen.h"
#include "CXWindowsClipboard.h"
#include "CXWindowsScreenSaver.h"
#include "CXWindowsUtil.h"
#include "CClipboard.h"
#include "IScreenEventHandler.h"
#include "IScreenReceiver.h"
#include "XScreen.h"
#include "CLock.h"
#include "CThread.h"
#include "CLog.h"
#include "IJob.h"
#include <cstring>
#if defined(X_DISPLAY_MISSING)
#	error X11 is required to build synergy
#else
#	include <X11/X.h>
#	include <X11/Xutil.h>
#endif
#if UNIX_LIKE
#include <sys/poll.h>
#endif

//
// CXWindowsScreen::CTimer
//

CXWindowsScreen::CTimer::CTimer(IJob* job, double timeout) :
	m_job(job),
	m_timeout(timeout)
{
	assert(m_job != NULL);
	assert(m_timeout > 0.0);

	reset();
}

CXWindowsScreen::CTimer::~CTimer()
{
	// do nothing
}

void
CXWindowsScreen::CTimer::run()
{
	m_job->run();
}

void
CXWindowsScreen::CTimer::reset()
{
	m_time = m_timeout;
}

CXWindowsScreen::CTimer::CTimer&
CXWindowsScreen::CTimer::operator-=(double dt)
{
	m_time -= dt;
	return *this;
}

CXWindowsScreen::CTimer::operator double() const
{
	return m_time;
}

bool
CXWindowsScreen::CTimer::operator<(const CTimer& t) const
{
	return m_time < t.m_time;
}


//
// CXWindowsScreen
//

CXWindowsScreen*		CXWindowsScreen::s_screen = NULL;

CXWindowsScreen::CXWindowsScreen(IScreenReceiver* receiver,
				IScreenEventHandler* eventHandler) :
	m_display(NULL),
	m_root(None),
	m_stop(false),
	m_receiver(receiver),
	m_eventHandler(eventHandler),
	m_window(None),
	m_x(0), m_y(0),
	m_w(0), m_h(0),
	m_screensaver(NULL),
	m_screensaverNotify(false),
	m_atomScreensaver(None)
{
	assert(s_screen       == NULL);
	assert(m_receiver     != NULL);
	assert(m_eventHandler != NULL);

	s_screen = this;

	// no clipboards to start with
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		m_clipboard[id] = NULL;
	}
}

CXWindowsScreen::~CXWindowsScreen()
{
	assert(s_screen  != NULL);
	assert(m_display == NULL);

	s_screen = NULL;
}

void
CXWindowsScreen::addTimer(IJob* job, double timeout)
{
	CLock lock(&m_timersMutex);
	removeTimerNoLock(job);
	m_timers.push(CTimer(job, timeout));
}

void
CXWindowsScreen::removeTimer(IJob* job)
{
	CLock lock(&m_timersMutex);
	removeTimerNoLock(job);
}

void
CXWindowsScreen::removeTimerNoLock(IJob* job)
{
	// do it the hard way.  first collect all jobs that are not
	// the removed job.
	CTimerPriorityQueue::container_type tmp;
	for (CTimerPriorityQueue::iterator index = m_timers.begin();
								index != m_timers.end(); ++index) {
		if (index->getJob() != job) {
			tmp.push_back(*index);
		}
	}

	// now swap in the new list
	m_timers.swap(tmp);
}

void
CXWindowsScreen::setWindow(Window window)
{
	CLock lock(&m_mutex);
	assert(m_display != NULL);

	// destroy the clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		delete m_clipboard[id];
		m_clipboard[id] = NULL;
	}

	// save the new window
	m_window = window;

	// initialize the clipboards
	if (m_window != None) {
		for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
			m_clipboard[id] = new CXWindowsClipboard(m_display, m_window, id);
		}
	}
}

Window
CXWindowsScreen::getRoot() const
{
	assert(m_display != NULL);
	return m_root;
}

Cursor
CXWindowsScreen::getBlankCursor() const
{
	return m_cursor;
}

void
CXWindowsScreen::open()
{
	assert(m_display == NULL);

	// set the X I/O error handler so we catch the display disconnecting
	XSetIOErrorHandler(&CXWindowsScreen::ioErrorHandler);

	// get the DISPLAY
	const char* display = getenv("DISPLAY");
	if (display == NULL) {
		display = ":0.0";
	}

	// open the display
	log((CLOG_DEBUG "XOpenDisplay(\"%s\")", display));
	m_display = XOpenDisplay(display);
	if (m_display == NULL) {
		throw XScreenOpenFailure();
	}

	// get root window
	m_root = DefaultRootWindow(m_display);

	// create the transparent cursor
	createBlankCursor();

	// get screen shape
	updateScreenShape();

	// initialize the screen saver
	m_atomScreensaver = XInternAtom(m_display, "SCREENSAVER", False);
	m_screensaver     = new CXWindowsScreenSaver(this, m_display);
}

void
CXWindowsScreen::mainLoop()
{
	// wait for an event in a cancellable way and don't lock the
	// display while we're waiting.
	CEvent event;
	m_mutex.lock();

#if UNIX_LIKE

	// use poll() to wait for a message from the X server or for timeout.
	// this is a good deal more efficient than polling and sleeping.
	struct pollfd pfds[1];
	pfds[0].fd     = ConnectionNumber(m_display);
	pfds[0].events = POLLIN;
	while (!m_stop) {
		// compute timeout to next timer
		int timeout = (m_timers.empty() ? -1 :
								static_cast<int>(1000.0 * m_timers.top()));

		// wait for message from X server or for timeout.  also check
		// if the thread has been cancelled.  poll() should return -1
		// with EINTR when the thread is cancelled.
		m_mutex.unlock();
		poll(pfds, 1, timeout);
		CThread::testCancel();
		m_mutex.lock();

		// process timers
		processTimers();

		// handle pending events
		while (!m_stop && XPending(m_display) > 0) {
			// get the event
			XNextEvent(m_display, &event.m_event);

			// process the event.  if unhandled then let the subclass
			// have a go at it.
			m_mutex.unlock();
			if (!onPreDispatch(&event)) {
				m_eventHandler->onEvent(&event);
			}
			m_mutex.lock();
		}
	}

#else // !UNIX_LIKE

	// poll and sleep
	while (!m_stop) {
		// poll for pending events and process timers
		while (!m_stop && XPending(m_display) == 0) {
			// check timers
			if (processTimers()) {
				continue;
			}

			// wait
			m_mutex.unlock();
			CThread::sleep(0.01);
			m_mutex.lock();
		}

		// process events
		while (!m_stop && XPending(m_display) > 0) {
			// get the event
			XNextEvent(m_display, &event.m_event);

			// process the event.  if unhandled then let the subclass
			// have a go at it.
			m_mutex.unlock();
			if (!onPreDispatch(&event)) {
				m_eventHandler->onEvent(&event);
			}
			m_mutex.lock();
		}
	}

#endif // !UNIX_LIKE

	m_mutex.unlock();
}

void
CXWindowsScreen::exitMainLoop()
{
	CLock lock(&m_mutex);
	m_stop = true;
}

void
CXWindowsScreen::close()
{
	CLock lock(&m_mutex);

	// done with screen saver
	delete m_screensaver;
	m_screensaver = NULL;

	// destroy clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		delete m_clipboard[id];
		m_clipboard[id] = NULL;
	}

	// close the display
	if (m_display != NULL) {
		XCloseDisplay(m_display);
		m_display = NULL;
		log((CLOG_DEBUG "closed display"));
	}
	XSetIOErrorHandler(NULL);
}

bool
CXWindowsScreen::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
	CLock lock(&m_mutex);

	// fail if we don't have the requested clipboard
	if (m_clipboard[id] == NULL) {
		return false;
	}

	// get the actual time.  ICCCM does not allow CurrentTime.
	Time timestamp = CXWindowsUtil::getCurrentTime(
								m_display, m_clipboard[id]->getWindow());

	if (clipboard != NULL) {
		// save clipboard data
		return CClipboard::copy(m_clipboard[id], clipboard, timestamp);
	}
	else {
		// assert clipboard ownership
		if (!m_clipboard[id]->open(timestamp)) {
			return false;
		}
		m_clipboard[id]->empty();
		m_clipboard[id]->close();
		return true;
	}
}

void
CXWindowsScreen::checkClipboards()
{
	// do nothing, we're always up to date
}

void
CXWindowsScreen::openScreensaver(bool notify)
{
	CLock lock(&m_mutex);
	assert(m_screensaver != NULL);

	m_screensaverNotify = notify;
	if (m_screensaverNotify) {
		m_screensaver->setNotify(m_window);
	}
	else {
		m_screensaver->disable();
	}
}

void
CXWindowsScreen::closeScreensaver()
{
	CLock lock(&m_mutex);
	if (m_screensaver != NULL) {
		if (m_screensaverNotify) {
			m_screensaver->setNotify(None);
		}
		else {
			m_screensaver->enable();
		}
	}
}

void
CXWindowsScreen::screensaver(bool activate)
{
	CLock lock(&m_mutex);
	assert(m_screensaver != NULL);

	if (activate) {
		m_screensaver->activate();
	}
	else {
		m_screensaver->deactivate();
	}
}

void
CXWindowsScreen::syncDesktop()
{
	// do nothing;  X doesn't suffer from this bogosity
}

bool
CXWindowsScreen::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
	assert(clipboard != NULL);

	// block others from using the display while we get the clipboard
	CLock lock(&m_mutex);

	// fail if we don't have the requested clipboard
	if (m_clipboard[id] == NULL) {
		return false;
	}

	// get the actual time.  ICCCM does not allow CurrentTime.
	Time timestamp = CXWindowsUtil::getCurrentTime(
								m_display, m_clipboard[id]->getWindow());

	// copy the clipboard
	return CClipboard::copy(clipboard, m_clipboard[id], timestamp);
}

void
CXWindowsScreen::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
	CLock lock(&m_mutex);
	assert(m_display != NULL);

	x = m_x;
	y = m_y;
	w = m_w;
	h = m_h;
}

void
CXWindowsScreen::getCursorPos(SInt32& x, SInt32& y) const
{
	CLock lock(&m_mutex);
	assert(m_display != NULL);

	Window root, window;
	int mx, my, xWindow, yWindow;
	unsigned int mask;
	if (XQueryPointer(m_display, getRoot(), &root, &window,
								&mx, &my, &xWindow, &yWindow, &mask)) {
		x = mx;
		y = my;
	}
	else {
		getCursorCenter(x, y);
	}
}

void
CXWindowsScreen::getCursorCenter(SInt32& x, SInt32& y) const
{
	CLock lock(&m_mutex);
	assert(m_display != NULL);

	x = m_x + (m_w >> 1);
	y = m_y + (m_h >> 1);
}

void
CXWindowsScreen::updateScreenShape()
{
	m_x = 0;
	m_y = 0;
	m_w = WidthOfScreen(DefaultScreenOfDisplay(m_display));
	m_h = HeightOfScreen(DefaultScreenOfDisplay(m_display));
	log((CLOG_INFO "screen shape: %d,%d %dx%d", m_x, m_y, m_w, m_h));
}

bool
CXWindowsScreen::onPreDispatch(CEvent* event)
{
	assert(event != NULL);
	XEvent* xevent = &event->m_event;

	switch (xevent->type) {
	case MappingNotify:
		// keyboard mapping changed
		XRefreshKeyboardMapping(&xevent->xmapping);

		// pass event on
		break;

	case SelectionClear:
		{
			// we just lost the selection.  that means someone else
			// grabbed the selection so this screen is now the
			// selection owner.  report that to the receiver.
			ClipboardID id = getClipboardID(xevent->xselectionclear.selection);
			if (id != kClipboardEnd) {
				log((CLOG_DEBUG "lost clipboard %d ownership at time %d", id, xevent->xselectionclear.time));
				m_clipboard[id]->lost(xevent->xselectionclear.time);
				m_receiver->onGrabClipboard(id);
				return true;
			}
		}
		break;

	case SelectionNotify:
		// notification of selection transferred.  we shouldn't
		// get this here because we handle them in the selection
		// retrieval methods.  we'll just delete the property
		// with the data (satisfying the usual ICCCM protocol).
		if (xevent->xselection.property != None) {
			CLock lock(&m_mutex);
			XDeleteProperty(m_display,
								xevent->xselection.requestor,
								xevent->xselection.property);
		}
		return true;

	case SelectionRequest:
		{
			// somebody is asking for clipboard data
			ClipboardID id = getClipboardID(
								xevent->xselectionrequest.selection);
			if (id != kClipboardEnd) {
				CLock lock(&m_mutex);
				m_clipboard[id]->addRequest(
								xevent->xselectionrequest.owner,
								xevent->xselectionrequest.requestor,
								xevent->xselectionrequest.target,
								xevent->xselectionrequest.time,
								xevent->xselectionrequest.property);
				return true;
			}
		}
		break;

	case PropertyNotify:
		// property delete may be part of a selection conversion
		if (xevent->xproperty.state == PropertyDelete) {
			processClipboardRequest(xevent->xproperty.window,
								xevent->xproperty.time,
								xevent->xproperty.atom);
			return true;
		}
		break;

	case ClientMessage:
		if (xevent->xclient.message_type == m_atomScreensaver ||
			xevent->xclient.format       == 32) {
			// screen saver activation/deactivation event
			m_eventHandler->onScreensaver(xevent->xclient.data.l[0] != 0);
			return true;
		}
		break;

	case DestroyNotify:
		// looks like one of the windows that requested a clipboard
		// transfer has gone bye-bye.
		destroyClipboardRequest(xevent->xdestroywindow.window);

		// we don't know if the event was handled or not so continue
		break;
	}

	// let screen saver have a go
	{
		CLock lock(&m_mutex);
		m_screensaver->onPreDispatch(xevent);
	}

	return m_eventHandler->onPreDispatch(event);
}

void
CXWindowsScreen::createBlankCursor()
{
	// this seems just a bit more complicated than really necessary

	// get the closet cursor size to 1x1
	unsigned int w, h;
	XQueryBestCursor(m_display, m_root, 1, 1, &w, &h);

	// make bitmap data for cursor of closet size.  since the cursor
	// is blank we can use the same bitmap for shape and mask:  all
	// zeros.
	const int size = ((w + 7) >> 3) * h;
	char* data = new char[size];
	memset(data, 0, size);

	// make bitmap
	Pixmap bitmap = XCreateBitmapFromData(m_display, m_root, data, w, h);

	// need an arbitrary color for the cursor
	XColor color;
	color.pixel = 0;
	color.red   = color.green = color.blue = 0;
	color.flags = DoRed | DoGreen | DoBlue;

	// make cursor from bitmap
	m_cursor = XCreatePixmapCursor(m_display, bitmap, bitmap,
								&color, &color, 0, 0);

	// don't need bitmap or the data anymore
	delete[] data;
	XFreePixmap(m_display, bitmap);
}

bool
CXWindowsScreen::processTimers()
{
	std::vector<IJob*> jobs;
	{
		CLock lock(&m_timersMutex);

		// get current time
		const double time = m_time.getTime();

		// done if no timers have expired
		if (m_timers.empty() || m_timers.top() > time) {
			return false;
		}

		// subtract current time from all timers.  note that this won't
		// change the order of elements in the priority queue (except
		// for floating point round off which we'll ignore).
		for (CTimerPriorityQueue::iterator index = m_timers.begin();
								index != m_timers.end(); ++index) {
			(*index) -= time;
		}

		// process all timers at or below zero, saving the jobs
		while (m_timers.top() <= 0.0) {
			CTimer timer = m_timers.top();
			jobs.push_back(timer.getJob());
			timer.reset();
			m_timers.pop();
			m_timers.push(timer);
		}

		// reset the clock
		m_time.reset();
	}

	// now run the jobs.  note that if one of these jobs removes
	// a timer later in the jobs list and deletes that job pointer
	// then this will crash when it tries to run that job.
	for (std::vector<IJob*>::iterator index = jobs.begin();
								index != jobs.end(); ++index) {
		(*index)->run();
	}
}

ClipboardID
CXWindowsScreen::getClipboardID(Atom selection) const
{
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		if (m_clipboard[id] != NULL &&
			m_clipboard[id]->getSelection() == selection) {
			return id;
		}
	}
	return kClipboardEnd;
}

void
CXWindowsScreen::processClipboardRequest(Window requestor,
				Time time, Atom property)
{
	CLock lock(&m_mutex);

	// check every clipboard until one returns success
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		if (m_clipboard[id] != NULL &&
			m_clipboard[id]->processRequest(requestor, time, property)) {
			break;
		}
	}
}

void
CXWindowsScreen::destroyClipboardRequest(Window requestor)
{
	CLock lock(&m_mutex);

	// check every clipboard until one returns success
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		if (m_clipboard[id] != NULL &&
			m_clipboard[id]->destroyRequest(requestor)) {
			break;
		}
	}
}

int
CXWindowsScreen::ioErrorHandler(Display*)
{
	// the display has disconnected, probably because X is shutting
	// down.  X forces us to exit at this point.  that's arguably
	// a flaw in X but, realistically, it's difficult to gracefully
	// handle not having a Display* anymore.  we'll simply log the
	// error, notify the subclass (which must not use the display
	// so we set it to NULL), and exit.
	log((CLOG_WARN "X display has unexpectedly disconnected"));
	s_screen->m_display = NULL;
	s_screen->m_receiver->onError();
	log((CLOG_CRIT "quiting due to X display disconnection"));
	exit(17);
}


//
// CDisplayLock
//

CDisplayLock::CDisplayLock(const CXWindowsScreen* screen) :
	m_mutex(&screen->m_mutex),
	m_display(screen->m_display)
{
	// note -- it's permitted for m_display to be NULL.  that might
	// happen if we couldn't connect to the display or if the
	// display unexpectedly disconnected.  the caller is expected
	// to check for NULL as necessary.

	m_mutex->lock();
}

CDisplayLock::~CDisplayLock()
{
	m_mutex->unlock();
}

CDisplayLock::operator Display*() const
{
	return m_display;
}
