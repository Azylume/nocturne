# The open core ends at the protocol; only hosted services are closed

Everything that runs on a user's machine is in this repository and stays AGPL-3.0-or-later (ADR 0012): the engine, the UI, the Machines tab that watches and updates venue machines over LAN or a tailnet, and the connector that links an engine or the UI to a hosted service. The paid part is the hosted side: the cloud Director (venues, calendar, remote alerts), the media server, the flyer generator and the client pipeline. They live in a separate private repository and talk to Nocturne only through the documented protocol.

- **No closed code in the desktop apps.** Nothing proprietary is linked into or loaded by a Nocturne binary, so there is no plugin system for closed extensions and no licence exception for parts of this repository.
- **The connector is open on purpose.** Anyone may write their own server for it; users can read what the app sends off the machine, which matters on a venue computer. What is sold is running the service, not access to it.
- **The protocol is the contract.** A hosted service implements it from its documentation and never links code from this repository. A change the hosted side needs lands here first, as an ordinary protocol change.

We chose this over a private fork of this repository, which needs constant merging and makes one wrong push a leak, and over a private monorepo that exports to this one, where every export is another chance to publish something private.

The cost: a feature that would only make sense as a closed part of the desktop app cannot be sold; it is either open or moves to the hosted side.
