#include "GnpLatencyModelTest.h"
#include "../core/Networking/Subnet.h"
#include "../core/Networking/LatencyModels/PingER.h"
#include "../core/Networking/LatencyModels/LinkMetrics.h"

GnpLatencyModelTest::GnpLatencyModelTest() : network(std::make_shared<Network>()) {}

std::shared_ptr<GnpLatencyModel> GnpLatencyModelTest::populatePingER() {
	std::unordered_map<std::string, std::unordered_map<std::string, LinkMetrics>> linkMetrics;
	linkMetrics["DE"]["DE"] = LinkMetrics(10, 12.5, 1);
	linkMetrics["US"]["DE"] = LinkMetrics(200, 250, 2);
	linkMetrics["JP"]["JP"] = LinkMetrics(20, 25, 3);
	linkMetrics["EU"]["JP"] = LinkMetrics(300, 375, 4);
	linkMetrics["US"]["CA"] = LinkMetrics(30, 30, 5);
	linkMetrics["JP"]["CA"] = LinkMetrics(250, 275, 6);
	linkMetrics["BR"]["BR"] = LinkMetrics(40, 45, 7);
	linkMetrics["CA"]["BR"] = LinkMetrics(100, 120, 8);

	std::shared_ptr<GnpLatencyModel> gnpLatencyModel(new GnpLatencyModel(*network));
	gnpLatencyModel->getPingER().setLinkMetrics(linkMetrics);

	return gnpLatencyModel;
}

void GnpLatencyModelTest::testPingErPropagationDelayGnp(std::shared_ptr<GnpLatencyModel> _pingER) {
	assert(fabs(_pingER->getPropagationDelay(0, 0) - 15) <= 0.0001);
}

int GnpLatencyModelTest::test() {
	std::shared_ptr<GnpLatencyModel> pingER = populatePingER();
	std::shared_ptr<Subnet> subnet(new Subnet(*network, pingER));

	std::shared_ptr<Node> sender0(new Node(0, 0, 10, subnet, "US"));
	std::shared_ptr<Node> sender1(new Node(1, 0, 10, subnet, "CA"));
	std::shared_ptr<Node> sender2(new Node(2, 0, 80, subnet, "JP"));
	std::shared_ptr<Node> sender3(new Node(3, 40, 0, subnet, "CN"));
	std::shared_ptr<Node> sender4(new Node(4, 30, 0, subnet, "DE"));
	std::shared_ptr<Node> sender5(new Node(5, 10, 0, subnet, "BR"));

	network->addNode(sender0);
	network->addNode(sender1);
	network->addNode(sender2);
	network->addNode(sender3);
	network->addNode(sender4);
	network->addNode(sender5);

	testPingErPropagationDelayGnp(pingER);

	return 0;
}
