#include "xpass.h"
#include "../tcp/template.h"

int hdr_xpass::offset_;
static class XPassHeaderClass : public PacketHeaderClass
{
public:
    XPassHeaderClass() : PacketHeaderClass("PacketHeader/XPass", sizeof(hdr_xpass))
    {
        bind_offset(&hdr_xpass::offset_);
    }
} class_xpass_hdr;

static class XPassClass : public TclClass
{
public:
    XPassClass() : TclClass("Agent/XPass") {}
    TclObject *create(int, const char *const *)
    {
        return (new XPassAgent());
    }
} class_xpass;

void SendCreditTimer::expire(Event *)
{
    a_->send_credit();
}

void CreditStopTimer::expire(Event *)
{
    a_->send_credit_stop();
}

void SenderRetransmitTimer::expire(Event *)
{
    a_->handle_sender_retransmit();
}

void ReceiverRetransmitTimer::expire(Event *)
{
    a_->handle_receiver_retransmit();
}

void FCTTimer::expire(Event *)
{
    a_->handle_fct();
}

void XPassAgent::delay_bind_init_all()
{
    delay_bind_init_one("max_credit_rate_");
    delay_bind_init_one("alpha_");
    delay_bind_init_one("min_credit_size_");
    delay_bind_init_one("max_credit_size_");
    delay_bind_init_one("min_ethernet_size_");
    delay_bind_init_one("max_ethernet_size_");
    delay_bind_init_one("xpass_hdr_size_");
    delay_bind_init_one("target_loss_scaling_");
    delay_bind_init_one("w_init_");
    delay_bind_init_one("min_w_");
    delay_bind_init_one("retransmit_timeout_");
    delay_bind_init_one("default_credit_stop_timeout_");
    delay_bind_init_one("min_jitter_");
    delay_bind_init_one("max_jitter_");
    Agent::delay_bind_init_all();
}

int XPassAgent::delay_bind_dispatch(const char *varName, const char *localName,
                                    TclObject *tracer)
{
    if (delay_bind(varName, localName, "max_credit_rate_", &max_credit_rate_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "alpha_", &alpha_, tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "min_credit_size_", &min_credit_size_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "max_credit_size_", &max_credit_size_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "min_ethernet_size_", &min_ethernet_size_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "max_ethernet_size_", &max_ethernet_size_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "xpass_hdr_size_", &xpass_hdr_size_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "target_loss_scaling_", &target_loss_scaling_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "w_init_", &w_init_, tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "min_w_", &min_w_, tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "retransmit_timeout_", &retransmit_timeout_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "default_credit_stop_timeout_", &default_credit_stop_timeout_,
                   tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "max_jitter_", &max_jitter_, tracer))
    {
        return TCL_OK;
    }
    if (delay_bind(varName, localName, "min_jitter_", &min_jitter_, tracer))
    {
        return TCL_OK;
    }
    return Agent::delay_bind_dispatch(varName, localName, tracer);
}

void XPassAgent::init()
{
    w_ = w_init_;
    //  cur_credit_rate_ = (int)(alpha_ * max_credit_rate_);
    last_credit_rate_update_ = now();
}

int XPassAgent::command(int argc, const char *const *argv)
{
    if (argc == 2)
    {
        if (strcmp(argv[1], "listen") == 0)
        {
            listen();
            return TCL_OK;
        }
        else if (strcmp(argv[1], "stop") == 0)
        {
            // on_transmission_ = false;
            return TCL_OK;
        }
    }
    else if (argc == 3)
    {
        if (strcmp(argv[1], "advance-bytes") == 0)
        {
            if (credit_recv_state_ == XPASS_RECV_CLOSED)
            {
                advance_bytes(atol(argv[2]));
                return TCL_OK;
            }
            else
            {
                return TCL_ERROR;
            }
        }
    }
    return Agent::command(argc, argv);
}

void XPassAgent::recv(Packet *pkt, Handler *)
{
    slog::log6(debug_, addr(), " XPassAgent::recv(Packet *pkt, Handler *)");
    hdr_cmn *cmnh = hdr_cmn::access(pkt);

    switch (cmnh->ptype())
    {
    case PT_XPASS_CREDIT_REQUEST:
        recv_credit_request(pkt);
        break;
    case PT_XPASS_CREDIT:
        recv_credit(pkt);
        break;
    case PT_XPASS_DATA:
        recv_data(pkt);
        break;
    case PT_XPASS_CREDIT_STOP:
        recv_credit_stop(pkt);
        break;
    case PT_XPASS_NACK:
        recv_nack(pkt);
        break;
    default:
        break;
    }
    Packet::free(pkt);
}

void XPassAgent::recv_credit_request(Packet *pkt)
{
    slog::log6(debug_, addr(), "XPassAgent::recv_credit_request(Packet *pkt)");
    hdr_xpass *xph = hdr_xpass::access(pkt);

    switch (credit_send_state_)
    {
    case XPASS_SEND_CLOSE_WAIT:
        fct_timer_.force_cancel();
    case XPASS_SEND_CLOSED:
        double lalpha;
        init();
        if (xph->sendbuffer_ >= 40)
        {
            lalpha = alpha_;
        }
        else
        {
            lalpha = alpha_ * xph->sendbuffer_ / 40.0;
        }
        cur_credit_rate_ = (int)(lalpha * max_credit_rate_);
        fst_ = xph->credit_sent_time();
        // need to start to send credits.
        send_credit();

        // XPASS_SEND_CLOSED -> XPASS_SEND_CREDIT_REQUEST_RECEIVED
        credit_send_state_ = XPASS_SEND_CREDIT_SENDING;
        break;
    }
}

void XPassAgent::recv_credit(Packet *pkt)
{
    slog::log6(debug_, addr(), "XPassAgent::recv_credit()");
    credit_recved_rtt_++;
    switch (credit_recv_state_)
    {
    case XPASS_RECV_CREDIT_REQUEST_SENT:
        slog::log6(debug_, addr(), "Cancelling sender_retransmit_timer because XPASS_RECV_CREDIT_REQUEST_SENT");
        sender_retransmit_timer_.force_cancel();
        slog::log6(debug_, addr(), "Going into XPASS_RECV_CREDIT_RECEIVING");
        credit_recv_state_ = XPASS_RECV_CREDIT_RECEIVING;
        // first sender RTT.
        rtt_ = now() - rtt_;
        last_credit_recv_update_ = now();
    case XPASS_RECV_CREDIT_RECEIVING:
        // send data
        if (datalen_remaining() > 0)
        {
            send(construct_data(pkt), 0);
        }

        if (datalen_remaining() == 0)
        {
            if (credit_stop_timer_.status() != TIMER_IDLE)
            {
                fprintf(stderr, "Error: CreditStopTimer seems to be scheduled more than once.\n");
                exit(1);
            }
            // Because ns2 does not allow sending two consecutive packets,
            // credit_stop_timer_ schedules CREDIT_STOP packet with no delay.
            credit_stop_timer_.sched(0);
        }
        else if (now() - last_credit_recv_update_ >= rtt_)
        {
            if (credit_recved_rtt_ >= (1 * pkt_remaining()))
            {
                // Early credit stop
                if (credit_stop_timer_.status() != TIMER_IDLE)
                {
                    fprintf(stderr, "Error: CreditStopTimer seems to be scheduled more than once.\n");
                    exit(1);
                }
                // Because ns2 does not allow sending two consecutive packets,
                // credit_stop_timer_ schedules CREDIT_STOP packet with no delay.
                credit_stop_timer_.sched(0);
            }
            credit_recved_rtt_ = 0;
            last_credit_recv_update_ = now();
        }
        break;
    case XPASS_RECV_CREDIT_STOP_SENT:
        if (datalen_remaining() > 0)
        {
            send(construct_data(pkt), 0);
        }
        else
        {
            credit_wasted_++;
        }
        credit_recved_++;
        break;
    case XPASS_RECV_CLOSE_WAIT:
        // accumulate credit count to check if credit stop has been delivered
        credit_wasted_++;
        break;
    case XPASS_RECV_CLOSED:
        credit_wasted_++;
        break;
    }
}

void XPassAgent::recv_data(Packet *pkt)
{
    slog::log6(debug_, addr(), "XPassAgent::recv_data()");
    hdr_xpass *xph = hdr_xpass::access(pkt);
    // distance between expected sequence number and actual sequence number.
    int distance = xph->credit_seq() - c_recv_next_;

    if (distance < 0)
    {
        // credit packet reordering or credit sequence number overflow happend.
        fprintf(stderr, "ERROR: Credit Sequence number is reverted.\n");
        exit(1);
    }
    credit_total_ += (distance + 1);
    credit_dropped_ += distance;

    c_recv_next_ = xph->credit_seq() + 1;

    process_ack(pkt);
    update_rtt(pkt);

    // give packet to application layer
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    RequestIdTuple req_id = RequestIdTuple();
    req_id.app_level_id_ = r2p2_hdr->app_level_id();
    req_id.msg_bytes_ = r2p2_hdr->msg_bytes();
    req_id.is_request_ = (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST);
    req_id.cl_thread_id_ = r2p2_hdr->cl_thread_id();
    req_id.cl_addr_ = r2p2_hdr->cl_addr();
    req_id.sr_addr_ = r2p2_hdr->sr_addr();
    req_id.client_port_ = dport();
    req_id.ts_ = r2p2_hdr->msg_creation_time();
    int datalen = hdr_cmn::access(pkt)->size() - hdr_tcp::access(pkt)->hlen();

    // correct for when the last pkt of the message was below min ethernet size
    int max_payload = max_ethernet_size_ - xpass_hdr_size_; // 1460
    int last_pkt_payload = req_id.msg_bytes_ % max_payload;
    // 84 - 78 = 6
    if ((last_pkt_payload < min_ethernet_size_ - xpass_hdr_size_) && (datalen < max_payload))
    {
        datalen = last_pkt_payload;
    }

    slog::log6(debug_, addr(), "XPassAgent::recv_data() (rcver). datalen=", datalen,
               "hdr_cmn::access(pkt)->size()=", hdr_cmn::access(pkt)->size(),
               "hdr_tcp::access(pkt)->hlen()=", hdr_tcp::access(pkt)->hlen(),
               "xph->credit_seq(): ", xph->credit_seq(), "c_recv_next_:", c_recv_next_,
               "last_pkt_payload:", last_pkt_payload, "req_id.msg_bytes_:", req_id.msg_bytes_,
               "req_id.is_request_:", req_id.is_request_);
    app_->recv_msg(datalen, std::move(req_id));
}

void XPassAgent::recv_nack(Packet *pkt)
{
    slog::log6(debug_, addr(), "XPassAgent::recv_nack()");
    assert(0); // SIRD xpass port. Should this happen with infinite buffers?
    hdr_tcp *tcph = hdr_tcp::access(pkt);
    switch (credit_recv_state_)
    {
    case XPASS_RECV_CREDIT_STOP_SENT:
    case XPASS_RECV_CLOSE_WAIT:
    case XPASS_RECV_CLOSED:
        send(construct_credit_request(), 0);
        slog::log6(debug_, addr(), "Going into XPASS_RECV_CREDIT_REQUEST_SENT");
        credit_recv_state_ = XPASS_RECV_CREDIT_REQUEST_SENT;
        sender_retransmit_timer_.resched(retransmit_timeout_);
    case XPASS_RECV_CREDIT_REQUEST_SENT:
    case XPASS_RECV_CREDIT_RECEIVING:
        // set t_seqno_ for retransmission
        t_seqno_ = tcph->ackno();
    }
}

void XPassAgent::recv_credit_stop(Packet *pkt)
{
    slog::log6(debug_, addr(), "XPassAgent::recv_credit_stop()");
    fct_ = now() - fst_;
    fct_timer_.sched(default_credit_stop_timeout_);
    send_credit_timer_.force_cancel();
    credit_send_state_ = XPASS_SEND_CLOSE_WAIT;

    // msg complete, give it to application layer
    // app_->recv_msg()
}

void XPassAgent::handle_fct()
{
    slog::log6(debug_, addr(), "XPassAgent::handle_fct()");
    // FILE *fct_out = fopen("outputs/fct.out", "a");

    // fprintf(fct_out, "%d,%ld,%.10lf\n", fid_, recv_next_ - 1, fct_);
    // fclose(fct_out);
    credit_send_state_ = XPASS_SEND_CLOSED;
}

void XPassAgent::handle_sender_retransmit()
{
    slog::log6(debug_, addr(), "XPassAgent::handle_sender_retransmit()");
    // assert(0); // I guess there shouldn;t be any of this with infinite buffers // apparently it is happening - sender_retransmit_timer_ schedules it when sending credit stop?
    switch (credit_recv_state_)
    {
    case XPASS_RECV_CREDIT_REQUEST_SENT:
        send(construct_credit_request(), 0);
        sender_retransmit_timer_.resched(retransmit_timeout_);
        break;
    case XPASS_RECV_CREDIT_STOP_SENT:
        if (datalen_remaining() > 0)
        {
            slog::log6(debug_, addr(), "Going into XPASS_RECV_CREDIT_REQUEST_SENT");
            credit_recv_state_ = XPASS_RECV_CREDIT_REQUEST_SENT;
            send(construct_credit_request(), 0);
            sender_retransmit_timer_.resched(retransmit_timeout_);
        }
        else
        {
            slog::log6(debug_, addr(), "Going into XPASS_RECV_CLOSE_WAIT");
            credit_recv_state_ = XPASS_RECV_CLOSE_WAIT;
            credit_recved_ = 0;
            sender_retransmit_timer_.resched((rtt_ > 0) ? rtt_ : default_credit_stop_timeout_);
        }
        break;
    case XPASS_RECV_CLOSE_WAIT:
        if (credit_recved_ == 0)
        {
            // FILE *waste_out = fopen("outputs/waste.out", "a");

            slog::log6(debug_, addr(), "Going into XPASS_RECV_CLOSED");
            credit_recv_state_ = XPASS_RECV_CLOSED;
            slog::log6(debug_, addr(), "Cancelling sender_retransmit_timer because XPASS_RECV_CLOSE_WAIT");
            sender_retransmit_timer_.force_cancel();
            // fprintf(waste_out, "%d,%ld,%d\n", fid_, curseq_ - 1, credit_wasted_);
            // fclose(waste_out);
            return;
        }
        // retransmit credit_stop
        send_credit_stop();
        break;
    case XPASS_RECV_CLOSED:
        fprintf(stderr, "Sender Retransmit triggered while connection is closed.");
        exit(1);
    }
}

void XPassAgent::handle_receiver_retransmit()
{
    slog::log6(debug_, addr(), "XPassAgent::handle_receiver_retransmit()");
    assert(0); // again, should not happen with inf buffers right?
    if (wait_retransmission_)
    {
        send(construct_nack(recv_next_), 0);
        receiver_retransmit_timer_.resched(retransmit_timeout_);
    }
}

Packet *XPassAgent::construct_credit_request()
{
    slog::log6(debug_, addr(), "XPassAgent::construct_credit_request()");
    Packet *p = allocpkt();
    if (!p)
    {
        fprintf(stderr, "ERROR: allockpkt() failed\n");
        exit(1);
    }

    hdr_tcp *tcph = hdr_tcp::access(p);
    hdr_cmn *cmnh = hdr_cmn::access(p);
    hdr_xpass *xph = hdr_xpass::access(p);

    tcph->seqno() = t_seqno_;
    tcph->ackno() = recv_next_;
    tcph->hlen() = xpass_hdr_size_;

    cmnh->size() = min_ethernet_size_;
    cmnh->ptype() = PT_XPASS_CREDIT_REQUEST;

    xph->credit_seq() = 0;
    xph->credit_sent_time_ = now();
    xph->sendbuffer_ = pkt_remaining();

    // to measure rtt between credit request and first credit
    // for sender.
    rtt_ = now();
    slog::log5(debug_, addr(), "XPassAgent::construct_credit_request() (sndr). t_seqno_=", t_seqno_, "xpass_hdr_size_=", xpass_hdr_size_, "tcmnh->size()=", cmnh->size());

    return p;
}

Packet *XPassAgent::construct_credit_stop()
{
    slog::log6(debug_, addr(), "XPassAgent::construct_credit_stop()");
    Packet *p = allocpkt();
    if (!p)
    {
        fprintf(stderr, "ERROR: allockpkt() failed\n");
        exit(1);
    }
    hdr_tcp *tcph = hdr_tcp::access(p);
    hdr_cmn *cmnh = hdr_cmn::access(p);
    hdr_xpass *xph = hdr_xpass::access(p);

    tcph->seqno() = t_seqno_;
    tcph->ackno() = recv_next_;
    tcph->hlen() = xpass_hdr_size_;

    cmnh->size() = min_ethernet_size_;
    cmnh->ptype() = PT_XPASS_CREDIT_STOP;

    xph->credit_seq() = 0;

    return p;
}

Packet *XPassAgent::construct_credit()
{
    slog::log6(debug_, addr(), "XPassAgent::construct_credit()");
    Packet *p = allocpkt();
    if (!p)
    {
        fprintf(stderr, "ERROR: allockpkt() failed\n");
        exit(1);
    }
    hdr_tcp *tcph = hdr_tcp::access(p);
    hdr_cmn *cmnh = hdr_cmn::access(p);
    hdr_xpass *xph = hdr_xpass::access(p);
    int credit_size = min_credit_size_;

    if (min_credit_size_ < max_credit_size_)
    {
        // variable credit size
        credit_size += rand() % (max_credit_size_ - min_credit_size_ + 1);
    }
    else
    {
        // static credit size
        if (min_credit_size_ != max_credit_size_)
        {
            fprintf(stderr, "ERROR: min_credit_size_ should be less than or equal to max_credit_size_\n");
            exit(1);
        }
    }

    tcph->seqno() = t_seqno_;
    tcph->ackno() = recv_next_;
    tcph->hlen() = credit_size;

    cmnh->size() = credit_size;
    cmnh->ptype() = PT_XPASS_CREDIT;

    xph->credit_sent_time() = now();
    xph->credit_seq() = c_seqno_;

    c_seqno_ = max(1, c_seqno_ + 1);

    return p;
}

Packet *XPassAgent::construct_data(Packet *credit)
{
    slog::log6(debug_, addr(), "XPassAgent::construct_data()");
    Packet *p = allocpkt();
    if (!p)
    {
        fprintf(stderr, "ERROR: allockpkt() failed\n");
        exit(1);
    }
    hdr_tcp *tcph = hdr_tcp::access(p);
    hdr_cmn *cmnh = hdr_cmn::access(p);
    hdr_xpass *xph = hdr_xpass::access(p);
    hdr_xpass *credit_xph = hdr_xpass::access(credit);
    int datalen = (int)min(max_segment(),
                           datalen_remaining());

    if (datalen <= 0)
    {
        fprintf(stderr, "ERROR: datapacket has length of less than zero\n");
        exit(1);
    }
    tcph->seqno() = t_seqno_;
    tcph->ackno() = recv_next_;
    tcph->hlen() = xpass_hdr_size_;

    cmnh->size() = max(min_ethernet_size_, xpass_hdr_size_ + datalen);
    cmnh->ptype() = PT_XPASS_DATA;

    xph->credit_sent_time() = credit_xph->credit_sent_time();
    xph->credit_seq() = credit_xph->credit_seq();

    t_seqno_ += datalen;
    slog::log6(debug_, addr(), "XPassAgent::construct_data() (sndr). t_seqno_=", t_seqno_,
               "datalen=", datalen, "datalen_remaining()", datalen_remaining(),
               "max_segment()", max_segment());

    // Hack for xpass port
    if (cur_req_id_tup_ != nullptr)
    {
        hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(p);
        r2p2_hdr->app_level_id() = cur_req_id_tup_->app_level_id_;
        r2p2_hdr->msg_bytes() = cur_req_id_tup_->msg_bytes_;
        if (cur_req_id_tup_->is_request_)
        {
            r2p2_hdr->msg_type() = hdr_r2p2::REQUEST;
        }
        else
        {
            r2p2_hdr->msg_type() = hdr_r2p2::REPLY;
        }
        r2p2_hdr->cl_thread_id() = cur_req_id_tup_->cl_thread_id_;
        r2p2_hdr->cl_addr() = cur_req_id_tup_->cl_addr_;
        r2p2_hdr->sr_addr() = cur_req_id_tup_->sr_addr_;
        r2p2_hdr->msg_creation_time() = cur_req_id_tup_->ts_;
        r2p2_hdr->is_pfabric_app_msg() = true;
        assert(cur_req_id_tup_->ts_ > 0);
    }
    // else // can happen during warmup.
    // {
    //     throw std::runtime_error("cur_req_id_tup_ should not be null at this point");
    // }

    return p;
}

Packet *XPassAgent::construct_nack(seq_t seq_no)
{
    slog::log6(debug_, addr(), "XPassAgent::construct_nack()");
    Packet *p = allocpkt();
    if (!p)
    {
        fprintf(stderr, "ERROR: allockpkt() failed\n");
        exit(1);
    }
    hdr_tcp *tcph = hdr_tcp::access(p);
    hdr_cmn *cmnh = hdr_cmn::access(p);

    tcph->ackno() = seq_no;
    tcph->hlen() = xpass_hdr_size_;

    cmnh->size() = min_ethernet_size_;
    cmnh->ptype() = PT_XPASS_NACK;

    return p;
}

void XPassAgent::send_credit()
{
    slog::log6(debug_, addr(), "XPassAgent::send_credit()");
    double avg_credit_size = (min_credit_size_ + max_credit_size_) / 2.0;
    double delay;

    credit_feedback_control();

    // send credit.
    send(construct_credit(), 0);
    assert(cur_credit_rate_ > 0);
    // calculate delay for next credit transmission.
    delay = avg_credit_size / cur_credit_rate_;
    // add jitter
    if (max_jitter_ > min_jitter_)
    {
        double jitter = (double)rand() / (double)RAND_MAX;
        jitter = jitter * (max_jitter_ - min_jitter_) + min_jitter_;
        // jitter is in the range between min_jitter_ and max_jitter_
        delay = delay * (1 + jitter);
    }
    else if (max_jitter_ < min_jitter_)
    {
        fprintf(stderr, "ERROR: max_jitter_ should be larger than min_jitter_");
        exit(1);
    }

    send_credit_timer_.resched(delay);
}

void XPassAgent::send_credit_stop()
{
    slog::log6(debug_, addr(), "XPassAgent::send_credit_stop() - scheduling sender retr timer in (us):", (rtt_ > 0 ? (2. * rtt_) : default_credit_stop_timeout_) * 1000.0 * 1000.0);
    send(construct_credit_stop(), 0);
    // set on timer
    sender_retransmit_timer_.resched(rtt_ > 0 ? (2. * rtt_) : default_credit_stop_timeout_);
    slog::log6(debug_, addr(), "Going into XPASS_RECV_CREDIT_STOP_SENT");
    credit_recv_state_ = XPASS_RECV_CREDIT_STOP_SENT; // Later changes to XPASS_RECV_CLOSE_WAIT -> XPASS_RECV_CLOSED
}

void XPassAgent::advance_bytes(seq_t nb)
{
    slog::log6(debug_, addr(), "XPassAgent::advance_bytes(seq_t nb) - scheduling sender retr timer in (us):",
               (retransmit_timeout_) * 1000.0 * 1000.0, "credit_recv_state_:", credit_recv_state_);
    if (credit_recv_state_ != XPASS_RECV_CLOSED)
    {
        fprintf(stderr, "ERROR: tried to advance_bytes without XPASS_RECV_CLOSED\n");
        throw std::runtime_error("ERROR: tried to advance_bytes without XPASS_RECV_CLOSED");
    }
    if (nb <= 0)
    {
        fprintf(stderr, "ERROR: advanced bytes are less than or equal to zero\n");
        throw std::runtime_error("ERROR: advanced bytes are less than or equal to zero");
    }

    // advance bytes
    curseq_ += nb;

    // send credit request
    send(construct_credit_request(), 0);
    sender_retransmit_timer_.sched(retransmit_timeout_);

    // XPASS_RECV_CLOSED -> XPASS_RECV_CREDIT_REQUEST_SENT
    slog::log6(debug_, addr(), "Going into XPASS_RECV_CREDIT_REQUEST_SENT");
    credit_recv_state_ = XPASS_RECV_CREDIT_REQUEST_SENT;
}

void XPassAgent::advance_bytes(int nb, RequestIdTuple &&req_id)
{
    slog::log4(debug_, addr(), "XPassAgent::advance_bytes(int nb, RequestIdTuple&& req_id). is_request:",
               req_id.is_request_, "bytes=", nb, "addr():", addr(), "daddr():", daddr(),
               "req_id:", req_id.app_level_id_, "ts:", req_id.ts_, "times_conn_used_:", times_conn_used_);
    if (cur_req_id_tup_ == nullptr)
    {
        cur_req_id_tup_ = new RequestIdTuple();
    }
    *cur_req_id_tup_ = req_id;
    assert(cur_req_id_tup_->ts_ > 0);
    advance_bytes(nb);
}

void XPassAgent::process_ack(Packet *pkt)
{
    hdr_cmn *cmnh = hdr_cmn::access(pkt);
    hdr_tcp *tcph = hdr_tcp::access(pkt);
    int datalen = cmnh->size() - tcph->hlen();
    slog::log6(debug_, addr(), "XPassAgent::process_ack() (rcver). recv_next_=", recv_next_,
               "tcph->seqno()=", tcph->seqno(), "tcph->hlen()=", tcph->hlen(),
               "datalen:", datalen, "times_conn_used_:", times_conn_used_);
    if (datalen < 0)
    {
        fprintf(stderr, "ERROR: negative length packet has been detected.\n");
        exit(1);
    }
    if (tcph->seqno() > recv_next_)
    {
        printf("[%d] %lf: data loss detected. (expected = %ld, received = %ld)\n",
               fid_, now(), recv_next_, tcph->seqno());
        throw std::runtime_error("This should not happen in SIRD xpass port as buffers (for packets) are infinite");
        if (!wait_retransmission_)
        {
            wait_retransmission_ = true;
            send(construct_nack(recv_next_), 0);
            receiver_retransmit_timer_.resched(retransmit_timeout_);
        }
    }
    else if (tcph->seqno() == recv_next_)
    {
        if (wait_retransmission_)
        {
            wait_retransmission_ = false;
            receiver_retransmit_timer_.force_cancel();
        }
        recv_next_ += datalen;
    }
}

void XPassAgent::update_rtt(Packet *pkt)
{
    slog::log6(debug_, addr(), "XPassAgent::update_rtt()");
    hdr_xpass *xph = hdr_xpass::access(pkt);

    double rtt = now() - xph->credit_sent_time();
    if (rtt_ > 0.0)
    {
        rtt_ = 0.8 * rtt_ + 0.2 * rtt;
    }
    else
    {
        rtt_ = rtt;
    }
}

void XPassAgent::credit_feedback_control()
{
    slog::log6(debug_, addr(), "XPassAgent::credit_feedback_control()");
    if (rtt_ <= 0.0)
    {
        return;
    }
    if ((now() - last_credit_rate_update_) < rtt_)
    {
        return;
    }
    if (credit_total_ == 0)
    {
        return;
    }

    int old_rate = cur_credit_rate_;
    double loss_rate = credit_dropped_ / (double)credit_total_;
    double target_loss = (1.0 - cur_credit_rate_ / (double)max_credit_rate_) * target_loss_scaling_;
    int min_rate = (int)(avg_credit_size() / rtt_);

    if (loss_rate > target_loss)
    {
        // congestion has been detected!
        if (loss_rate >= 1.0)
        {
            cur_credit_rate_ = (int)(avg_credit_size() / rtt_);
        }
        else
        {
            cur_credit_rate_ = (int)(avg_credit_size() * (credit_total_ - credit_dropped_) / (now() - last_credit_rate_update_) * (1.0 + target_loss));
        }
        if (cur_credit_rate_ > old_rate)
        {
            cur_credit_rate_ = old_rate;
        }

        w_ = max(w_ / 2.0, min_w_);
        can_increase_w_ = false;
    }
    else
    {
        // there is no congestion.
        if (can_increase_w_)
        {
            w_ = min(w_ + 0.05, 0.5);
        }
        else
        {
            can_increase_w_ = true;
        }
        if (cur_credit_rate_ < max_credit_rate_)
        {
            cur_credit_rate_ = (int)(w_ * max_credit_rate_ + (1 - w_) * cur_credit_rate_);
        }
    }

    if (cur_credit_rate_ > max_credit_rate_)
    {
        cur_credit_rate_ = max_credit_rate_;
    }
    if (cur_credit_rate_ < min_rate)
    {
        cur_credit_rate_ = min_rate;
    }

    credit_total_ = 0;
    credit_dropped_ = 0;
    last_credit_rate_update_ = now();
}