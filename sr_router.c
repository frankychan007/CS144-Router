/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

/*
 *-----------------------------------------------------------------------------
 * Include Files
 *-----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*
 *-----------------------------------------------------------------------------
 * Private Defines
 *-----------------------------------------------------------------------------
 */

#define MIN_IP_HEADER_LENGTH  (5)
#define DEFAULT_TTL           (64)
#define SUPPORTED_IP_VERSION  (4)

/*
 *-----------------------------------------------------------------------------
 * Private Macros
 *-----------------------------------------------------------------------------
 */

#define GET_ETHERNET_PACKET_TYPE(pktPtr)  (ntohs(((sr_ethernet_hdr_t*)pktPtr)->ether_type))
#define GET_ETHERNET_DEST_ADDR(pktPtr)    (((sr_ethernet_hdr_t*)pktPtr)->ether_dhost)

#define GET_IP_HEADER_LENGTH(pktPtr)      ((((sr_ip_hdr_t*)(pktPtr))->ip_hl) * 4)

#ifdef DONT_DEFINE_UNLESS_DEBUGGING
# define LOG_MESSAGE(...) fprintf(stderr, __VA_ARGS__)
#else 
# define LOG_MESSAGE(...)
#endif

/*
 *-----------------------------------------------------------------------------
 * Private Types
 *-----------------------------------------------------------------------------
 */

/*
 *-----------------------------------------------------------------------------
 * Private variables & Constants
 *-----------------------------------------------------------------------------
 */

static uint16_t ipIdentifyNumber = 0;

static const uint8_t broadcastEthernetAddress[ETHER_ADDR_LEN] =
   { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/*
 *-----------------------------------------------------------------------------
 * Private Function Declarations
 *-----------------------------------------------------------------------------
 */

static void linkHandleReceivedArpPacket(struct sr_instance* sr, sr_arp_hdr_t* packet,
   unsigned int length, const struct sr_if* const interface);
static void linkArpAndSendPacket(struct sr_instance* sr, sr_ethernet_hdr_t* packet,
   unsigned int length, const struct sr_if* const interface);
static void networkHandleReceivedIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, const struct sr_if* const interface);
static void networkHandleIcmpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, const struct sr_if* const interface);
static void networkForwardIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet, unsigned int length,
   const struct sr_if* const receivedInterface);
static bool networkIpDesinationIsUs(struct sr_instance* sr, const sr_ip_hdr_t* const packet);
static bool networkIpSourceIsUs(struct sr_instance* sr, const sr_ip_hdr_t* const packet);
static int networkGetMaskLength(uint32_t mask);
static struct sr_rt* networkGetPacketRoute(struct sr_instance* sr, const sr_ip_hdr_t * const packet);

/*
 *-----------------------------------------------------------------------------
 * Public Function Definitions
 *-----------------------------------------------------------------------------
 */

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
   /* REQUIRES */
   assert(sr);
   
   /* Initialize cache and cache cleanup thread */
   sr_arpcache_init(&(sr->cache));
   
   pthread_attr_init(&(sr->attr));
   pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
   pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
   pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
   pthread_t thread;
   
   pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
   
   /* Add initialization code here! */
   /* Note to grader: No need! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr, uint8_t * packet/* lent */, unsigned int length,
   char* interface/* lent */)
{
   struct sr_if* receivedInterfaceEntry = NULL;
   
   /* REQUIRES */
   assert(sr);
   assert(packet);
   assert(interface);
   
   /*printf("*** -> Received packet of length %d \n", length);
   print_hdrs(packet, length);*/
   
   /* fill in code here */
   
   if (length < sizeof(sr_ethernet_hdr_t))
   {
      /* Ummm...this packet doesn't appear to be long enough to 
       * process... Drop it like it's hot! */
      return;
   }
   
   receivedInterfaceEntry = sr_get_interface(sr, interface);
   
   if ((receivedInterfaceEntry == NULL)
      || ((memcmp(GET_ETHERNET_DEST_ADDR(packet), receivedInterfaceEntry->addr, ETHER_ADDR_LEN) != 0)
         && (memcmp(GET_ETHERNET_DEST_ADDR(packet), broadcastEthernetAddress, ETHER_ADDR_LEN) != 0)))
   {
      /* Packet not sent to our Ethernet address? */
      LOG_MESSAGE("Dropping packet due to invalid Ethernet receive parameters.\n");
      return;
   }
   
   switch (ethertype(packet))
   {
      case ethertype_arp:
         /* Pass the packet to the next layer, strip the low level header. */
         linkHandleReceivedArpPacket(sr, (sr_arp_hdr_t*) (packet + sizeof(sr_ethernet_hdr_t)),
            length - sizeof(sr_ethernet_hdr_t), receivedInterfaceEntry);
         break;
         
      case ethertype_ip:
         /* Pass the packet to the next layer, strip the low level header. */
         networkHandleReceivedIpPacket(sr, (sr_ip_hdr_t*) (packet + sizeof(sr_ethernet_hdr_t)),
            length - sizeof(sr_ethernet_hdr_t), receivedInterfaceEntry);
         break;
         
      default:
         /* We have no logic to handle other packet types. Drop the packet! */
         LOG_MESSAGE("Dropping packet due to invalid Ethernet message type: 0x%X.\n", ethertype(packet));
         return;
   }

}/* end sr_handlepacket */

/**
 * LinkSendArpRequest()\n
 * IP Stack Level: Link Layer (Ethernet)\n
 * @brief Function sends an ARP request based on the provided request.
 * @param sr pointer to simple router state.
 * @param request pointer ARP request state.
 * @post does NOT update times sent in request. Must be done by caller.
 */
void LinkSendArpRequest(struct sr_instance* sr, struct sr_arpreq* request)
{
   uint8_t* arpPacket = (uint8_t *) malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t));
   sr_ethernet_hdr_t* ethernetHdr = (sr_ethernet_hdr_t*) arpPacket;
   sr_arp_hdr_t* arpHdr = (sr_arp_hdr_t*) (arpPacket + sizeof(sr_ethernet_hdr_t));
   assert(arpPacket);
   
   LOG_MESSAGE("ARPing %u.%u.%u.%u on %s\n", (request->ip >> 24) & 0xFF, 
      (request->ip >> 16) & 0xFF, (request->ip >> 8) & 0xFF, request->ip & 0xFF, 
      request->requestedInterface->name);
   
   /* Ethernet Header */
   memcpy(ethernetHdr->ether_dhost, broadcastEthernetAddress, ETHER_ADDR_LEN);
   memcpy(ethernetHdr->ether_shost, request->requestedInterface->addr, ETHER_ADDR_LEN);
   ethernetHdr->ether_type = htons(ethertype_arp);
   
   /* ARP Header */
   arpHdr->ar_hrd = htons(arp_hrd_ethernet);
   arpHdr->ar_pro = htons(ethertype_ip);
   arpHdr->ar_hln = ETHER_ADDR_LEN;
   arpHdr->ar_pln = IP_ADDR_LEN;
   arpHdr->ar_op = htons(arp_op_request);
   memcpy(arpHdr->ar_sha, request->requestedInterface->addr, ETHER_ADDR_LEN);
   arpHdr->ar_sip = request->requestedInterface->ip;
   memset(arpHdr->ar_tha, 0, ETHER_ADDR_LEN); /* Not strictly necessary by RFC 826 */
   arpHdr->ar_tip = htonl(request->ip);
   
   /* Ship it! */
   sr_send_packet(sr, arpPacket, sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t),
      request->requestedInterface->name);
   
   free(arpPacket);
}

/**
 * NetworkSendTypeThreeIcmpPacket()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function sends a type 3 (Destination Unreachable) packet.
 * @param sr pointer to simple router state struct.
 * @param icmpCode ICMP code to send (Type 3 has many to choose from).
 * @param originalPacketPtr pointer to the original received packet that caused the ICMP error.
 */
void NetworkSendTypeThreeIcmpPacket(struct sr_instance* sr, sr_icmp_code_t icmpCode,
   sr_ip_hdr_t* originalPacketPtr)
{
   struct sr_rt* icmpRoute;
   struct sr_if* destinationInterface;
   
   uint8_t* replyPacket = malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) 
      + sizeof(sr_icmp_t3_hdr_t));
   sr_ip_hdr_t* replyIpHeader = (sr_ip_hdr_t*) (replyPacket + sizeof(sr_ethernet_hdr_t));
   sr_icmp_t3_hdr_t* replyIcmpHeader = (sr_icmp_t3_hdr_t*) ((uint8_t*) replyIpHeader
      + sizeof(sr_ip_hdr_t));
   
   assert(originalPacketPtr);
   assert(sr);
   assert(replyPacket);
   
   if (networkIpSourceIsUs(sr, originalPacketPtr))
   {
      /* Well this is embarrassing. We apparently can't route a packet we 
       * wanted to originate! Some router we turned out to be, we can't even 
       * route our own packets. This is possible if an ARP request fails. */
      LOG_MESSAGE("Attempted to send Destination Unreachable ICMP packet to ourself.\n");
      free(replyPacket);
      return;
   }
   
   /* Fill in IP header */
   replyIpHeader->ip_v = SUPPORTED_IP_VERSION;
   replyIpHeader->ip_hl = MIN_IP_HEADER_LENGTH;
   replyIpHeader->ip_tos = 0;
   replyIpHeader->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
   replyIpHeader->ip_id = htons(ipIdentifyNumber); ipIdentifyNumber++;
   replyIpHeader->ip_off = htons(IP_DF);
   replyIpHeader->ip_ttl = DEFAULT_TTL;
   replyIpHeader->ip_p = ip_protocol_icmp;
   replyIpHeader->ip_sum = 0;
   replyIpHeader->ip_dst = originalPacketPtr->ip_src; /* Already in network byte order. */
   
   /* PAUSE. We need to get the destination interface. API has enough 
    * information to get it now. */
   icmpRoute = networkGetPacketRoute(sr, replyIpHeader);
   assert(icmpRoute);
   destinationInterface = sr_get_interface(sr, icmpRoute->interface);
   assert(destinationInterface);
   
   /* Okay, RESUME. */
   replyIpHeader->ip_src = destinationInterface->ip;
   replyIpHeader->ip_sum = cksum(replyIpHeader, GET_IP_HEADER_LENGTH(replyIpHeader));
   
   /* Fill in ICMP fields. */
   replyIcmpHeader->icmp_type = icmp_type_desination_unreachable;
   replyIcmpHeader->icmp_code = icmpCode;
   replyIcmpHeader->icmp_sum = 0;
   memcpy(replyIcmpHeader->data, originalPacketPtr, ICMP_DATA_SIZE);
   replyIcmpHeader->icmp_sum = cksum(replyIcmpHeader, sizeof(sr_icmp_t3_hdr_t));
   
   linkArpAndSendPacket(sr, (sr_ethernet_hdr_t*) replyPacket,
      sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t),
      destinationInterface);
   
   free(replyPacket);
}

/*
 *-----------------------------------------------------------------------------
 * Private Function Definitions
 *-----------------------------------------------------------------------------
 */

/**
 * linkHandleReceivedArpPacket()\n
 * IP Stack Level: Link Layer (Ethernet)\n
 * @brief Function handles a received ARP packet.
 * @param sr pointer to simple router state.
 * @param packet pointer to received ARP packet header.
 * @param length number of valid ARP packet bytes.
 * @param interface pointer to interface ARP packet was received.
 */
static void linkHandleReceivedArpPacket(struct sr_instance* sr, sr_arp_hdr_t * packet,
   unsigned int length, const struct sr_if* const interface)
{
   if (length < sizeof(sr_arp_hdr_t))
   {
      /* Not big enough to be an ARP packet... */
      return;
   }
   
   if ((ntohs(packet->ar_pro) != ethertype_ip)
      || (ntohs(packet->ar_hrd) != arp_hrd_ethernet)
      || (packet->ar_pln != IP_ADDR_LEN) 
      || (packet->ar_hln != ETHER_ADDR_LEN))
   {
      /* Received unsupported packet argument */
      LOG_MESSAGE("ARP packet received with invalid parameters. Dropping.\n");
      return;
   }
   
   switch (ntohs(packet->ar_op))
   {
      case arp_op_request:
      {
         if (packet->ar_tip == interface->ip)
         {
            /* We're being ARPed! Prepare the reply! */
            uint8_t* replyPacket = (uint8_t *) malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t));
            sr_ethernet_hdr_t* ethernetHdr = (sr_ethernet_hdr_t*)replyPacket;
            sr_arp_hdr_t* arpHdr = (sr_arp_hdr_t*)(replyPacket + sizeof(sr_ethernet_hdr_t));
            
            LOG_MESSAGE("Received ARP request. Sending ARP reply.\n");
            
            /* Ethernet Header */
            memcpy(ethernetHdr->ether_dhost, packet->ar_sha, ETHER_ADDR_LEN);
            memcpy(ethernetHdr->ether_shost, interface->addr, ETHER_ADDR_LEN);
            ethernetHdr->ether_type = htons(ethertype_arp);
            
            /* ARP Header */
            arpHdr->ar_hrd = htons(arp_hrd_ethernet);
            arpHdr->ar_pro = htons(ethertype_ip);
            arpHdr->ar_hln = ETHER_ADDR_LEN;
            arpHdr->ar_pln = IP_ADDR_LEN;
            arpHdr->ar_op = htons(arp_op_reply);
            memcpy(arpHdr->ar_sha, interface->addr, ETHER_ADDR_LEN);
            arpHdr->ar_sip = interface->ip;
            memcpy(arpHdr->ar_tha, packet->ar_sha, ETHER_ADDR_LEN);
            arpHdr->ar_tip = packet->ar_sip;
            
            sr_send_packet(sr, replyPacket, sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t),
               interface->name);
            
            free(replyPacket);
         }
         break;
      }
      
      case arp_op_reply:
      {
         /* Note: Due to the point-to-point nature of ARP, we don't have to 
          * check all our interface IP addresses, only the one that the ARP 
          * packet was received on. */
         if (packet->ar_tip == interface->ip)
         {
            struct sr_arpreq* requestPointer = sr_arpcache_insert(
               &sr->cache, packet->ar_sha, ntohl(packet->ar_sip));
            
            if (requestPointer != NULL)
            {
               LOG_MESSAGE("Received ARP reply, sending all queued packets.\n");
               
               while (requestPointer->packets != NULL)
               {
                  struct sr_packet* curr = requestPointer->packets;
                  
                  /* Copy in the newly discovered Ethernet address of the frame */
                  memcpy(((sr_ethernet_hdr_t*) curr->buf)->ether_dhost,
                     packet->ar_sha, ETHER_ADDR_LEN);
                  
                  /* The last piece of the pie is now complete. Ship it. */
                  sr_send_packet(sr, curr->buf, curr->len, curr->iface);
                  
                  /* Forward list of packets. */
                  requestPointer->packets = requestPointer->packets->next;
                  
                  /* Free all memory associated with this packet (allocated on queue). */
                  free(curr->buf);
                  free(curr->iface);
                  free(curr);
               }
               
               /* Bye bye ARP request. */
               sr_arpreq_destroy(&sr->cache, requestPointer);
            }
            else
            {
               /* Queued response to one of our ARP request retries? */
               LOG_MESSAGE("Received ARP reply, but found no request.\n");
            }
         }
         break;
      }
      
      default:
      {
         /* Unrecognized ARP type */
         LOG_MESSAGE("Received packet with invalid ARP type: 0x%X.\n", ntohs(packet->ar_op));
         break;
      }
   }
}

/**
 * networkHandleReceivedIpPacket()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function handles a received IPv4 packet.
 * @param sr pointer to simple router state.
 * @param packet pointer to received IP packet header.
 * @param length number of valid IP packet header + payload bytes.
 * @param interface pointer to interface IP packet was received.
 */
static void networkHandleReceivedIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, const struct sr_if* const interface)
{
   if (length < sizeof(sr_ip_hdr_t))
   {
      /* Not big enough to be an IP packet... */
      LOG_MESSAGE("Received IP packet with invalid length. Dropping.\n");
      return;
   }
   
   /* Verify checksum before parsing packet. */
   /* We have two options here.
    * 1) always assume the packet header is 20 bytes long, precluding us 
    *    from receiving packets with option bytes set.
    * 2) We take the length field as gospel (i.e. there isn't an error in 
    *    this nibble) and go with it. 
    * I will choose the latter, but protect against headers less than 20 
    * bytes. If it was wrong, theoretically the checksum should fail since 
    * I will be taking it over more or less bytes than was intended.
    */
   if (packet->ip_hl >= MIN_IP_HEADER_LENGTH)
   {
      uint16_t headerChecksum = packet->ip_sum;
      uint16_t calculatedChecksum = 0;
      packet->ip_sum = 0;
      
      calculatedChecksum = cksum(packet, GET_IP_HEADER_LENGTH(packet));
      
      if (headerChecksum != calculatedChecksum)
      {
         /* Bad checksum... */
         LOG_MESSAGE("IP checksum failed. Dropping received packet.\n");
         return;
      }
      else
      {
         /* Put it back. This is so if we send an ICMP message which contains 
          * this packet's header, it can be as we received it. */
         packet->ip_sum = headerChecksum;
      }
   }
   else
   {
      /* Something is way wrong with this packet. Throw it out. */
      LOG_MESSAGE("Received IP packet with invalid length in header. Dropping.\n");
      return;
   }
   
   if (packet->ip_v != SUPPORTED_IP_VERSION)
   {
      /* What do you think we are? Some fancy, IPv6 router? Guess again! 
       * Process IPv4 packets only.*/
      LOG_MESSAGE("Received non-IPv4 packet. Dropping.\n");
      return;
   }
   
   if (networkIpDesinationIsUs(sr, packet))
   {
      /* Somebody must like me, because they're sending packets to my 
       * address! */
      if (packet->ip_p == (uint8_t) ip_protocol_icmp)
      {
         networkHandleIcmpPacket(sr, packet, length, interface);
      }
      else
      {
         /* I don't process anything else! Send port unreachable. */
         LOG_MESSAGE("Received Non-ICMP packet destined for me. Sending ICMP port unreachable.\n");
         NetworkSendTypeThreeIcmpPacket(sr, icmp_code_destination_port_unreachable, packet);
      }
   }
   else
   {
      /* Decrement TTL and forward. */
      packet->ip_ttl -= 1;
      if (packet->ip_ttl == 0)
      {
         /* Uh oh... someone's just about run out of time. */
         uint8_t* replyPacket = malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) 
            + sizeof(sr_icmp_t3_hdr_t));
         sr_ip_hdr_t* replyIpHeader = (sr_ip_hdr_t*) (replyPacket + sizeof(sr_ethernet_hdr_t));
         sr_icmp_t3_hdr_t* replyIcmpHeader = (sr_icmp_t3_hdr_t*) ((uint8_t*) replyIpHeader
            + sizeof(sr_ip_hdr_t));
         
         LOG_MESSAGE("TTL expired on received packet. Sending an ICMP time exceeded.\n");
         
         /* To maintain integrity of the sent packet, we should probably send 
          * the packet back as we received it. */
         packet->ip_ttl = 1;
         
         /* Fill in IP header */
         replyIpHeader->ip_v = SUPPORTED_IP_VERSION;
         replyIpHeader->ip_hl = MIN_IP_HEADER_LENGTH;
         replyIpHeader->ip_tos = 0;
         replyIpHeader->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
         replyIpHeader->ip_id = htons(ipIdentifyNumber); ipIdentifyNumber++;
         replyIpHeader->ip_off = htons(IP_DF);
         replyIpHeader->ip_ttl = DEFAULT_TTL;
         replyIpHeader->ip_p = ip_protocol_icmp;
         replyIpHeader->ip_sum = 0;
         replyIpHeader->ip_src = interface->ip;
         replyIpHeader->ip_dst = packet->ip_src; /* Already in network byte order. */
         replyIpHeader->ip_sum = cksum(replyIpHeader, GET_IP_HEADER_LENGTH(replyIpHeader));
         
         /* Fill in ICMP fields. */
         replyIcmpHeader->icmp_type = icmp_type_time_exceeded;
         replyIcmpHeader->icmp_code = 0;
         replyIcmpHeader->icmp_sum = 0;
         memcpy(replyIcmpHeader->data, packet, ICMP_DATA_SIZE);
         replyIcmpHeader->icmp_sum = cksum(replyIcmpHeader, sizeof(sr_icmp_t3_hdr_t));
         
         linkArpAndSendPacket(sr, (sr_ethernet_hdr_t*) replyPacket,
            sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t), interface);
         
         free(replyPacket);
      }
      else
      {
         /* Recalculate checksum since we altered the packet header. */
         packet->ip_sum = 0;
         packet->ip_sum = cksum(packet, GET_IP_HEADER_LENGTH(packet));
         
         networkForwardIpPacket(sr, packet, length, interface);
      }
   }
}

/**
 * networkHandleIcmpPacket()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function handles a received ICMP packet.
 * @param sr pointer to simple router state structure.
 * @param packet pointer to IP header of packet containing ICMP packet.
 * @param length length in bytes of packet IP header and payload.
 * @param interface pointer to router interface that packet was recieved.
 */
static void networkHandleIcmpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, const struct sr_if* const interface)
{
   sr_icmp_hdr_t* icmpHeader = (sr_icmp_hdr_t*) (((uint8_t*) packet) + GET_IP_HEADER_LENGTH(packet));
   int icmpLength = length - GET_IP_HEADER_LENGTH(packet);
   
   /* Check the integrity of the ICMP packet */
   {
      uint16_t headerChecksum = icmpHeader->icmp_sum;
      uint16_t calculatedChecksum = 0;
      icmpHeader->icmp_sum = 0;
      
      calculatedChecksum = cksum(icmpHeader, icmpLength);
      
      if (headerChecksum != calculatedChecksum)
      {
         /* Bad checksum... */
         LOG_MESSAGE("ICMP checksum failed. Dropping received packet.\n");
         return;
      }
   }
   
   if (icmpHeader->icmp_type == icmp_type_echo_request)
   {
      /* Send an echo Reply! */
      uint8_t* replyPacket = malloc(icmpLength + sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t));
      sr_ip_hdr_t* replyIpHeader = (sr_ip_hdr_t*) (replyPacket + sizeof(sr_ethernet_hdr_t));
      sr_icmp_hdr_t* replyIcmpHeader = (sr_icmp_hdr_t*) ((uint8_t*) replyIpHeader
         + sizeof(sr_ip_hdr_t));
      assert(replyPacket);
      
      LOG_MESSAGE("Received ICMP echo request packet. Sending ICMP echo reply.\n");
      
      /* Fill in IP Header fields. */
      replyIpHeader->ip_v = SUPPORTED_IP_VERSION;
      replyIpHeader->ip_hl = MIN_IP_HEADER_LENGTH;
      replyIpHeader->ip_tos = 0;
      replyIpHeader->ip_len = htons((uint16_t) length);
      replyIpHeader->ip_id = htons(ipIdentifyNumber); ipIdentifyNumber++;
      replyIpHeader->ip_off = htons(IP_DF);
      replyIpHeader->ip_ttl = DEFAULT_TTL;
      replyIpHeader->ip_p = ip_protocol_icmp;
      replyIpHeader->ip_sum = 0;
      replyIpHeader->ip_src = packet->ip_dst; /* Already in network byte order. */
      replyIpHeader->ip_dst = packet->ip_src; /* Already in network byte order. */
      replyIpHeader->ip_sum = cksum(replyIpHeader, GET_IP_HEADER_LENGTH(replyIpHeader));
      
      /* Fill in ICMP fields. */
      replyIcmpHeader->icmp_type = icmp_type_echo_reply;
      replyIcmpHeader->icmp_code = 0;
      replyIcmpHeader->icmp_sum = 0;
      
      /* Copy the old payload into the new one... */
      memcpy(((uint8_t*) replyIcmpHeader) + sizeof(sr_icmp_hdr_t),
         ((uint8_t*) icmpHeader) + sizeof(sr_icmp_hdr_t), icmpLength - sizeof(sr_icmp_hdr_t));
      
      /* ...then update the final checksum for the ICMP payload. */
      replyIcmpHeader->icmp_sum = cksum(replyIcmpHeader, icmpLength);
      
      /* Reply payload built. Ship it! */
      linkArpAndSendPacket(sr, (sr_ethernet_hdr_t*) replyPacket, length + sizeof(sr_ethernet_hdr_t),
         interface);
      
      free(replyPacket);
   }
   else
   {
      /* I don't send any non-ICMP packets...How did I receive another ICMP type? */
      LOG_MESSAGE("Received unexpected ICMP message. Type: %u, Code: %u\n", 
         icmpHeader->icmp_type, icmpHeader->icmp_code);
   }
}

/**
 * networkForwardIpPacket()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function forwards (i.e routes) a provided received packet pointer.
 * @param sr pointer to simple router structure
 * @param packet pointer to received packet in need of routing
 * @param length number of valid payload and IP header of packet.
 * @param receivedInterface pointer to the interface the packet was originally received.
 */
static void networkForwardIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, const struct sr_if* const receivedInterface)
{
   /* Get the route we should take for the provided packet. */
   struct sr_rt* forwardRoute = networkGetPacketRoute(sr, packet);
   
   /* Check to make sure we made a viable routing decision. If we made the 
    * decision to forward onto the interface we received the packet or 
    * couldn't make a decision, something is wrong. Send a host 
    * unreachable if this is the case. */
   if ((forwardRoute != NULL) && (strcmp(forwardRoute->interface, receivedInterface->name) != 0))
   {
      /* We found a viable route. Forward to it! */
      struct sr_if* forwardInterface = sr_get_interface(sr, forwardRoute->interface);
      uint8_t* forwardPacket = malloc(length + sizeof(sr_ethernet_hdr_t));
      memcpy(forwardPacket + sizeof(sr_ethernet_hdr_t), packet, length);
      
      LOG_MESSAGE("Forwarding from interface %s to %s\n", receivedInterface->name, 
         forwardInterface->name);
   
      linkArpAndSendPacket(sr, (sr_ethernet_hdr_t*)forwardPacket,
         length + sizeof(sr_ethernet_hdr_t), forwardInterface);
      
      free(forwardPacket);
   }
   else
   {
      /* Routing table told us to route this packet back the way it came. 
       * That's probably wrong, so we assume the host is actually 
       * unreachable. */
      LOG_MESSAGE("Routing decision could not be made. Sending ICMP Host unreachable.\n");
      NetworkSendTypeThreeIcmpPacket(sr, icmp_code_destination_host_unreachable, packet);
   }
}

/**
 * networkGetPacketRoute()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function gets the longest prefix match route for a provided IP packet.
 * @param sr pointer to simple router structure.
 * @param packet pointer to packet in need of routing.
 * @return pointer to the routing table entry where we should route the packet.
 */
static struct sr_rt* networkGetPacketRoute(struct sr_instance* sr, const sr_ip_hdr_t * const packet)
{
   struct sr_rt* routeIter;
   int networkMaskLength = -1;
   struct sr_rt* ret = NULL;
   
   for (routeIter = sr->routing_table; routeIter; routeIter = routeIter->next)
   {
      /* Assure the route we are about to check has a longer mask then the 
       * last one we chose.  This is so we can find the longest prefix match. */
      if (networkGetMaskLength(routeIter->mask.s_addr) > networkMaskLength)
      {
         /* Mask is longer, now see if the destination matches. */
         if ((ntohl(packet->ip_dst) & routeIter->mask.s_addr) 
            == (ntohl(routeIter->dest.s_addr) & routeIter->mask.s_addr))
         {
            /* Longer prefix match found. */
            ret = routeIter;
            networkMaskLength = networkGetMaskLength(routeIter->mask.s_addr);
         }
      }
   }
   
   return ret;
}

/**
 * linkArpAndSendPacket()\n
 * IP Stack Level: Link Layer (Ethernet)\n
 * Description:\n
 *    Function provided the link layer functionality required to send a 
 *    provided packet.  If there is an ARP cache entry for the destination, 
 *    the packet is sent immediately.  Otherwise the function will send an 
 *    ARP request along the provided interface.
 * @brief Function populates Ethernet header of a provided packet and sends it on the provided interface.
 * @param sr pointer to simple router state.
 * @param packet pointer to packet to send.
 * @param length size of the packet.
 * @param interface pointer to interface to send packet on.
 * @warning Function is for IP datagrams only. ARP packets should not go through this function.
 */
static void linkArpAndSendPacket(struct sr_instance* sr, sr_ethernet_hdr_t* packet, 
   unsigned int length, const struct sr_if* const interface)
{
   /* Need the gateway IP to do the ARP cache lookup. */
   uint32_t nextHopIpAddress = ntohl(sr_get_rt(sr, interface->name)->gw.s_addr);
   struct sr_arpentry* arpEntry = sr_arpcache_lookup(&sr->cache, nextHopIpAddress);
   
   /* This function is only for IP packets, fill in the type */
   packet->ether_type = htons(ethertype_ip);
   memcpy(packet->ether_shost, interface->addr, ETHER_ADDR_LEN);
   
   if (arpEntry != NULL)
   {
      memcpy(packet->ether_dhost, arpEntry->mac, ETHER_ADDR_LEN);
      sr_send_packet(sr, (uint8_t*) packet, length, interface->name);
   }
   else
   {
      /* We need to ARP our next hop. Setup the request and send the ARP packet. */
      struct sr_arpreq* arpRequestPtr = sr_arpcache_queuereq(&sr->cache, nextHopIpAddress,
         (uint8_t*) packet, length, interface->name);
      
      if (arpRequestPtr->times_sent == 0)
      {
         /* New request. Send the first ARP NOW! */
         arpRequestPtr->requestedInterface = interface;
         
         LinkSendArpRequest(sr, arpRequestPtr);
         
         arpRequestPtr->times_sent = 1;
         arpRequestPtr->sent = time(NULL);
      }
   }
}

/**
 * networkIpDesinationIsUs()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function checks if ANY of our IP addresses matches the packet's destination IP.
 * @param sr pointer to simple router state.
 * @param packet pointer to received packet.
 * @return true if we were the destination of this packet. false otherwise.
 */
static bool networkIpDesinationIsUs(struct sr_instance* sr,
   const sr_ip_hdr_t* const packet)
{
   struct sr_if* interfaceIterator;
   
   for (interfaceIterator = sr->if_list; interfaceIterator != NULL; interfaceIterator =
      interfaceIterator->next)
   {
      if (packet->ip_dst == interfaceIterator->ip)
      {
         return true;
      }
   }
   
   return false;
}

/**
 * networkIpSourceIsUs()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function checks if ANY of our IP addresses matches the packet's source IP.
 * @param sr pointer to simple router state.
 * @param packet pointer to packet.
 * @return true if we were the source of this packet. false otherwise.
 */
static bool networkIpSourceIsUs(struct sr_instance* sr, const sr_ip_hdr_t* const packet)
{
   struct sr_if* interfaceIterator;
   
   for (interfaceIterator = sr->if_list; interfaceIterator != NULL; interfaceIterator =
      interfaceIterator->next)
   {
      if (packet->ip_src == interfaceIterator->ip)
      {
         return true;
      }
   }
   
   return false;
}

/**
 * networkGetMaskLength()\n
 * IP Stack Level: Network (IP)\n
 * @brief Function gets the length of a provided IPv4 subnet mask.
 * @param mask IPv4 subnet mask.
 * @return the number of bits set in the mask starting from the most significant.
 */
static int networkGetMaskLength(uint32_t mask)
{
   int ret = 0;
   uint32_t bitScanner = 0x80000000;
   
   while ((bitScanner != 0) && ((bitScanner & mask) != 0))
   {
      bitScanner >>= 1;
      ret++;
   }
   
   return ret;
}
