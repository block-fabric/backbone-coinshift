Public eCash betanet, Coinshift (slot 255), fetched 2026-10-22:

- `blocks.bin`: blocks 0..2261 from seed.beta.ecash.eu.com:4255 (204.168.254.113, peer version
  0.14.8) with `coinshift-fetch-blocks`; records `u32 height, u32 len, bincode(ResponseMessage::Block)`
  as received.
- `twpd255.json`: `GetTwoWayPegData {sidechainId: 255, endBlockHash: 0000…f50d83 (main 973,923)}`
  from https://enforcer.beta.ecash.ninja (Connect JSON): every mainchain block with a slot-255 BMM
  commitment or event (2,265 blocks, 6 deposits, no bundle events).
- `oracle_digests.txt`: `cs_oracle replay` (THEIR State, coinshift-rs v0.14.8) digest after each block; `coinshift-replay digests` prints the same 2,262 lines.
