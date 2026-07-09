#ifndef SWITCH_CB_MMU_H
#define SWITCH_CB_MMU_H

#include <ns3/object.h>
#include <ns3/random-variable-stream.h>

namespace ns3 {

class SwitchCbMmu : public Object {
public:
	static const unsigned pCnt = 128;
	static const unsigned MTU = 1048; // align with SwitchMmu::MTU

	static TypeId GetTypeId(void);

	SwitchCbMmu();
	void InitSwitch(void);

	bool CheckCreditAdmission(uint32_t port, uint32_t psize) const;
	void UpdateCreditAdmission(uint32_t port, uint32_t psize);
	void RemoveCreditAdmission(uint32_t port, uint32_t psize);

	bool CheckDataAdmission(uint32_t port, uint32_t psize) const;
	void UpdateDataAdmission(uint32_t port, uint32_t psize);
	void RemoveDataAdmission(uint32_t port, uint32_t psize);

	bool ShouldSendCn(uint32_t port) const;

	uint32_t GetCreditQueueBytes(uint32_t port) const;
	uint32_t GetDataQueueBytes(uint32_t port) const;
	uint32_t GetTotalDataQueueBytes(void) const;
	uint32_t GetDedicatedFcBsBytes(void) const;
	uint32_t GetDataSharedUsedBytes(void) const;
	uint32_t GetDataSharedLimitBytes(void) const;
	uint32_t GetDataHeadroomUsedBytes(uint32_t port) const;
	uint32_t GetDataGuaranteeLimitBytes(uint32_t port) const;

	void SetActivePortCnt(uint32_t v);
	uint32_t GetActivePortCnt(void) const;

	void SetMaxBufferBytesPerPort(uint32_t v);
	uint32_t GetMaxBufferBytesPerPort(void) const;

	void ConfigBufferSize(uint32_t size);

	void SetCreditQueueLimitBytes(uint32_t v);
	uint32_t GetCreditQueueLimitBytes(void) const;

	// Legacy compatibility knobs retained for configs/logging. The current
	// dedicated DPDQ MMU admission rule ignores guarantee/headroom.
	void SetDefaultDataHeadroomBytes(uint32_t v);
	uint32_t GetDefaultDataHeadroomBytes(void) const;
	void SetDefaultDataGuaranteeBytes(uint32_t v);
	uint32_t GetDefaultDataGuaranteeBytes(void) const;

	void ConfigDataHeadroom(uint32_t port, uint32_t size);
	void ConfigDataHeadroomByBdp(uint32_t port, uint64_t rateBps, uint64_t linkDelayNs);
	void ConfigDataGuarantee(uint32_t port, uint32_t size);

	void SetDataEcnKminBytes(uint32_t v);
	uint32_t GetDataEcnKminBytes(void) const;
	void SetDataEcnKmaxBytes(uint32_t v);
	uint32_t GetDataEcnKmaxBytes(void) const;
	void SetDataAdmissionAlpha(double v);
	double GetDataAdmissionAlpha(void) const;
	void SetDataEcnPmax(double v);
	double GetDataEcnPmax(void) const;

	void SetNodeId(uint32_t v);
	uint32_t GetNodeId(void) const;

	void SetDebugLog(bool v);
	bool GetDebugLog(void) const;

	void SetDebugMaxLogs(uint32_t v);
	uint32_t GetDebugMaxLogs(void) const;

private:
	void RecomputeDataSharedLimit(void);
	void LogEvent(const char *event,
		const char *queueKind,
		uint32_t port,
		uint32_t psize,
		uint32_t beforeBytes,
		uint32_t afterBytes,
		bool admitted,
		const char *reason) const;

	uint32_t m_nodeId;
	uint32_t m_activePortCnt;
	uint32_t m_maxBufferBytesPerPort;
	uint32_t m_staticMaxBufferBytes;
	uint32_t m_maxBufferBytes;

	uint32_t m_creditQueueLimitBytes;
	// Compatibility-only fields: retained so older configs still parse cleanly,
	// but they do not affect the current dedicated DPDQ MMU admission rule.
	uint32_t m_defaultDataHeadroomBytes;
	uint32_t m_defaultDataGuaranteeBytes;

	uint32_t m_dataEcnKminBytes;
	uint32_t m_dataEcnKmaxBytes;
	double m_dataAdmissionAlpha;
	double m_dataEcnPmax;

	uint32_t m_creditQueueBytes[pCnt];
	uint32_t m_dataQueueBytes[pCnt];
	// Data queue is accounted as a single shared pool across ports. The legacy
	// guarantee/headroom counters remain for compatibility/debug visibility.
	uint32_t m_dataGuaranteedBytesByPort[pCnt];
	uint32_t m_dataSharedBytesByPort[pCnt];
	uint32_t m_dataHeadroomBytesByPort[pCnt];
	uint32_t m_dataGuaranteeLimitBytes[pCnt];
	uint32_t m_dataHeadroomLimitBytes[pCnt];

	uint32_t m_dataSharedLimitBytes;
	uint32_t m_dataSharedUsedBytes;

	bool m_debugLog;
	uint32_t m_debugMaxLogs;
	mutable uint32_t m_debugLogCount;

	mutable UniformRandomVariable m_uniformRandom;
};

} // namespace ns3

#endif // SWITCH_CB_MMU_H
