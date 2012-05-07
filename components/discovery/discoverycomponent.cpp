#include "i2-discovery.h"

using namespace icinga;

string DiscoveryComponent::GetName(void) const
{
	return "discoverycomponent";
}

void DiscoveryComponent::Start(void)
{
	m_DiscoveryEndpoint = make_shared<VirtualEndpoint>();

	long isBroker = 0;
	GetConfig()->GetPropertyInteger("broker", &isBroker);
	m_Broker = (isBroker != 0);

	if (IsBroker()) {
		m_DiscoveryEndpoint->RegisterMethodSource("discovery::NewComponent");
		m_DiscoveryEndpoint->RegisterMethodHandler("discovery::RegisterComponent",
			bind_weak(&DiscoveryComponent::RegisterComponentMessageHandler, shared_from_this()));
	}

	m_DiscoveryEndpoint->RegisterMethodSource("discovery::RegisterComponent");
	m_DiscoveryEndpoint->RegisterMethodHandler("discovery::NewComponent",
		bind_weak(&DiscoveryComponent::NewComponentMessageHandler, shared_from_this()));
	m_DiscoveryEndpoint->RegisterMethodHandler("discovery::Welcome",
		bind_weak(&DiscoveryComponent::WelcomeMessageHandler, shared_from_this()));

	GetEndpointManager()->ForeachEndpoint(bind(&DiscoveryComponent::NewEndpointHandler, this, _1));
	GetEndpointManager()->OnNewEndpoint += bind_weak(&DiscoveryComponent::NewEndpointHandler, shared_from_this());

	GetEndpointManager()->RegisterEndpoint(m_DiscoveryEndpoint);

	m_DiscoveryConnectTimer = make_shared<Timer>();
	m_DiscoveryConnectTimer->SetInterval(30);
	m_DiscoveryConnectTimer->OnTimerExpired += bind_weak(&DiscoveryComponent::ReconnectTimerHandler, shared_from_this());
	m_DiscoveryConnectTimer->Start();
}

void DiscoveryComponent::Stop(void)
{
	EndpointManager::Ptr mgr = GetEndpointManager();

	if (mgr)
		mgr->UnregisterEndpoint(m_DiscoveryEndpoint);
}

int DiscoveryComponent::CheckExistingEndpoint(Endpoint::Ptr endpoint, const NewEndpointEventArgs& neea)
{
	if (endpoint == neea.Endpoint)
		return 0;

	if (!neea.Endpoint->IsConnected())
		return 0;

	if (endpoint->GetIdentity() == neea.Endpoint->GetIdentity()) {
		Application::Log("Detected duplicate identity (" + endpoint->GetIdentity() + " - Disconnecting old endpoint.");

		neea.Endpoint->Stop();
		GetEndpointManager()->UnregisterEndpoint(neea.Endpoint);
	}

	return 0;
}

int DiscoveryComponent::NewEndpointHandler(const NewEndpointEventArgs& neea)
{
	neea.Endpoint->OnIdentityChanged += bind_weak(&DiscoveryComponent::NewIdentityHandler, shared_from_this());

	if (IsBroker()) {
		/* accept discovery::RegisterComponent messages from any endpoint */
		neea.Endpoint->RegisterMethodSource("discovery::RegisterComponent");
	}

	neea.Endpoint->RegisterMethodSource("discovery::Welcome");

	/* TODO: implement message broker authorisation */
	neea.Endpoint->RegisterMethodSource("discovery::NewComponent");

	/* TODO: register handler to unregister this endpoint when it's closed */

	return 0;
}

int DiscoveryComponent::DiscoverySinkHandler(const NewMethodEventArgs& nmea, ComponentDiscoveryInfo::Ptr info) const
{
	info->SubscribedMethods.insert(nmea.Method);
	return 0;
}

int DiscoveryComponent::DiscoverySourceHandler(const NewMethodEventArgs& nmea, ComponentDiscoveryInfo::Ptr info) const
{
	info->PublishedMethods.insert(nmea.Method);
	return 0;
}

int DiscoveryComponent::DiscoveryEndpointHandler(const NewEndpointEventArgs& neea, ComponentDiscoveryInfo::Ptr info) const
{
	neea.Endpoint->ForeachMethodSink(bind(&DiscoveryComponent::DiscoverySinkHandler, this, _1, info));
	neea.Endpoint->ForeachMethodSource(bind(&DiscoveryComponent::DiscoverySourceHandler, this, _1, info));
	return 0;
}

bool DiscoveryComponent::GetComponentDiscoveryInfo(string component, ComponentDiscoveryInfo::Ptr *info) const
{
	if (component == GetEndpointManager()->GetIdentity()) {
		/* Build fake discovery info for ourselves */
		*info = make_shared<ComponentDiscoveryInfo>();
		GetEndpointManager()->ForeachEndpoint(bind(&DiscoveryComponent::DiscoveryEndpointHandler, this, _1, *info));
		
		(*info)->Node = GetIcingaApplication()->GetNode();
		(*info)->Service = GetIcingaApplication()->GetService();

		return true;
	}

	map<string, ComponentDiscoveryInfo::Ptr>::const_iterator i;

	i = m_Components.find(component);

	if (i == m_Components.end())
		return false;

	*info = i->second;
	return true;
}

bool DiscoveryComponent::IsBroker(void) const
{
	return m_Broker;
}

int DiscoveryComponent::NewIdentityHandler(const EventArgs& ea)
{
	Endpoint::Ptr endpoint = static_pointer_cast<Endpoint>(ea.Source);
	string identity = endpoint->GetIdentity();

	if (!GetIcingaApplication()->IsDebugging()) {
		if (identity == GetEndpointManager()->GetIdentity()) {
			Application::Log("Detected loop-back connection - Disconnecting endpoint.");

			endpoint->Stop();
			GetEndpointManager()->UnregisterEndpoint(endpoint);

			return 0;
		}

		GetEndpointManager()->ForeachEndpoint(bind(&DiscoveryComponent::CheckExistingEndpoint, this, endpoint, _1));
	}

	// we assume the other component _always_ wants
	// discovery::RegisterComponent messages from us
	endpoint->RegisterMethodSink("discovery::RegisterComponent");

	// send a discovery::RegisterComponent message, if the
	// other component is a broker this makes sure
	// the broker knows about our message types
	SendDiscoveryMessage("discovery::RegisterComponent", GetEndpointManager()->GetIdentity(), endpoint);

	map<string, ComponentDiscoveryInfo::Ptr>::iterator i;

	if (IsBroker()) {
		// we assume the other component _always_ wants
		// discovery::NewComponent messages from us
		endpoint->RegisterMethodSink("discovery::NewComponent");

		// send discovery::NewComponent message for ourselves
		SendDiscoveryMessage("discovery::NewComponent", GetEndpointManager()->GetIdentity(), endpoint);

		// send discovery::NewComponent messages for all components
		// we know about
		for (i = m_Components.begin(); i != m_Components.end(); i++) {
			SendDiscoveryMessage("discovery::NewComponent", i->first, endpoint);
		}
	}

	// check if we already know the other component
	i = m_Components.find(endpoint->GetIdentity());

	if (i == m_Components.end()) {
		// we don't know the other component yet, so
		// wait until we get a discovery::NewComponent message
		// from a broker
		return 0;
	}

	FinishDiscoverySetup(endpoint);

	return 0;
}

int DiscoveryComponent::WelcomeMessageHandler(const NewRequestEventArgs& nrea)
{
	Endpoint::Ptr endpoint = nrea.Sender;

	if (endpoint->GetHandshakeCounter() >= 2)
		return 0;

	endpoint->IncrementHandshakeCounter();

	if (endpoint->GetHandshakeCounter() >= 2) {
		EventArgs ea;
		ea.Source = shared_from_this();
		endpoint->OnSessionEstablished(ea);
	}

	return 0;
}

void DiscoveryComponent::FinishDiscoverySetup(Endpoint::Ptr endpoint)
{
	if (endpoint->GetHandshakeCounter() >= 2)
		return;

	// we assume the other component _always_ wants
	// discovery::Welcome messages from us
	endpoint->RegisterMethodSink("discovery::Welcome");
	JsonRpcRequest request;
	request.SetMethod("discovery::Welcome");
	GetEndpointManager()->SendUnicastRequest(m_DiscoveryEndpoint, endpoint, request);

	ComponentDiscoveryInfo::Ptr info;

	if (GetComponentDiscoveryInfo(endpoint->GetIdentity(), &info)) {
		set<string>::iterator i;
		for (i = info->PublishedMethods.begin(); i != info->PublishedMethods.end(); i++)
			endpoint->RegisterMethodSource(*i);

		for (i = info->SubscribedMethods.begin(); i != info->SubscribedMethods.end(); i++)
			endpoint->RegisterMethodSink(*i);
	}

	endpoint->IncrementHandshakeCounter();

	if (endpoint->GetHandshakeCounter() >= 2) {
		EventArgs ea;
		ea.Source = shared_from_this();
		endpoint->OnSessionEstablished(ea);
	}
}

void DiscoveryComponent::SendDiscoveryMessage(string method, string identity, Endpoint::Ptr recipient)
{
	JsonRpcRequest request;
	request.SetMethod(method);
	
	DiscoveryMessage params;
	request.SetParams(params);

	params.SetIdentity(identity);

	Message subscriptions;
	params.SetSubscribes(subscriptions);

	Message publications;
	params.SetProvides(publications);

	ComponentDiscoveryInfo::Ptr info;

	if (!GetComponentDiscoveryInfo(identity, &info))
		return;

	if (!info->Node.empty() && !info->Service.empty()) {
		params.SetNode(info->Node);
		params.SetService(info->Service);
	}

	set<string>::iterator i;
	for (i = info->PublishedMethods.begin(); i != info->PublishedMethods.end(); i++)
		publications.AddUnnamedPropertyString(*i);

	for (i = info->SubscribedMethods.begin(); i != info->SubscribedMethods.end(); i++)
		subscriptions.AddUnnamedPropertyString(*i);

	if (recipient)
		GetEndpointManager()->SendUnicastRequest(m_DiscoveryEndpoint, recipient, request);
	else
		GetEndpointManager()->SendMulticastRequest(m_DiscoveryEndpoint, request);
}

void DiscoveryComponent::ProcessDiscoveryMessage(string identity, DiscoveryMessage message)
{
	ComponentDiscoveryInfo::Ptr info = make_shared<ComponentDiscoveryInfo>();

	message.GetNode(&info->Node);
	message.GetService(&info->Service);

	Message provides;
	if (message.GetProvides(&provides)) {
		DictionaryIterator i;
		for (i = provides.GetDictionary()->Begin(); i != provides.GetDictionary()->End(); i++) {
			info->PublishedMethods.insert(i->second);
		}
	}

	Message subscribes;
	if (message.GetSubscribes(&subscribes)) {
		DictionaryIterator i;
		for (i = subscribes.GetDictionary()->Begin(); i != subscribes.GetDictionary()->End(); i++) {
			info->SubscribedMethods.insert(i->second);
		}
	}

	map<string, ComponentDiscoveryInfo::Ptr>::iterator i;

	i  = m_Components.find(identity);

	if (i != m_Components.end())
		m_Components.erase(i);

	m_Components[identity] = info;

	if (IsBroker())
		SendDiscoveryMessage("discovery::NewComponent", identity, Endpoint::Ptr());

	Endpoint::Ptr endpoint = GetEndpointManager()->GetEndpointByIdentity(identity);
	if (endpoint)
		FinishDiscoverySetup(endpoint);
}

int DiscoveryComponent::NewComponentMessageHandler(const NewRequestEventArgs& nrea)
{
	DiscoveryMessage message;
	nrea.Request.GetParams(&message);

	string identity;
	if (!message.GetIdentity(&identity))
		return 0;

	ProcessDiscoveryMessage(identity, message);
	return 0;
}

int DiscoveryComponent::RegisterComponentMessageHandler(const NewRequestEventArgs& nrea)
{
	DiscoveryMessage message;
	nrea.Request.GetParams(&message);
	ProcessDiscoveryMessage(nrea.Sender->GetIdentity(), message);
	return 0;
}

int DiscoveryComponent::ReconnectTimerHandler(const TimerEventArgs& tea)
{
	EndpointManager::Ptr endpointManager = GetEndpointManager();

	map<string, ComponentDiscoveryInfo::Ptr>::iterator i;
	for (i = m_Components.begin(); i != m_Components.end(); i++) {
		Endpoint::Ptr endpoint = endpointManager->GetEndpointByIdentity(i->first);
		if (endpoint)
			continue;

		ComponentDiscoveryInfo::Ptr info = i->second;
		endpointManager->AddConnection(info->Node, info->Service);
	}

	return 0;
}

EXPORT_COMPONENT(DiscoveryComponent);
