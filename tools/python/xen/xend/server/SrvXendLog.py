# Copyright (C) 2004 Mike Wray <mike.wray@hp.com>

from xen.web import static

from xen.xend import XendRoot

from xen.web.SrvDir import SrvDir

class SrvXendLog(SrvDir):
    """Xend log.
    """

    def __init__(self):
        SrvDir.__init__(self)
        logging = XendRoot.instance().get_logging()
        self.logfile = static.File(logging.getLogFilename(), defaultType="text/plain")
        self.logfile.type = "text/plain"
        self.logfile.encoding = None

    def render_GET(self, req):
        return self.logfile.render(req)
