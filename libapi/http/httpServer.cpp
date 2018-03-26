#include <chrono>
#include <thread>
#include <mongoose/mongoose.h>
#include <unistd.h>
#include <limits.h>
#include "httpServer.h"
#include "libdevcore/Log.h"
#include "libdevcore/Common.h"
#include "miner-buildinfo.h"

using namespace dev;
using namespace eth;

httpServer http_server;


void httpServer::getstat1(stringstream& ss)
{
	char hostName[HOST_NAME_MAX + 1];
	gethostname(hostName, HOST_NAME_MAX + 1);
	using namespace std::chrono;
	auto info = miner_get_buildinfo();
	WorkingProgress p = m_farm->miningProgress();
	SolutionStats s = m_farm->getSolutionStats();
	string l = m_farm->farmLaunchedFormatted();
	ss <<
	   "<head><title>Miner Stats</title></head><body><table width=\"50%\" border=1 cellpadding=2 cellspacing=0 align=center>"
	   "<tr valign=top align=center><th colspan=5>" << info->project_version <<
	   " on " << hostName << "</th></tr>"
	   "<tr valign=top align=center>"
	   "<th>GPU</th><th>Hash Rate (mh/s)</th><th>Temperature (C)</th><th>Fan Percent.</th><th>Power (W)</th></tr>";
	double hashSum = 0.0;
	double powerSum = 0.0;
	for (unsigned i = 0; i < p.minersHashes.size(); i++) {
		double rate = p.minerRate(p.minersHashes[i]) / 1000000.0;
		hashSum += rate;
		ss <<
		   "<tr valign=top align=center><td>" << i <<
		   "</td><td>" << fixed << setprecision(2) << rate;
		if (i < p.minerMonitors.size()) {
			HwMonitor& hw(p.minerMonitors[i]);
			powerSum += hw.powerW;
			ss << "</td><td>" << hw.tempC << "</td><td>" << hw.fanP << "</td><td>" <<
			   fixed << setprecision(0) << hw.powerW << "</td></tr>";
		} else
			ss << "</td><td>-</td><td>-</td><td>-</td></tr>";
	}
	ss <<
	   "<tr valign=top align=center><th>Total</th><td>" <<
	   fixed << setprecision(2) << hashSum << "</td><td colspan=2>" << s <<
	   "</td><td>" << fixed << setprecision(0) << powerSum << "</td></tr>"
	   "</table></body></html>";
}

static void static_getstat1(stringstream& ss)
{
	http_server.getstat1(ss);
}

void ev_handler(struct mg_connection* c, int ev, void* p)
{

	if (ev == MG_EV_HTTP_REQUEST) {
		struct http_message* hm = (struct http_message*) p;
		char uri[32];
		unsigned uriLen = hm->uri.len;
		if (uriLen >= sizeof(uri) - 1)
			uriLen = sizeof(uri) - 1;
		memcpy(uri, hm->uri.p, uriLen);
		uri[uriLen] = 0;
		if (::strcmp(uri, "/getstat1"))
			mg_http_send_error(c, 404, nullptr);
		else {
			stringstream content;
			static_getstat1(content);
			mg_send_head(c, 200, (int)content.str().length(), "Content-Type: text/html");
			mg_printf(c, "%.*s", (int)content.str().length(), content.str().c_str());
		}
	}
}

void httpServer::run(unsigned short port, dev::eth::Farm* farm)
{
	m_farm = farm;
	m_port = to_string(port);
	new thread(bind(&httpServer::run_thread, this));
}

void httpServer::run_thread()
{
	struct mg_mgr mgr;
	struct mg_connection* c;

	mg_mgr_init(&mgr, NULL);
	loginfo << "Starting web server on port " << m_port << '\n';
	c = mg_bind(&mgr, m_port.c_str(), ev_handler);
	if (c == NULL) {
		logerror << "Failed to create listener\n";
		return;
	}

	// Set up HTTP server parameters
	mg_set_protocol_http_websocket(c);

	for (;;)
		mg_mgr_poll(&mgr, 1000);
}

httpServer::httpServer()
{
}

httpServer::~httpServer()
{
}


