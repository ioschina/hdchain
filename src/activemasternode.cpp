// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "masternode.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "protocol.h"
#include "masternodeconfig.h"



// Keep track of the active Masternode
CActiveMasternode activeMasternode;

void CActiveMasternode::ManageState()
{
    LogPrint("masternode", "CActiveMasternode::ManageState -- Start\n");
    if(!fMasterNode) {
        LogPrint("masternode", "CActiveMasternode::ManageState -- Not a masternode, returning\n");
        return;
    }

    if(Params().NetworkIDString() != CBaseChainParams::REGTEST && !masternodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_MASTERNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveMasternode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if(nState == ACTIVE_MASTERNODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_MASTERNODE_INITIAL;
    }

    LogPrint("masternode", "CActiveMasternode::ManageState -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    if(eType == MASTERNODE_UNKNOWN) {
        ManageStateInitial();
    }

    if(eType == MASTERNODE_REMOTE) {
        ManageStateRemote();
    } else if(eType == MASTERNODE_LOCAL) {
        // Try Remote Start first so the started local masternode can be restarted without recreate masternode broadcast.
        ManageStateRemote();
#ifdef ENABLE_WALLET
        if(nState != ACTIVE_MASTERNODE_STARTED)
            ManageStateLocal();
#endif // ENABLE_WALLET
    }

    SendMasternodePing();
}

std::string CActiveMasternode::GetStateString() const
{
    switch (nState) {
        case ACTIVE_MASTERNODE_INITIAL:         return "INITIAL";
        case ACTIVE_MASTERNODE_SYNC_IN_PROCESS: return "SYNC_IN_PROCESS";
        case ACTIVE_MASTERNODE_INPUT_TOO_NEW:   return "INPUT_TOO_NEW";
        case ACTIVE_MASTERNODE_NOT_CAPABLE:     return "NOT_CAPABLE";
        case ACTIVE_MASTERNODE_STARTED:         return "STARTED";
        default:                                return "UNKNOWN";
    }
}

std::string CActiveMasternode::GetStatus() const
{
    switch (nState) {
        case ACTIVE_MASTERNODE_INITIAL:         return "Node just started, not yet activated";
        case ACTIVE_MASTERNODE_SYNC_IN_PROCESS: return "Sync in progress. Must wait until sync is complete to start Masternode";
        case ACTIVE_MASTERNODE_INPUT_TOO_NEW:   return strprintf("Masternode input must have at least %d confirmations", Params().GetConsensus().nMasternodeMinimumConfirmations);
        case ACTIVE_MASTERNODE_NOT_CAPABLE:     return "Not capable masternode: " + strNotCapableReason;
        case ACTIVE_MASTERNODE_STARTED:         return "Masternode successfully started";
        default:                                return "Unknown";
    }
}

std::string CActiveMasternode::GetTypeString() const
{
    std::string strType;
    switch(eType) {
    case MASTERNODE_UNKNOWN:
        strType = "UNKNOWN";
        break;
    case MASTERNODE_REMOTE:
        strType = "REMOTE";
        break;
    case MASTERNODE_LOCAL:
        strType = "LOCAL";
        break;
    default:
        strType = "UNKNOWN";
        break;
    }
    return strType;
}

bool CActiveMasternode::SendMasternodePing()
{
    if(!fPingerEnabled) {
        LogPrint("masternode", "CActiveMasternode::SendMasternodePing -- %s: masternode ping service is disabled, skipping...\n", GetStateString());
        return false;
    }

    if(!mnodeman.Has(vin)) {
        if(nState != ACTIVE_MASTERNODE_NOT_CAPABLE) {
            strNotCapableReason = "Masternode not in masternode list";
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
        }
        LogPrintf("CActiveMasternode::SendMasternodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CMasternodePing mnp(vin);
    if(!mnp.Sign(keyMasternode, pubKeyMasternode)) {
        LogPrintf("CActiveMasternode::SendMasternodePing -- ERROR: Couldn't sign Masternode Ping\n");
        return false;
    }

    // Update lastPing for our masternode in Masternode list
    if(mnodeman.IsMasternodePingedWithin(vin, MASTERNODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveMasternode::SendMasternodePing -- Too early to send Masternode Ping\n");
        return false;
    }

    mnodeman.SetMasternodeLastPing(vin, mnp);

    LogPrintf("CActiveMasternode::SendMasternodePing -- Relaying ping(%ld-%d), collateral=%s\n", mnp.certifyPeriod, mnp.certifyVersion, vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveMasternode::ManageStateInitial()
{
    LogPrint("masternode", "CActiveMasternode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
        strNotCapableReason = "Masternode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveMasternode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CMasternode::IsValidNetAddr(service);
        if(!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
				nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveMasternode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode* pnode, vNodes) {
                LogPrintf("fSuccessfullyConnected = %c, addr.IsIPv4 = %c\n", pnode->fSuccessfullyConnected ? 'y' : 'n', pnode->addr.IsIPv4() ? 'y' : 'n' );            
			    if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CMasternode::IsValidNetAddr(service);
                    LogPrintf("GetLocal() = %c, IsValidNetAddr = %c \n", GetLocal(service, &pnode->addr), CMasternode::IsValidNetAddr(service));
                    if(fFoundLocal) break;
                }
            }
        }
    }

    if(!fFoundLocal) {
        nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveMasternode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(), mainnetDefaultPort);
            LogPrintf("CActiveMasternode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if(service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(), mainnetDefaultPort);
        LogPrintf("CActiveMasternode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveMasternode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());

    if(!ConnectNode((CAddress)service, NULL, true)) {
        nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveMasternode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = MASTERNODE_REMOTE;

	if(masternodeConfig.GetMasternodeVin(vin))
	{
		eType = MASTERNODE_LOCAL;
	}

    LogPrint("masternode", "CActiveMasternode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveMasternode::ManageStateRemote()
{
    LogPrint("masternode", "CActiveMasternode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyMasternode.GetID() = %s\n", 
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeyMasternode.GetID().ToString());

    mnodeman.CheckMasternode(pubKeyMasternode);
    masternode_info_t infoMn = mnodeman.GetMasternodeInfo(pubKeyMasternode);
    if(infoMn.fInfoValid) {
        if(infoMn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveMasternode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(service != infoMn.addr) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = "Specified IP doesn't match our external address.";
            LogPrintf("CActiveMasternode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(vin != infoMn.vin) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = "Specified collateraloutputtxid doesn't match our external vin.";
            LogPrintf("CActiveMasternode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(!CMasternode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Masternode in %s state", CMasternode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveMasternode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(nState != ACTIVE_MASTERNODE_STARTED) {
            LogPrintf("CActiveMasternode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_MASTERNODE_STARTED;
        }
    }
    else {
        nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
        strNotCapableReason = "Masternode not in masternode list";
        LogPrintf("CActiveMasternode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}


void CActiveMasternode::ManageStateLocal()
{
    LogPrint("masternode", "CActiveMasternode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
    if(nState == ACTIVE_MASTERNODE_STARTED) {
        return;
    }

    // Choose coins to use

    if(masternodeConfig.GetMasternodeVin(vin)) {
        int nInputAge = GetInputAge(vin);
        if(nInputAge < Params().GetConsensus().nMasternodeMinimumConfirmations){
            nState = ACTIVE_MASTERNODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveMasternode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        CMasternodeBroadcast mnb;
        std::string strError;
        if(!CMasternodeBroadcast::Create(vin, service,  keyMasternode, pubKeyMasternode, strError, mnb)) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveMasternode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        if(!CBitcoinAddress().Set(mnb.GetPayeeDestination())) {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = "Error Collateral transaction without change address, can't design a payee address!";
            LogPrintf("CActiveMasternode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        // check if it is registered on the HDCH center server
        //CMasternode mn(mnb);
        if(!mnodecenter.LoadLicense(mnb))
        {
            nState = ACTIVE_MASTERNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf(_("%s didn't registered on HDCH Center"), mnb.vin.prevout.ToStringShort());
            LogPrintf("CActiveMasternode::ManageStateLocal -- Didn't registered on HDCH Center, masternode=%s\n", mnb.vin.prevout.ToStringShort());
            return;
        }
        //mnb.certifyPeriod = mn.certifyPeriod;
        //mnb.certificate = mn.certificate;
        LogPrintf("CActiveMasternode::ManageStateLocal -- Load License(%ld-%d)\n", mnb.certifyPeriod, mnb.certifyVersion);
		
        fPingerEnabled = true;
        nState = ACTIVE_MASTERNODE_STARTED;

        //update to masternode list
        LogPrintf("CActiveMasternode::ManageStateLocal -- Update Masternode List\n");
        mnodeman.UpdateMasternodeList(mnb);
        mnodeman.NotifyMasternodeUpdates();

        //send to all peers
        LogPrintf("CActiveMasternode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.Relay();
    }
}
