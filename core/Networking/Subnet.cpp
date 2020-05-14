/*
 * Copyright (c) 2012-2013 Open Source Community - <http://www.peerfact.org>
 * Copyright (c) 2011-2012 University of Paderborn - UPB
 * Copyright (c) 2005-2011 KOM - Multimedia Communications Lab
 *
 * This file is part of PeerfactSim.KOM.
 *
 * PeerfactSim.KOM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * PeerfactSim.KOM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PeerfactSim.KOM.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// ported to C++ with appropriate modifications for Marlin Protocol

#include <algorithm>
#include <limits>

#include "Subnet.h"
#include "Bandwidth.h"
#include "BandwidthModels/TransferProgress.h"
#include "BandwidthModels/gnp/AbstractGnpNetBandwidthManager.h"
#include "BandwidthModels/gnp/GnpNetBandwidthAllocation.h"
#include "BandwidthModels/gnp/GnpNetBandwidthManager.h"
#include "LatencyModels/GnpLatencyModel.h"
#include "NetworkLayer/IPv4Message.h"
#include "NetworkLayer/NetworkMessage.h"
#include "NetworkLayer/NetworkLayer.h"
#include "TransportLayer/L4Message.h"
#include "TransportLayer/L4Protocol.h"
#include "../EventManagement/Event/EventTypes/MessageToNodeEvent.h"
#include "../Network/Messages/Message.h"
#include "../Network/Messages/SubnetMessage.h"
#include "../Network/Network.h"
#include "../Network/Node/Node.h"

Subnet::Subnet(Network& _network) : network(_network), lastMsgId(0), latencyModel(std::make_shared<GnpLatencyModel>()),
									nextRescheduleTime(-1), bandwidthManager(std::make_shared<GnpNetBandwidthManager>(_network)) {}

std::shared_ptr<AbstractGnpNetBandwidthManager> Subnet::getBandwidthManager() {
	return bandwidthManager;
}

bool Subnet::shouldDropMsg(std::shared_ptr<NetworkMessage> msg) {
	double packetLossProbability = latencyModel->getUDPErrorProbability(std::static_pointer_cast<IPv4Message>(msg));

	double randomNum = network.getRandomDouble();

	if(randomNum < packetLossProbability) {
		return true;
	}

	return false;
}

uint64_t Subnet::setMessageId(std::shared_ptr<NetworkMessage> msg) {
	int msgId = msg->getPayload()->getMessageId();

	if(msgId == -1) {
		msgId = lastMsgId++;
		msg->getPayload()->setMessageId(msgId);
	}

	return msgId;
}

void Subnet::send(std::shared_ptr<NetworkMessage> msg, uint64_t _currentTick, std::vector<std::shared_ptr<Event>>& _newEvents) {
	NodeId senderId = msg->getSender();
	NodeId receiverId = msg->getReceiver();
	L4ProtocolType l4Protocol = msg->getPayload()->getL4Protocol().l4ProtocolType;

	if(senderId == receiverId) {
		return;
	}

	if(l4Protocol == L4ProtocolType::UDP) {
		if(shouldDropMsg(msg)) {
			return;
		}
	}

	int msgId = setMessageId(msg);

	std::shared_ptr<Node> sender = network.getNode(senderId);
	std::shared_ptr<Node> receiver = network.getNode(receiverId);

	if(msg->getNumFragments() == 1) {
		uint64_t propagationTicks = latencyModel->getPropagationDelay(senderId, receiverId);
		uint64_t transmissionTicks = latencyModel->getTransmissionDelay(msg->getSize(), std::min(sender->getNetworkLayer()->getMaxBandwidth()->getUpBW(),
																								receiver->getNetworkLayer()->getMaxBandwidth()->getDownBW()));
		uint64_t transmissionEndTick = std::max(_currentTick, sender->getNetworkLayer()->getNextFreeSendingTime()) + transmissionTicks;
		sender->getNetworkLayer()->setNextFreeSendingTime(transmissionEndTick);

		uint64_t ticksBeforeReception = (sender->getNetworkLayer()->getNextFreeSendingTime() > _currentTick
											? sender->getNetworkLayer()->getNextFreeSendingTime() - _currentTick
											: 0)
										+ transmissionTicks + propagationTicks;

		std::shared_ptr<TransferProgress> transferProgress(new TransferProgress(msg, 0, std::numeric_limits<std::size_t>::max(), _currentTick));

		_newEvents.push_back(std::make_shared<MessageToNodeEvent>(
			MessageToNodeEvent(std::shared_ptr<Message>(new SubnetMessage(SubnetMessageType::MESSAGE_RECVD, transferProgress)),
							   -1, -1, ticksBeforeReception)
		));
	}
	else {
		double maxBandwidthRequired = sender->getNetworkLayer()->getMaxBandwidth()->getUpBW();
		if(l4Protocol == L4ProtocolType::TCP) {
			double tcpThroughput = latencyModel->getTCPThroughput(senderId, receiverId);
			maxBandwidthRequired = std::min(maxBandwidthRequired, tcpThroughput);
		}
		std::shared_ptr<GnpNetBandwidthAllocation> ba = bandwidthManager->addConnection(senderId, receiverId, maxBandwidthRequired);
		std::shared_ptr<TransferProgress> transferProgress(new TransferProgress(msg, msg->getSize(), 0, _currentTick));
		connectionsToTransfersMap[ba].insert(transferProgress);
		messageIdsToTransfersMap[msgId] = transferProgress;

		if(nextRescheduleTime < _currentTick + 1) {
			nextRescheduleTime = _currentTick + 1;
			_newEvents.push_back(std::make_shared<MessageToNodeEvent>(
				MessageToNodeEvent(std::shared_ptr<Message>(new SubnetMessage(SubnetMessageType::BANDWIDTH_REALLOC)),
								   -1, -1, 1)
			));
		}
	}
}

void Subnet::cancelTransmission(int _msgId, uint64_t _currentTick, std::vector<std::shared_ptr<Event>>& _newEvents) {
	std::shared_ptr<TransferProgress> tp = messageIdsToTransfersMap[_msgId];
	std::shared_ptr<NetworkMessage> msg = tp->getMessage();
	NodeId senderId = msg->getSender();
	NodeId receiverId = msg->getReceiver();
	std::shared_ptr<Node> sender = network.getNode(senderId);

	double maxBandwidthRequired = sender->getNetworkLayer()->getMaxBandwidth()->getUpBW();

	if(msg->getPayload()->getL4Protocol().l4ProtocolType == L4ProtocolType::TCP) {
		double tcpThroughput = latencyModel->getTCPThroughput(senderId, receiverId);
		maxBandwidthRequired = std::min(maxBandwidthRequired, tcpThroughput);
	}

	bandwidthManager->removeConnection(senderId, receiverId, maxBandwidthRequired);

	messageIdsToTransfersMap.erase(_msgId);
	cancelledTransfers.insert(tp);

	if(nextRescheduleTime < _currentTick + 1) {
		nextRescheduleTime = _currentTick + 1;
		_newEvents.push_back(std::make_shared<MessageToNodeEvent>(
			MessageToNodeEvent(std::shared_ptr<Message>(new SubnetMessage(SubnetMessageType::BANDWIDTH_REALLOC)),
							   -1, -1, 1)
		));
	}
}

void Subnet::onDisconnect(NodeId _nodeId, uint64_t _currentTick, std::vector<std::shared_ptr<Event>>& _newEvents) {
	for(auto ba: bandwidthManager->removeConnections(_nodeId)) {
		std::set<std::shared_ptr<TransferProgress>> transfers = connectionsToTransfersMap[ba];
		connectionsToTransfersMap.erase(ba);
		cancelledTransfers.insert(transfers.begin(), transfers.end());
		if(!transfers.empty()) {
			for(auto tp: transfers) {
				messageIdsToTransfersMap.erase(tp->getMessage()->getPayload()->getMessageId());
			}
		}
		if(nextRescheduleTime < _currentTick + 1) {
			nextRescheduleTime = _currentTick + 1;
			_newEvents.push_back(std::make_shared<MessageToNodeEvent>(
				MessageToNodeEvent(std::shared_ptr<Message>(new SubnetMessage(SubnetMessageType::BANDWIDTH_REALLOC)),
								   -1, -1, 1)
			));
		}
	}
}

void Subnet::onMessageReceived(std::shared_ptr<TransferProgress> _tp, uint64_t _currentTick, std::vector<std::shared_ptr<Event>>& _newEvents) {
	std::shared_ptr<NetworkMessage> msg = _tp->getMessage();
	NodeId senderId = msg->getSender();
	NodeId receiverId = msg->getReceiver();
	std::shared_ptr<Node> sender = network.getNode(senderId);
	std::shared_ptr<Node> receiver = network.getNode(receiverId);

	if(msg->getNumFragments() == 1) {
		// receiver.addToReceiveQueue(msg);
	}
	else {
		if(_tp->obsolete || (cancelledTransfers.find(_tp) != cancelledTransfers.end())) {
			cancelledTransfers.erase(_tp);
			return;
		}
		else {
			// receiver.receive(msg);
			if(nextRescheduleTime < _currentTick + 1) {
				nextRescheduleTime = _currentTick + 1;
				_newEvents.push_back(std::make_shared<MessageToNodeEvent>(
					MessageToNodeEvent(std::shared_ptr<Message>(new SubnetMessage(SubnetMessageType::BANDWIDTH_REALLOC)),
									   -1, -1, 1)
				));
			}
		}

		double maxBandwidthRequired = sender->getNetworkLayer()->getMaxBandwidth()->getUpBW();
		L4ProtocolType l4Protocol = msg->getPayload()->getL4Protocol().l4ProtocolType;
		if(l4Protocol == L4ProtocolType::TCP) {
			double tcpThroughput = latencyModel->getTCPThroughput(senderId, receiverId);
			maxBandwidthRequired = std::min(maxBandwidthRequired, tcpThroughput);
		}

		std::shared_ptr<GnpNetBandwidthAllocation> ba = bandwidthManager->removeConnection(senderId, receiverId, maxBandwidthRequired);

		if(connectionsToTransfersMap.find(ba) != connectionsToTransfersMap.end()) {
			if(connectionsToTransfersMap[ba].size() <= 1) {
				connectionsToTransfersMap.erase(ba);
			}
			else {
				connectionsToTransfersMap[ba].erase(_tp);
			}
		}

		messageIdsToTransfersMap.erase(msg->getPayload()->getMessageId());
	}
}

void Subnet::onBandwidthReallocation(uint64_t _currentTick, std::vector<std::shared_ptr<Event>>& _newEvents) {
	bandwidthManager->allocateBandwidth();
	for(auto ba: bandwidthManager->getChangedAllocations()) {
		rescheduleTransfers(ba, _currentTick, _newEvents);
	}
}

void Subnet::rescheduleTransfers(std::shared_ptr<GnpNetBandwidthAllocation> _ba, uint64_t _currentTick, std::vector<std::shared_ptr<Event>>& _newEvents) {
	std::set<std::shared_ptr<TransferProgress>> transfers = connectionsToTransfersMap[_ba];
	if(transfers.empty()) return;

	std::set<std::shared_ptr<TransferProgress>> updatedTransfers;

	NodeId senderId = _ba->getSender();
	NodeId receiverId = _ba->getReceiver();
	std::shared_ptr<Node> sender = network.getNode(senderId);
	std::shared_ptr<Node> receiver = network.getNode(receiverId);

	double remainingBandwidth = _ba->getAllocatedBandwidth();
	int remainingTransfers = transfers.size();

	for(std::shared_ptr<TransferProgress> tp: transfers) {
		double remainingBytes = tp->getRemainingBytes(_currentTick);
		double bandwidth = remainingBandwidth/remainingTransfers;

		std::shared_ptr<NetworkMessage> msg = tp->getMessage();

		if(msg->getPayload()->getL4Protocol().l4ProtocolType == L4ProtocolType::TCP) {
			double tcpThroughput = latencyModel->getTCPThroughput(senderId, receiverId);
			bandwidth = std::min(bandwidth, tcpThroughput);
		}

		remainingBandwidth -= bandwidth;
		remainingTransfers--;

		uint64_t propagationTicks = latencyModel->getPropagationDelay(senderId, receiverId);
		uint64_t transmissionTicks = latencyModel->getTransmissionDelay(remainingBytes, bandwidth);
		uint64_t ticksBeforeReception = transmissionTicks + propagationTicks;

		std::shared_ptr<TransferProgress> transferProgress(new TransferProgress(msg, bandwidth, remainingBytes, _currentTick));

		_newEvents.push_back(std::make_shared<MessageToNodeEvent>(
			MessageToNodeEvent(std::shared_ptr<Message>(new SubnetMessage(SubnetMessageType::MESSAGE_RECVD, transferProgress)),
							   -1, -1, ticksBeforeReception)
		));

		updatedTransfers.insert(transferProgress);

		int msgId = msg->getPayload()->getMessageId();
		messageIdsToTransfersMap[msgId] = transferProgress;

		transferProgress->firstSchedule = false;
		if(!tp->firstSchedule) {
			tp->obsolete = true;
		}
	}

	connectionsToTransfersMap[_ba] = updatedTransfers;
}
