//
// Created by consti10 on 21.12.20.
//

#ifndef WIFIBROADCAST_RAWRECEIVER_H
#define WIFIBROADCAST_RAWRECEIVER_H

#include <functional>
#include <unordered_map>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "Helper.hpp"
#include "HelperSources/TimeHelper.hpp"
#include "wifibroadcast.hpp"

// This is a single header-only file you can use to build your own wifibroadcast link

// stuff that helps for receiving data with pcap
namespace RawReceiverHelper{
    // call before pcap_activate
    static void iteratePcapTimestamps(pcap_t* ppcap){
        int* availableTimestamps;
        const int nTypes=pcap_list_tstamp_types(ppcap,&availableTimestamps);
        std::cout<<"N available timestamp types "<<nTypes<<"\n";
        for(int i=0;i<nTypes;i++){
            const char* name=pcap_tstamp_type_val_to_name(availableTimestamps[i]);
            const char* description=pcap_tstamp_type_val_to_description(availableTimestamps[i]);
            std::cout<<"Name: "<<std::string(name)<<" Description: "<<std::string(description)<<"\n";
            if(availableTimestamps[i]==PCAP_TSTAMP_HOST){
                std::cout<<"Setting timestamp to host\n";
                pcap_set_tstamp_type(ppcap,PCAP_TSTAMP_HOST);
            }
        }
        pcap_free_tstamp_types(availableTimestamps);
    }
    // copy paste from svpcom
    // I think this one opens the rx interface with pcap and then sets a filter such that only packets pass through for the selected radio port
    static pcap_t* openRxWithPcap(const std::string& wlan,const int radio_port){
        pcap_t* ppcap;
        char errbuf[PCAP_ERRBUF_SIZE];
        ppcap = pcap_create(wlan.c_str(), errbuf);
        if (ppcap == NULL) {
            throw std::runtime_error(StringFormat::convert("Unable to open interface %s in pcap: %s", wlan.c_str(), errbuf));
        }
        iteratePcapTimestamps(ppcap);
        if (pcap_set_snaplen(ppcap, 4096) != 0) throw std::runtime_error("set_snaplen failed");
        if (pcap_set_promisc(ppcap, 1) != 0) throw std::runtime_error("set_promisc failed");
        //if (pcap_set_rfmon(ppcap, 1) !=0) throw runtime_error("set_rfmon failed");
        if (pcap_set_timeout(ppcap, -1) != 0) throw std::runtime_error("set_timeout failed");
        //if (pcap_set_buffer_size(ppcap, 2048) !=0) throw runtime_error("set_buffer_size failed");
        // Important: Without enabling this mode pcap buffers quite a lot of packets starting with version 1.5.0 !
        // https://www.tcpdump.org/manpages/pcap_set_immediate_mode.3pcap.html
        if(pcap_set_immediate_mode(ppcap,true)!=0)throw std::runtime_error(StringFormat::convert("pcap_set_immediate_mode failed: %s", pcap_geterr(ppcap)));
        if (pcap_activate(ppcap) != 0) throw std::runtime_error(StringFormat::convert("pcap_activate failed: %s", pcap_geterr(ppcap)));
        if (pcap_setnonblock(ppcap, 1, errbuf) != 0) throw std::runtime_error(StringFormat::convert("set_nonblock failed: %s", errbuf));

        int link_encap = pcap_datalink(ppcap);
        struct bpf_program bpfprogram{};
        std::string program;
        switch (link_encap) {
            case DLT_PRISM_HEADER:
                std::cout<<wlan<<" has DLT_PRISM_HEADER Encap\n";
                program = StringFormat::convert("radio[0x4a:4]==0x13223344 && radio[0x4e:2] == 0x55%.2x", radio_port);
                break;

            case DLT_IEEE802_11_RADIO:
                std::cout<<wlan<<" has DLT_IEEE802_11_RADIO Encap\n";
                program = StringFormat::convert("ether[0x0a:4]==0x13223344 && ether[0x0e:2] == 0x55%.2x", radio_port);
                break;
            default:
                throw std::runtime_error(StringFormat::convert("unknown encapsulation on %s", wlan.c_str()));
        }
        if (pcap_compile(ppcap, &bpfprogram, program.c_str(), 1, 0) == -1) {
            throw std::runtime_error(StringFormat::convert("Unable to compile %s: %s", program.c_str(), pcap_geterr(ppcap)));
        }
        if (pcap_setfilter(ppcap, &bpfprogram) == -1) {
            throw std::runtime_error(StringFormat::convert("Unable to set filter %s: %s", program.c_str(), pcap_geterr(ppcap)));
        }
        pcap_freecode(&bpfprogram);
        return ppcap;
    }
}

// This class listens for WIFI data on the specified wlan for wifi packets with the right RADIO_PORT
// Processing of data is done by the Aggregator
// It uses a slightly different pattern than at the transmitter:
// loop_iter loops over all packets for this wifi card that are not processed yet, then returns.
// Use the fd to check if data is available for this wifi card
class PcapReceiver {
public:
    // this callback is called with the received packet from pcap
    typedef std::function<void(const uint8_t wlan_idx,const pcap_pkthdr& hdr,const uint8_t* pkt)> PROCESS_PACKET_CALLBACK;
    PcapReceiver(const std::string& wlan, int wlan_idx, int radio_port,PROCESS_PACKET_CALLBACK callback): WLAN_IDX(wlan_idx),RADIO_PORT(radio_port), mCallback(callback){
        ppcap=RawReceiverHelper::openRxWithPcap(wlan, RADIO_PORT);
        fd = pcap_get_selectable_fd(ppcap);
    }

    ~PcapReceiver(){
        close(fd);
        pcap_close(ppcap);
    }
    // loop receiving data from this interface until no more data is available
    void loop_iter() {
        // loop while incoming queue is not empty
        int nPacketsPolledUntilQueueWasEmpty=0;
        for (;;){
            struct pcap_pkthdr hdr{};
            const uint8_t *pkt = pcap_next(ppcap, &hdr);
            if (pkt == nullptr) {
#ifdef ENABLE_ADVANCED_DEBUGGING
                nOfPacketsPolledFromPcapQueuePerIteration.add(nPacketsPolledUntilQueueWasEmpty);
                std::cout<<"nOfPacketsPolledFromPcapQueuePerIteration: "<<nOfPacketsPolledFromPcapQueuePerIteration.getAvgReadable()<<"\n";
                nOfPacketsPolledFromPcapQueuePerIteration.reset();
#endif
                break;
            }
            timeForParsingPackets.start();
            mCallback(WLAN_IDX,hdr,pkt);
            timeForParsingPackets.stop();
#ifdef ENABLE_ADVANCED_DEBUGGING
            // how long the cpu spends on agg.processPacket
        timeForParsingPackets.printInIntervalls(std::chrono::seconds(1));
#endif
            nPacketsPolledUntilQueueWasEmpty++;
        }
    }

    int getfd() const { return fd; }

public:
    // the wifi interface this receiver listens on (not the radio port)
    const int WLAN_IDX;
    // the radio port it filters pacp packets for
    const int RADIO_PORT;
public:
    // this callback is called with valid data when doing loop_iter()
    const PROCESS_PACKET_CALLBACK mCallback;
    // this fd is created by pcap
    int fd;
    pcap_t *ppcap;
    // measures the cpu time spent on the callback
    Chronometer timeForParsingPackets{"PP"};
    // If each iteration pulls too many packets out your CPU is most likely too slow
    BaseAvgCalculator<int> nOfPacketsPolledFromPcapQueuePerIteration;
};

#endif //WIFIBROADCAST_RAWRECEIVER_H
