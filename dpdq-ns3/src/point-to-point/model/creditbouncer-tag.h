#ifndef CREDITBOUNCER_TAG_H
#define CREDITBOUNCER_TAG_H

#include "ns3/tag.h"

namespace ns3 {

class CreditBouncerTag : public Tag
{
public:
	enum MessageType : uint8_t {
		CB_MSG_NONE = 0,
		CB_MSG_GRANT_REQ = 1,
		CB_MSG_REQUEST = 2,
		CB_MSG_GRANT = 3,
		// Credit returned by a switch when port-level FC state is PAUSE.
		CB_MSG_BOUNCED_CREDIT = 4,
		// Receiver-side resend request carrying missing chunk ranges.
		CB_MSG_RESEND = 5,
	};
	enum DataClass : uint8_t {
		CB_DATA_CLASS_NONE = 0,
		CB_DATA_CLASS_UNSOLICITED = 1,
		CB_DATA_CLASS_UNSCHEDULED = 2,
		CB_DATA_CLASS_SCHEDULED = 3,
	};

	CreditBouncerTag();
	CreditBouncerTag(MessageType type,
			 uint32_t creditReqBytes,
			 uint32_t creditBytes,
			 bool csnMarked = false,
			 bool priorityFlow = false,
			 DataClass dataClass = CB_DATA_CLASS_NONE,
			 uint32_t unsolCreditBytes = 0,
			 uint32_t unsolCreditDataBytes = 0,
			 bool usedUnsolCredit = false,
			 uint16_t creditPad = 0,
			 uint32_t resendStartIdx = 0,
			 uint32_t resendNumChunks = 0);

	static TypeId GetTypeId(void);
	virtual TypeId GetInstanceTypeId(void) const;
	virtual uint32_t GetSerializedSize(void) const;
	virtual void Serialize(TagBuffer i) const;
	virtual void Deserialize(TagBuffer i);
	virtual void Print(std::ostream &os) const;

	void SetType(MessageType type);
	MessageType GetType() const;

	void SetCreditReqBytes(uint32_t creditReqBytes);
	uint32_t GetCreditReqBytes() const;

	void SetCreditBytes(uint32_t creditBytes);
	uint32_t GetCreditBytes() const;

	void SetCsnMarked(bool csnMarked);
	bool IsCsnMarked() const;

	void SetPriorityFlow(bool priorityFlow);
	bool IsPriorityFlow() const;

	void SetDataClass(DataClass dataClass);
	DataClass GetDataClass() const;
	bool IsUnsolicitedData() const;
	bool IsUnscheduledData() const;
	bool IsScheduledData() const;

	void SetUnsolCreditBytes(uint32_t unsolCreditBytes);
	uint32_t GetUnsolCreditBytes() const;

	void SetUnsolCreditDataBytes(uint32_t unsolCreditDataBytes);
	uint32_t GetUnsolCreditDataBytes() const;

	void SetUsedUnsolCredit(bool usedUnsolCredit);
	bool IsUsedUnsolCredit() const;

	void SetCreditPad(uint16_t creditPad);
	uint16_t GetCreditPad() const;

	void SetResendStartIdx(uint32_t resendStartIdx);
	uint32_t GetResendStartIdx() const;

	void SetResendNumChunks(uint32_t resendNumChunks);
	uint32_t GetResendNumChunks() const;

	bool IsGrantReq() const;
	bool IsRequest() const;
	bool IsGrant() const;
	bool IsBouncedCredit() const;
	bool IsResend() const;

	private:
		// Encodes whether this packet is a GRANT_REQ, REQUEST data packet, or GRANT.
		uint8_t m_type;
		// Message size/demand announced to the receiver in payload bytes.
		uint32_t m_creditReqBytes;
		// Granted credit carried by a GRANT packet, measured in on-wire bytes.
		uint32_t m_creditBytes;
		// Host-side congestion signal attached to scheduled data packets.
		uint8_t m_csnMarked;
		// Whether this REQUEST data packet belongs to the always-SRPT priority flow.
		uint8_t m_priorityFlow;
		// Sender-side classification of the packet's data: unsolicited, unscheduled, or scheduled.
		uint8_t m_dataClass;
		// Initial self-allocated credit announced on the first data packet, in on-wire bytes.
		uint32_t m_unsolCreditBytes;
		// Initial self-allocated credit announced on the first data packet, in payload bytes.
		uint32_t m_unsolCreditDataBytes;
		// Whether this specific packet consumes bytes from the self-allocated unsolicited budget.
		uint8_t m_usedUnsolCredit;
		// Padding bytes implied by a short grant so sender can reconstruct payload credit exactly.
		uint16_t m_creditPad;
		// First missing chunk index requested by a RESEND control packet.
		uint32_t m_resendStartIdx;
		// Number of missing chunks requested by a RESEND control packet.
		uint32_t m_resendNumChunks;
	};

} // namespace ns3

#endif // CREDITBOUNCER_TAG_H
