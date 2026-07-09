#include "r2p2-hdr.h"
#include <stdexcept>

int hdr_r2p2::offset_;

class R2p2HeaderClass : public PacketHeaderClass
{
public:
    R2p2HeaderClass() : PacketHeaderClass("PacketHeader/R2P2",
                                          sizeof(hdr_r2p2))
    {
        bind_offset(&hdr_r2p2::offset_);
    }
} class_r2p2hdr;

static const char *pkt_type_names[] = {
    "Request",
    "Reply",
    "ReqRdy",
    "Feedback",
    "Drop",
    "Sack",
    "Freeze",
    "Unfreeze",
    "Credit",
    "CreditReq",
    "Resend"};

const char *
hdr_r2p2::get_pkt_type(int type)
{
    if (type >= hdr_r2p2::NUMBER_OF_TYPES)
    {
        throw std::out_of_range("Attempted to access type: " + std::to_string(type));
    }
    return pkt_type_names[type];
}
