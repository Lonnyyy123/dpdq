#include "switch-cb-mmu.h"

#include <algorithm>
#include <iostream>
#include <limits>

#include "ns3/assert.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

NS_LOG_COMPONENT_DEFINE("SwitchCbMmu");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(SwitchCbMmu);

TypeId SwitchCbMmu::GetTypeId(void) {
	static TypeId tid = TypeId("ns3::SwitchCbMmu")
		.SetParent<Object>()
		.AddConstructor<SwitchCbMmu>()
		.AddAttribute("NodeId",
			"Owning switch node id for debug logs.",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchCbMmu::SetNodeId, &SwitchCbMmu::GetNodeId),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("ActivePortCnt",
			"Number of active switch ports.",
			UintegerValue(12),
			MakeUintegerAccessor(&SwitchCbMmu::SetActivePortCnt, &SwitchCbMmu::GetActivePortCnt),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("MaxTotalBufferPerPort",
			"Maximum shared buffer size contribution per active port in bytes.",
			UintegerValue(375 * 1000),
			MakeUintegerAccessor(&SwitchCbMmu::SetMaxBufferBytesPerPort, &SwitchCbMmu::GetMaxBufferBytesPerPort),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditQueueLimitBytes",
			"Fixed queue size per egress port for CreditBouncer control packets.",
			UintegerValue(64 * 1024),
			MakeUintegerAccessor(&SwitchCbMmu::SetCreditQueueLimitBytes, &SwitchCbMmu::GetCreditQueueLimitBytes),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("DataHeadroomBytes",
			"Legacy compatibility field retained for configs/logging; ignored by the current dedicated DPDQ MMU admission rule.",
			UintegerValue(12500 + 2 * MTU),
			MakeUintegerAccessor(&SwitchCbMmu::SetDefaultDataHeadroomBytes, &SwitchCbMmu::GetDefaultDataHeadroomBytes),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("DataGuaranteeBytes",
			"Legacy compatibility field retained for configs/logging; ignored by the current dedicated DPDQ MMU admission rule.",
			UintegerValue(1048),
			MakeUintegerAccessor(&SwitchCbMmu::SetDefaultDataGuaranteeBytes, &SwitchCbMmu::GetDefaultDataGuaranteeBytes),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("DataEcnKminBytes",
			"Data queue ECN minimum threshold in bytes.",
			UintegerValue(64 * 1024),
			MakeUintegerAccessor(&SwitchCbMmu::SetDataEcnKminBytes, &SwitchCbMmu::GetDataEcnKminBytes),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("DataEcnKmaxBytes",
			"Data queue ECN maximum threshold in bytes.",
			UintegerValue(96 * 1024),
			MakeUintegerAccessor(&SwitchCbMmu::SetDataEcnKmaxBytes, &SwitchCbMmu::GetDataEcnKmaxBytes),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("DataAdmissionAlpha",
			"Admission coefficient alpha2 in qd + pkt <= alpha2 * (Bs - sumQd).",
			DoubleValue(1.0),
			MakeDoubleAccessor(&SwitchCbMmu::SetDataAdmissionAlpha, &SwitchCbMmu::GetDataAdmissionAlpha),
			MakeDoubleChecker<double>(0.0))
		.AddAttribute("DataEcnPmax",
			"Data queue ECN max probability.",
			DoubleValue(1.0),
			MakeDoubleAccessor(&SwitchCbMmu::SetDataEcnPmax, &SwitchCbMmu::GetDataEcnPmax),
			MakeDoubleChecker<double>())
		.AddAttribute("DebugLog",
			"Enable CB-MMU debug logging.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchCbMmu::SetDebugLog, &SwitchCbMmu::GetDebugLog),
			MakeBooleanChecker())
		.AddAttribute("DebugMaxLogs",
			"Maximum number of CB-MMU debug logs.",
			UintegerValue(2000),
			MakeUintegerAccessor(&SwitchCbMmu::SetDebugMaxLogs, &SwitchCbMmu::GetDebugMaxLogs),
			MakeUintegerChecker<uint32_t>());
	return tid;
}

SwitchCbMmu::SwitchCbMmu()
	: m_nodeId(0),
	  m_activePortCnt(12),
	  m_maxBufferBytesPerPort(375 * 1000),
	  m_staticMaxBufferBytes(0),
	  m_maxBufferBytes(0),
	  m_creditQueueLimitBytes(64 * 1024),
	  m_defaultDataHeadroomBytes(12500 + 2 * MTU),
	  m_defaultDataGuaranteeBytes(1048),
	  m_dataEcnKminBytes(64 * 1024),
	  m_dataEcnKmaxBytes(96 * 1024),
	  m_dataAdmissionAlpha(1.0),
	  m_dataEcnPmax(1.0),
	  m_dataSharedLimitBytes(0),
	  m_dataSharedUsedBytes(0),
	  m_debugLog(false),
	  m_debugMaxLogs(2000),
	  m_debugLogCount(0) {
	m_uniformRandom.SetStream(0);
	InitSwitch();
}

void SwitchCbMmu::InitSwitch(void) {
	m_maxBufferBytes = m_staticMaxBufferBytes ? m_staticMaxBufferBytes : (m_maxBufferBytesPerPort * m_activePortCnt);
	m_dataSharedUsedBytes = 0;

	for (uint32_t i = 0; i < pCnt; ++i) {
		m_creditQueueBytes[i] = 0;
		m_dataQueueBytes[i] = 0;
		m_dataGuaranteedBytesByPort[i] = 0;
		m_dataSharedBytesByPort[i] = 0;
		m_dataHeadroomBytesByPort[i] = 0;
		if (i == 0 || i > m_activePortCnt) {
			m_dataGuaranteeLimitBytes[i] = 0;
			m_dataHeadroomLimitBytes[i] = 0;
		} else {
			if (m_dataGuaranteeLimitBytes[i] == 0) {
				m_dataGuaranteeLimitBytes[i] = m_defaultDataGuaranteeBytes;
			}
			if (m_dataHeadroomLimitBytes[i] == 0) {
				m_dataHeadroomLimitBytes[i] = m_defaultDataHeadroomBytes;
			}
		}
	}

	RecomputeDataSharedLimit();

	LogEvent("init", "mmu", 0, 0, 0, 0, true, "reinitialized");
}

void SwitchCbMmu::RecomputeDataSharedLimit(void) {
	// DPDQ dedicated MMU model:
	// 1) Reserve a small fixed credit queue per egress port.
	// 2) Data queue uses the remaining shared memory budget Bs.
	// Legacy guarantee/headroom fields are intentionally ignored.
	uint64_t totalCreditReserve = static_cast<uint64_t>(m_creditQueueLimitBytes) * m_activePortCnt;
	uint64_t dataBudget = m_maxBufferBytes > totalCreditReserve
		? (static_cast<uint64_t>(m_maxBufferBytes) - totalCreditReserve)
		: 0;
	m_dataSharedLimitBytes = static_cast<uint32_t>(
		std::min<uint64_t>(dataBudget, std::numeric_limits<uint32_t>::max()));
}

bool SwitchCbMmu::CheckCreditAdmission(uint32_t port, uint32_t psize) const {
	if (port == 0 || port >= pCnt) {
		LogEvent("check", "credit", port, psize, 0, 0, false, "invalid_port");
		return false;
	}
	uint32_t before = m_creditQueueBytes[port];
	bool admitted = before + psize <= m_creditQueueLimitBytes;
	LogEvent("check", "credit", port, psize, before, before, admitted,
		admitted ? "ok" : "credit_queue_full");
	return admitted;
}

void SwitchCbMmu::UpdateCreditAdmission(uint32_t port, uint32_t psize) {
	if (port == 0 || port >= pCnt) {
		LogEvent("enqueue", "credit", port, psize, 0, 0, false, "invalid_port");
		return;
	}
	uint32_t before = m_creditQueueBytes[port];
	m_creditQueueBytes[port] += psize;
	LogEvent("enqueue", "credit", port, psize, before, m_creditQueueBytes[port], true, "ok");
}

void SwitchCbMmu::RemoveCreditAdmission(uint32_t port, uint32_t psize) {
	if (port == 0 || port >= pCnt) {
		LogEvent("dequeue", "credit", port, psize, 0, 0, false, "invalid_port");
		return;
	}
	uint32_t before = m_creditQueueBytes[port];
	m_creditQueueBytes[port] = (m_creditQueueBytes[port] >= psize) ? (m_creditQueueBytes[port] - psize) : 0;
	LogEvent("dequeue", "credit", port, psize, before, m_creditQueueBytes[port], true, "ok");
}

bool SwitchCbMmu::CheckDataAdmission(uint32_t port, uint32_t psize) const {
	if (port == 0 || port >= pCnt) {
		LogEvent("check", "data", port, psize, 0, 0, false, "invalid_port");
		return false;
	}

	uint64_t qdBefore = m_dataQueueBytes[port];
	uint64_t qdAfter = qdBefore + psize;
	uint64_t sumQdBefore = m_dataSharedUsedBytes;
	uint64_t bs = m_dataSharedLimitBytes;
	uint64_t available = bs > sumQdBefore ? (bs - sumQdBefore) : 0;
	uint64_t threshold = static_cast<uint64_t>(
		m_dataAdmissionAlpha * static_cast<double>(available));
	bool admitted = qdAfter <= threshold;
	LogEvent("check", "data", port, psize, m_dataQueueBytes[port], m_dataQueueBytes[port], admitted,
		admitted ? "arrival_rule_ok" : "arrival_rule_exceeded");
	return admitted;
}

void SwitchCbMmu::UpdateDataAdmission(uint32_t port, uint32_t psize) {
	if (port == 0 || port >= pCnt || psize == 0) {
		LogEvent("enqueue", "data", port, psize, 0, 0, false, "invalid_input");
		return;
	}

	uint32_t before = m_dataQueueBytes[port];
	m_dataQueueBytes[port] += psize;
	m_dataSharedBytesByPort[port] += psize;
	m_dataSharedUsedBytes += psize;
	LogEvent("enqueue", "data", port, psize, before, m_dataQueueBytes[port], true, "used_shared");
}

void SwitchCbMmu::RemoveDataAdmission(uint32_t port, uint32_t psize) {
	if (port == 0 || port >= pCnt || psize == 0) {
		LogEvent("dequeue", "data", port, psize, 0, 0, false, "invalid_input");
		return;
	}

	uint32_t before = m_dataQueueBytes[port];
	uint32_t oldDataBytes = before;
	m_dataQueueBytes[port] = (m_dataQueueBytes[port] >= psize) ? (m_dataQueueBytes[port] - psize) : 0;
	uint32_t removeBytes = std::min(psize, oldDataBytes);

	uint32_t removeShared = std::min(removeBytes, m_dataSharedBytesByPort[port]);
	m_dataSharedBytesByPort[port] -= removeShared;
	m_dataSharedUsedBytes = (m_dataSharedUsedBytes >= removeShared) ? (m_dataSharedUsedBytes - removeShared) : 0;
	LogEvent("dequeue", "data", port, psize, before, m_dataQueueBytes[port], true,
		removeShared > 0 ? "released_shared" : "released_empty");
}

bool SwitchCbMmu::ShouldSendCn(uint32_t port) const {
	if (port == 0 || port >= pCnt) {
		return false;
	}
	uint32_t qlen = m_dataQueueBytes[port];
	if (qlen > m_dataEcnKmaxBytes) {
		return true;
	}
	if (qlen <= m_dataEcnKminBytes || m_dataEcnKminBytes >= m_dataEcnKmaxBytes) {
		return false;
	}
	double p = 1.0 * (qlen - m_dataEcnKminBytes) / (m_dataEcnKmaxBytes - m_dataEcnKminBytes) * m_dataEcnPmax;
	return m_uniformRandom.GetValue(0.0, 1.0) < p;
}

uint32_t SwitchCbMmu::GetCreditQueueBytes(uint32_t port) const {
	return (port < pCnt) ? m_creditQueueBytes[port] : 0;
}

uint32_t SwitchCbMmu::GetDataQueueBytes(uint32_t port) const {
	return (port < pCnt) ? m_dataQueueBytes[port] : 0;
}

uint32_t SwitchCbMmu::GetTotalDataQueueBytes(void) const {
	uint64_t totalDataBytes = 0;
	for (uint32_t port = 1; port <= m_activePortCnt && port < pCnt; ++port) {
		totalDataBytes += m_dataQueueBytes[port];
	}
	return static_cast<uint32_t>(std::min<uint64_t>(totalDataBytes, std::numeric_limits<uint32_t>::max()));
}

uint32_t SwitchCbMmu::GetDedicatedFcBsBytes(void) const {
	uint64_t totalCreditReserve = static_cast<uint64_t>(m_creditQueueLimitBytes) * m_activePortCnt;
	uint64_t dedicatedFcBs = m_maxBufferBytes > totalCreditReserve
		? (static_cast<uint64_t>(m_maxBufferBytes) - totalCreditReserve)
		: 0;
	return static_cast<uint32_t>(std::min<uint64_t>(dedicatedFcBs, std::numeric_limits<uint32_t>::max()));
}

uint32_t SwitchCbMmu::GetDataSharedUsedBytes(void) const {
	return m_dataSharedUsedBytes;
}

uint32_t SwitchCbMmu::GetDataSharedLimitBytes(void) const {
	return m_dataSharedLimitBytes;
}

uint32_t SwitchCbMmu::GetDataHeadroomUsedBytes(uint32_t port) const {
	return (port < pCnt) ? m_dataHeadroomBytesByPort[port] : 0;
}

uint32_t SwitchCbMmu::GetDataGuaranteeLimitBytes(uint32_t port) const {
	return (port < pCnt) ? m_dataGuaranteeLimitBytes[port] : 0;
}

void SwitchCbMmu::SetActivePortCnt(uint32_t v) {
	if (v == m_activePortCnt) {
		return;
	}
	m_activePortCnt = std::min<uint32_t>(v, pCnt - 1);
	InitSwitch();
}

uint32_t SwitchCbMmu::GetActivePortCnt(void) const {
	return m_activePortCnt;
}

void SwitchCbMmu::SetMaxBufferBytesPerPort(uint32_t v) {
	if (v == m_maxBufferBytesPerPort) {
		return;
	}
	m_maxBufferBytesPerPort = v;
	InitSwitch();
}

uint32_t SwitchCbMmu::GetMaxBufferBytesPerPort(void) const {
	return m_maxBufferBytesPerPort;
}

void SwitchCbMmu::ConfigBufferSize(uint32_t size) {
	if (size == m_staticMaxBufferBytes) {
		return;
	}
	m_staticMaxBufferBytes = size;
	InitSwitch();
}

void SwitchCbMmu::SetCreditQueueLimitBytes(uint32_t v) {
	if (v == m_creditQueueLimitBytes) {
		return;
	}
	m_creditQueueLimitBytes = v;
	RecomputeDataSharedLimit();
}

uint32_t SwitchCbMmu::GetCreditQueueLimitBytes(void) const {
	return m_creditQueueLimitBytes;
}

void SwitchCbMmu::SetDefaultDataHeadroomBytes(uint32_t v) {
	m_defaultDataHeadroomBytes = v;
	for (uint32_t i = 1; i <= m_activePortCnt && i < pCnt; ++i) {
		m_dataHeadroomLimitBytes[i] = v;
	}
}

uint32_t SwitchCbMmu::GetDefaultDataHeadroomBytes(void) const {
	return m_defaultDataHeadroomBytes;
}

void SwitchCbMmu::SetDefaultDataGuaranteeBytes(uint32_t v) {
	m_defaultDataGuaranteeBytes = v;
	for (uint32_t i = 1; i <= m_activePortCnt && i < pCnt; ++i) {
		m_dataGuaranteeLimitBytes[i] = v;
	}
}

uint32_t SwitchCbMmu::GetDefaultDataGuaranteeBytes(void) const {
	return m_defaultDataGuaranteeBytes;
}

void SwitchCbMmu::ConfigDataHeadroom(uint32_t port, uint32_t size) {
	if (port == 0 || port >= pCnt) {
		return;
	}
	m_dataHeadroomLimitBytes[port] = size;
}

void SwitchCbMmu::ConfigDataHeadroomByBdp(uint32_t port, uint64_t rateBps, uint64_t linkDelayNs) {
	if (port == 0 || port >= pCnt || rateBps == 0 || linkDelayNs == 0) {
		return;
	}

	// Match the legacy switch-mmu-style value for compatibility/debugging, even
	// though the current dedicated DPDQ MMU admission rule ignores headroom.
	long double bdpBytes = (static_cast<long double>(rateBps) * static_cast<long double>(linkDelayNs)) / 8000000000.0L;
	long double headroom = 2.0L * bdpBytes + 2.0L * static_cast<long double>(MTU);
	uint64_t headroomBytes64 = headroom <= 0.0L ? 0ULL : static_cast<uint64_t>(headroom);
	uint32_t headroomBytes = headroomBytes64 > std::numeric_limits<uint32_t>::max()
		? std::numeric_limits<uint32_t>::max()
		: static_cast<uint32_t>(headroomBytes64);

	m_dataHeadroomLimitBytes[port] = headroomBytes;
}

void SwitchCbMmu::ConfigDataGuarantee(uint32_t port, uint32_t size) {
	if (port == 0 || port >= pCnt) {
		return;
	}
	m_dataGuaranteeLimitBytes[port] = size;
}

void SwitchCbMmu::SetDataEcnKminBytes(uint32_t v) {
	m_dataEcnKminBytes = v;
}

uint32_t SwitchCbMmu::GetDataEcnKminBytes(void) const {
	return m_dataEcnKminBytes;
}

void SwitchCbMmu::SetDataEcnKmaxBytes(uint32_t v) {
	m_dataEcnKmaxBytes = v;
}

uint32_t SwitchCbMmu::GetDataEcnKmaxBytes(void) const {
	return m_dataEcnKmaxBytes;
}

void SwitchCbMmu::SetDataAdmissionAlpha(double v) {
	m_dataAdmissionAlpha = v;
}

double SwitchCbMmu::GetDataAdmissionAlpha(void) const {
	return m_dataAdmissionAlpha;
}

void SwitchCbMmu::SetDataEcnPmax(double v) {
	m_dataEcnPmax = v;
}

double SwitchCbMmu::GetDataEcnPmax(void) const {
	return m_dataEcnPmax;
}

void SwitchCbMmu::SetNodeId(uint32_t v) {
	m_nodeId = v;
}

uint32_t SwitchCbMmu::GetNodeId(void) const {
	return m_nodeId;
}

void SwitchCbMmu::SetDebugLog(bool v) {
	m_debugLog = v;
}

bool SwitchCbMmu::GetDebugLog(void) const {
	return m_debugLog;
}

void SwitchCbMmu::SetDebugMaxLogs(uint32_t v) {
	m_debugMaxLogs = v;
}

uint32_t SwitchCbMmu::GetDebugMaxLogs(void) const {
	return m_debugMaxLogs;
}

void SwitchCbMmu::LogEvent(const char *event,
	const char *queueKind,
	uint32_t port,
	uint32_t psize,
	uint32_t beforeBytes,
	uint32_t afterBytes,
	bool admitted,
	const char *reason) const {
	if (!m_debugLog || m_debugLogCount >= m_debugMaxLogs) {
		return;
	}
	std::cerr << "CBMMU event=" << event
		<< " node=" << m_nodeId
		<< " qkind=" << queueKind
		<< " port=" << port
		<< " psize=" << psize
		<< " before=" << beforeBytes
		<< " after=" << afterBytes
		<< " admitted=" << (admitted ? 1 : 0)
		<< " reason=" << reason
		<< " credit_q_limit=" << m_creditQueueLimitBytes
		<< " data_guarantee_used=" << (port < pCnt ? m_dataGuaranteedBytesByPort[port] : 0)
		<< " data_guarantee_limit=" << (port < pCnt ? m_dataGuaranteeLimitBytes[port] : 0)
		<< " data_adm_alpha=" << m_dataAdmissionAlpha
		<< " data_shared_used=" << m_dataSharedUsedBytes
		<< " data_shared_limit=" << m_dataSharedLimitBytes;
	if (port < pCnt) {
		std::cerr << " data_headroom_used=" << m_dataHeadroomBytesByPort[port]
			<< " data_headroom_limit=" << m_dataHeadroomLimitBytes[port];
	}
	std::cerr << "\n";
	m_debugLogCount++;
}

} // namespace ns3
