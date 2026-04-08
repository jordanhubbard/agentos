//! AgentNet - capability-native networking
//! 
//! Every network operation requires an explicit capability.
//! No ambient network access. Zero-trust by architecture.

use alloc::string::String;

/// A network endpoint
#[derive(Debug, Clone)]
pub struct NetEndpoint {
    pub protocol: NetProtocol,
    pub addr: NetAddr,
    pub state: EndpointState,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NetProtocol {
    AgentMesh,  // intra-agentOS
    Tcp,
    Udp,
    Quic,
}

#[derive(Debug, Clone)]
pub enum NetAddr {
    AgentId(String),
    IpPort { ip: [u8; 4], port: u16 },
    Ip6Port { ip: [u8; 16], port: u16 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EndpointState {
    Connecting,
    Connected,
    Listening,
    Closed,
}
