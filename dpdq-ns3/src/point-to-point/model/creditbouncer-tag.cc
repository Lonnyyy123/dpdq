#include "creditbouncer-tag.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(CreditBouncerTag);

CreditBouncerTag::CreditBouncerTag()
	: m_type(CB_MSG_NONE),
	  m_creditReqBytes(0),
	  m_creditBytes(0),
	  m_csnMarked(0),
	  m_priorityFlow(0),
	  m_dataClass(CB_DATA_CLASS_NONE),
	  m_unsolCreditBytes(0),
	  m_unsolCreditDataBytes(0),
	  m_usedUnsolCredit(0),
	  m_creditPad(0),
	  m_resendStartIdx(0),
	  m_resendNumChunks(0)
{
}

CreditBouncerTag::CreditBouncerTag(MessageType type,
				   uint32_t creditReqBytes,
				   uint32_t creditBytes,
				   bool csnMarked,
				   bool priorityFlow,
				   DataClass dataClass,
				   uint32_t unsolCreditBytes,
				   uint32_t unsolCreditDataBytes,
				   bool usedUnsolCredit,
				   uint16_t creditPad,
				   uint32_t resendStartIdx,
				   uint32_t resendNumChunks)
	: m_type(static_cast<uint8_t>(type)),
	  m_creditReqBytes(creditReqBytes),
	  m_creditBytes(creditBytes),
	  m_csnMarked(csnMarked ? 1 : 0),
	  m_priorityFlow(priorityFlow ? 1 : 0),
	  m_dataClass(static_cast<uint8_t>(dataClass)),
	  m_unsolCreditBytes(unsolCreditBytes),
	  m_unsolCreditDataBytes(unsolCreditDataBytes),
	  m_usedUnsolCredit(usedUnsolCredit ? 1 : 0),
	  m_creditPad(creditPad),
	  m_resendStartIdx(resendStartIdx),
	  m_resendNumChunks(resendNumChunks)
{
}

TypeId CreditBouncerTag::GetTypeId(void)
{
	static TypeId tid = TypeId("ns3::CreditBouncerTag")
		.SetParent<Tag>()
		.AddConstructor<CreditBouncerTag>();
	return tid;
}

TypeId CreditBouncerTag::GetInstanceTypeId(void) const
{
	return GetTypeId();
}

uint32_t CreditBouncerTag::GetSerializedSize(void) const
{
	return 31;
}

void CreditBouncerTag::Serialize(TagBuffer i) const
{
	i.WriteU8(m_type);
	i.WriteU32(m_creditReqBytes);
	i.WriteU32(m_creditBytes);
	i.WriteU8(m_csnMarked);
	i.WriteU8(m_priorityFlow);
	i.WriteU8(m_dataClass);
	i.WriteU32(m_unsolCreditBytes);
	i.WriteU32(m_unsolCreditDataBytes);
	i.WriteU8(m_usedUnsolCredit);
	i.WriteU16(m_creditPad);
	i.WriteU32(m_resendStartIdx);
	i.WriteU32(m_resendNumChunks);
}

void CreditBouncerTag::Deserialize(TagBuffer i)
{
	m_type = i.ReadU8();
	m_creditReqBytes = i.ReadU32();
	m_creditBytes = i.ReadU32();
	m_csnMarked = i.ReadU8();
	m_priorityFlow = i.ReadU8();
	m_dataClass = i.ReadU8();
	m_unsolCreditBytes = i.ReadU32();
	m_unsolCreditDataBytes = i.ReadU32();
	m_usedUnsolCredit = i.ReadU8();
	m_creditPad = i.ReadU16();
	m_resendStartIdx = i.ReadU32();
	m_resendNumChunks = i.ReadU32();
}

void CreditBouncerTag::Print(std::ostream &os) const
{
	os << "type=" << static_cast<uint32_t>(m_type)
	   << " creditReqBytes=" << m_creditReqBytes
	   << " creditBytes=" << m_creditBytes
	   << " csnMarked=" << static_cast<uint32_t>(m_csnMarked)
	   << " priorityFlow=" << static_cast<uint32_t>(m_priorityFlow)
	   << " dataClass=" << static_cast<uint32_t>(m_dataClass)
	   << " unsolCreditBytes=" << m_unsolCreditBytes
	   << " unsolCreditDataBytes=" << m_unsolCreditDataBytes
	   << " usedUnsolCredit=" << static_cast<uint32_t>(m_usedUnsolCredit)
	   << " creditPad=" << m_creditPad
	   << " resendStartIdx=" << m_resendStartIdx
	   << " resendNumChunks=" << m_resendNumChunks;
}

void CreditBouncerTag::SetType(MessageType type)
{
	m_type = static_cast<uint8_t>(type);
}

CreditBouncerTag::MessageType CreditBouncerTag::GetType() const
{
	return static_cast<MessageType>(m_type);
}

void CreditBouncerTag::SetCreditReqBytes(uint32_t creditReqBytes)
{
	m_creditReqBytes = creditReqBytes;
}

uint32_t CreditBouncerTag::GetCreditReqBytes() const
{
	return m_creditReqBytes;
}

void CreditBouncerTag::SetCreditBytes(uint32_t creditBytes)
{
	m_creditBytes = creditBytes;
}

uint32_t CreditBouncerTag::GetCreditBytes() const
{
	return m_creditBytes;
}

void CreditBouncerTag::SetCsnMarked(bool csnMarked)
{
	m_csnMarked = csnMarked ? 1 : 0;
}

bool CreditBouncerTag::IsCsnMarked() const
{
	return m_csnMarked != 0;
}

void CreditBouncerTag::SetPriorityFlow(bool priorityFlow)
{
	m_priorityFlow = priorityFlow ? 1 : 0;
}

bool CreditBouncerTag::IsPriorityFlow() const
{
	return m_priorityFlow != 0;
}

void CreditBouncerTag::SetDataClass(DataClass dataClass)
{
	m_dataClass = static_cast<uint8_t>(dataClass);
}

CreditBouncerTag::DataClass CreditBouncerTag::GetDataClass() const
{
	return static_cast<DataClass>(m_dataClass);
}

bool CreditBouncerTag::IsUnsolicitedData() const
{
	return GetDataClass() == CB_DATA_CLASS_UNSOLICITED;
}

bool CreditBouncerTag::IsUnscheduledData() const
{
	return GetDataClass() == CB_DATA_CLASS_UNSCHEDULED;
}

bool CreditBouncerTag::IsScheduledData() const
{
	return GetDataClass() == CB_DATA_CLASS_SCHEDULED;
}

void CreditBouncerTag::SetUnsolCreditBytes(uint32_t unsolCreditBytes)
{
	m_unsolCreditBytes = unsolCreditBytes;
}

uint32_t CreditBouncerTag::GetUnsolCreditBytes() const
{
	return m_unsolCreditBytes;
}

void CreditBouncerTag::SetUnsolCreditDataBytes(uint32_t unsolCreditDataBytes)
{
	m_unsolCreditDataBytes = unsolCreditDataBytes;
}

uint32_t CreditBouncerTag::GetUnsolCreditDataBytes() const
{
	return m_unsolCreditDataBytes;
}

void CreditBouncerTag::SetUsedUnsolCredit(bool usedUnsolCredit)
{
	m_usedUnsolCredit = usedUnsolCredit ? 1 : 0;
}

bool CreditBouncerTag::IsUsedUnsolCredit() const
{
	return m_usedUnsolCredit != 0;
}

void CreditBouncerTag::SetCreditPad(uint16_t creditPad)
{
	m_creditPad = creditPad;
}

uint16_t CreditBouncerTag::GetCreditPad() const
{
	return m_creditPad;
}

void CreditBouncerTag::SetResendStartIdx(uint32_t resendStartIdx)
{
	m_resendStartIdx = resendStartIdx;
}

uint32_t CreditBouncerTag::GetResendStartIdx() const
{
	return m_resendStartIdx;
}

void CreditBouncerTag::SetResendNumChunks(uint32_t resendNumChunks)
{
	m_resendNumChunks = resendNumChunks;
}

uint32_t CreditBouncerTag::GetResendNumChunks() const
{
	return m_resendNumChunks;
}

bool CreditBouncerTag::IsGrantReq() const
{
	return GetType() == CB_MSG_GRANT_REQ;
}

bool CreditBouncerTag::IsRequest() const
{
	return GetType() == CB_MSG_REQUEST;
}

bool CreditBouncerTag::IsGrant() const
{
	return GetType() == CB_MSG_GRANT;
}

bool CreditBouncerTag::IsBouncedCredit() const
{
	return GetType() == CB_MSG_BOUNCED_CREDIT;
}

bool CreditBouncerTag::IsResend() const
{
	return GetType() == CB_MSG_RESEND;
}

} // namespace ns3
